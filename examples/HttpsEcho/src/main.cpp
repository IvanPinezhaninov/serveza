/******************************************************************************
**
** Copyright (C) 2026 Ivan Pinezhaninov <ivan.pinezhaninov@gmail.com>
**
** This file is part of the serveza which can be found at
** https://github.com/IvanPinezhaninov/serveza/.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
** IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
** DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
** OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
** THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**
******************************************************************************/

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>

#include <serveza/serveza.h>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace sys = boost::system;
using tcp = net::ip::tcp;

namespace {

constexpr std::size_t workerCount = 4;
constexpr unsigned short port = 8443;
constexpr std::size_t headerLimit = 16 * 1024;
constexpr std::size_t bodyLimit = 1024 * 1024;
std::mutex logMutex;

bool isExpectedDisconnect(const sys::error_code& ec)
{
  return ec == http::error::end_of_stream || ec == net::error::eof || ec == net::error::connection_reset ||
         ec == net::error::operation_aborted || ec == ssl::error::stream_truncated;
}

template<typename Handler>
class HttpsEchoConnection final : public std::enable_shared_from_this<HttpsEchoConnection<Handler>> {
public:
  HttpsEchoConnection(tcp::socket& socket, ssl::context& tlsCtx, serveza::connection_info info, Handler handler)
    : m_stream{socket, tlsCtx}
    , m_deadline{socket.get_executor()}
    , m_info{std::move(info)}
    , m_handler{std::move(handler)}
  {}

  void run()
  {
    {
      std::lock_guard<std::mutex> lock{logMutex};
      std::cout << "HTTPS client " << m_info.id << " connected (" << m_info.remote_endpoint << ')' << std::endl;
    }
    armDeadline(std::chrono::seconds{10});
    std::shared_ptr<HttpsEchoConnection> self = this->shared_from_this();
    m_stream.async_handshake(ssl::stream_base::server, [self](sys::error_code ec) {
      self->cancelDeadline();
      if (ec) {
        self->complete(ec);
        return;
      }
      self->read();
    });
  }

private:
  void read()
  {
    armDeadline(m_firstRequest ? std::chrono::seconds{10} : std::chrono::seconds{60});
    m_parser.emplace();
    m_parser->header_limit(headerLimit);
    m_parser->body_limit(bodyLimit);
    std::shared_ptr<HttpsEchoConnection> self = this->shared_from_this();
    http::async_read(m_stream, m_buffer, *m_parser, [self](sys::error_code ec, std::size_t) {
      self->cancelDeadline();
      if (ec) {
        self->complete(ec);
        return;
      }
      self->m_request = self->m_parser->release();
      self->m_parser.reset();
      self->m_firstRequest = false;
      self->write();
    });
  }

  void write()
  {
    {
      std::lock_guard<std::mutex> lock{logMutex};
      std::cout << "HTTPS client " << m_info.id << ": " << m_request.method_string() << ' ' << m_request.target()
                << ", " << m_request.body().size() << " body bytes" << std::endl;
    }
    m_response.version(m_request.version());
    m_response.result(http::status::ok);
    m_response.set(http::field::server, "serveza");
    m_response.set(http::field::content_type, "text/plain");
    m_response.keep_alive(m_request.keep_alive());
    m_response.body() = m_request.body();
    m_response.prepare_payload();

    armDeadline(std::chrono::seconds{10});
    std::shared_ptr<HttpsEchoConnection> self = this->shared_from_this();
    http::async_write(m_stream, m_response, [self](sys::error_code ec, std::size_t) {
      self->cancelDeadline();
      if (ec) {
        self->complete(ec);
        return;
      }
      if (!self->m_response.keep_alive()) {
        self->shutdown();
        return;
      }
      self->m_request = {};
      self->m_response = {};
      self->read();
    });
  }

  void shutdown()
  {
    armDeadline(std::chrono::seconds{10});
    std::shared_ptr<HttpsEchoConnection> self = this->shared_from_this();
    m_stream.async_shutdown([self](sys::error_code ec) {
      self->cancelDeadline();
      self->complete(ec);
    });
  }

  void complete(sys::error_code ec)
  {
    if (m_finished) return;
    m_finished = true;
    cancelDeadline();
    {
      std::lock_guard<std::mutex> lock{logMutex};
      std::cout << "HTTPS client " << m_info.id << " disconnected";
      if (ec && !isExpectedDisconnect(ec)) std::cout << ": " << ec.message();
      std::cout << std::endl;
    }
    if (isExpectedDisconnect(ec)) ec.clear();
    Handler handler = std::move(m_handler);
    handler(ec);
  }

  void armDeadline(std::chrono::seconds timeout)
  {
    m_deadline.expires_after(timeout);
    std::shared_ptr<HttpsEchoConnection> self = this->shared_from_this();
    m_deadline.async_wait([self](sys::error_code ec) {
      if (ec) return;
      sys::error_code ignored;
      self->m_stream.next_layer().cancel(ignored);
    });
  }

  void cancelDeadline()
  {
    try {
      m_deadline.cancel();
    } catch (...) {}
  }

  ssl::stream<tcp::socket&> m_stream;
  net::steady_timer m_deadline;
  serveza::connection_info m_info;
  Handler m_handler;
  beast::flat_buffer m_buffer;
  std::optional<http::request_parser<http::string_body>> m_parser;
  http::request<http::string_body> m_request;
  http::response<http::string_body> m_response;
  bool m_firstRequest{true};
  bool m_finished{};
};

class HttpsEcho final {
public:
  explicit HttpsEcho(ssl::context& tlsCtx)
    : m_tlsCtx{tlsCtx}
  {}

  template<typename Handler>
  void operator()(serveza::session_context<tcp>& ctx, Handler handler) const
  {
    using Connection = HttpsEchoConnection<std::decay_t<Handler>>;
    std::make_shared<Connection>(ctx.socket(), m_tlsCtx, ctx.info(), std::move(handler))->run();
  }

private:
  ssl::context& m_tlsCtx;
};

} // namespace

int main(int argc, char** argv)
{
  try {
    const char* certificate = argc > 1 ? argv[1] : SERVEZA_HTTPS_CERTIFICATE_PATH;
    const char* privateKey = argc > 2 ? argv[2] : SERVEZA_HTTPS_PRIVATE_KEY_PATH;
    ssl::context tlsCtx{ssl::context::tls_server};
    tlsCtx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3 |
                       ssl::context::single_dh_use);
    tlsCtx.use_certificate_chain_file(certificate);
    tlsCtx.use_private_key_file(privateKey, ssl::context::pem);

    net::thread_pool pool{workerCount};
    serveza::server server{pool.get_executor()};
    std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
        {net::ip::address_v4::loopback(), port}, [&tlsCtx] { return serveza::callback_session{HttpsEcho{tlsCtx}}; });

    net::strand<net::thread_pool::executor_type> control = net::make_strand(pool);
    net::signal_set signals{control, SIGINT, SIGTERM};
    signals.async_wait([&server](sys::error_code ec, int) {
      if (ec) return;
      {
        std::lock_guard<std::mutex> lock{logMutex};
        std::cout << "Stopping Serveza HTTPS echo example." << std::endl;
      }
      server.request_stop();
    });

    int result = EXIT_SUCCESS;
    server.async_wait(net::bind_executor(control, [&](sys::error_code ec) {
      if (ec) {
        std::lock_guard<std::mutex> lock{logMutex};
        std::cerr << "HTTPS listener stopped with error: " << ec.message() << std::endl;
        result = EXIT_FAILURE;
      }
      signals.cancel();
    }));
    server.async_start(net::bind_executor(control, [listener, &server, &result](sys::error_code ec) {
      if (ec) {
        {
          std::lock_guard<std::mutex> lock{logMutex};
          std::cerr << "HTTPS listener start failed: " << ec.message() << std::endl;
        }
        result = EXIT_FAILURE;
        server.request_stop();
        return;
      }
      std::lock_guard<std::mutex> lock{logMutex};
      std::cout << "Serveza HTTPS echo is listening on " << listener->endpoint() << std::endl;
      std::cout << "I/O worker threads: " << workerCount << std::endl;
      std::cout << "Try: curl --insecure https://localhost:" << port << "/ --data 'Hello World'" << std::endl;
    }));

    pool.join();
    return result;
  } catch (const std::exception& error) {
    std::cerr << "HTTPS example failed: " << error.what() << std::endl;
    return EXIT_FAILURE;
  }
}
