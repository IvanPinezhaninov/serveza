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

#include <array>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <serveza/serveza.h>

namespace net = boost::asio;
namespace ssl = net::ssl;
namespace sys = boost::system;
using tcp = net::ip::tcp;

namespace {

constexpr std::size_t workerCount = 4;
constexpr unsigned short port = 54321;
constexpr std::string_view greeting = "Hello! I'm a TLS echo server.\n";
std::mutex logMutex;

template<typename Handler>
class TlsEchoConnection final : public std::enable_shared_from_this<TlsEchoConnection<Handler>> {
public:
  TlsEchoConnection(tcp::socket& socket, ssl::context& tlsCtx, serveza::connection_info info, Handler handler)
    : m_stream{socket, tlsCtx}
    , m_info{std::move(info)}
    , m_handler{std::move(handler)}
  {}

  void run()
  {
    {
      std::lock_guard<std::mutex> lock{logMutex};
      std::cout << "TLS client " << m_info.id << " connected (" << m_info.remote_endpoint << ')' << std::endl;
    }
    std::shared_ptr<TlsEchoConnection> self = this->shared_from_this();
    m_stream.async_handshake(ssl::stream_base::server, [self](sys::error_code ec) {
      if (ec) return self->complete(ec);
      self->writeGreeting();
    });
  }

private:
  void writeGreeting()
  {
    std::shared_ptr<TlsEchoConnection> self = this->shared_from_this();
    net::async_write(m_stream, net::buffer(greeting.data(), greeting.size()), [self](sys::error_code ec, std::size_t) {
      if (ec) return self->complete(ec);
      self->read();
    });
  }

  void read()
  {
    std::shared_ptr<TlsEchoConnection> self = this->shared_from_this();
    m_stream.async_read_some(net::buffer(m_data), [self](sys::error_code ec, std::size_t size) {
      if (ec == net::error::eof) return self->shutdown();
      if (ec) return self->complete(ec);
      self->write(size);
    });
  }

  void write(std::size_t size)
  {
    std::shared_ptr<TlsEchoConnection> self = this->shared_from_this();
    net::async_write(m_stream, net::buffer(m_data.data(), size), [self](sys::error_code ec, std::size_t) {
      if (ec) return self->complete(ec);
      self->read();
    });
  }

  void shutdown()
  {
    std::shared_ptr<TlsEchoConnection> self = this->shared_from_this();
    m_stream.async_shutdown([self](sys::error_code) { self->complete({}); });
  }

  void complete(sys::error_code ec)
  {
    if (m_finished) return;
    m_finished = true;
    {
      std::lock_guard<std::mutex> lock{logMutex};
      std::cout << "TLS client " << m_info.id << " disconnected";
      if (ec && ec != net::error::operation_aborted && ec != ssl::error::stream_truncated)
        std::cout << ": " << ec.message();
      std::cout << std::endl;
    }
    Handler handler = std::move(m_handler);
    if (ec == net::error::operation_aborted || ec == ssl::error::stream_truncated) ec.clear();
    handler(ec);
  }

  ssl::stream<tcp::socket&> m_stream;
  serveza::connection_info m_info;
  Handler m_handler;
  std::array<char, 4096> m_data{};
  bool m_finished{};
};

class TlsEcho final {
public:
  explicit TlsEcho(ssl::context& tlsCtx)
    : m_tlsCtx{tlsCtx}
  {}

  template<typename Handler>
  void operator()(serveza::session_context<tcp>& ctx, Handler handler) const
  {
    using Connection = TlsEchoConnection<std::decay_t<Handler>>;
    std::make_shared<Connection>(ctx.socket(), m_tlsCtx, ctx.info(), std::move(handler))->run();
  }

private:
  ssl::context& m_tlsCtx;
};

} // namespace

int main(int argc, char** argv)
{
  try {
    const char* certificate = argc > 1 ? argv[1] : SERVEZA_TLS_CERTIFICATE_PATH;
    const char* privateKey = argc > 2 ? argv[2] : SERVEZA_TLS_PRIVATE_KEY_PATH;
    ssl::context tlsCtx{ssl::context::tls_server};
    tlsCtx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3 |
                       ssl::context::single_dh_use);
    tlsCtx.use_certificate_chain_file(certificate);
    tlsCtx.use_private_key_file(privateKey, ssl::context::pem);

    net::thread_pool pool{workerCount};
    serveza::server server{pool.get_executor()};
    std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
        {net::ip::address_v4::loopback(), port}, [&tlsCtx] { return serveza::callback_session{TlsEcho{tlsCtx}}; });

    net::strand<net::thread_pool::executor_type> control = net::make_strand(pool);
    net::signal_set signals{control, SIGINT, SIGTERM};
    signals.async_wait([&server](sys::error_code ec, int) {
      if (ec) return;
      std::lock_guard<std::mutex> lock{logMutex};
      std::cout << "Stopping Serveza TLS echo example." << std::endl;
      server.request_stop();
    });

    int result = EXIT_SUCCESS;
    server.async_wait(net::bind_executor(control, [&](sys::error_code ec) {
      if (ec) {
        std::lock_guard<std::mutex> lock{logMutex};
        std::cerr << "TLS listener stopped with error: " << ec.message() << std::endl;
        result = EXIT_FAILURE;
      }
      signals.cancel();
    }));
    server.async_start(net::bind_executor(control, [listener, &server, &result](sys::error_code ec) {
      if (ec) {
        {
          std::lock_guard<std::mutex> lock{logMutex};
          std::cerr << "TLS listener start failed: " << ec.message() << std::endl;
        }
        result = EXIT_FAILURE;
        server.request_stop();
        return;
      }
      std::lock_guard<std::mutex> lock{logMutex};
      std::cout << "Serveza TLS echo is listening on " << listener->endpoint() << std::endl;
      std::cout << "I/O worker threads: " << workerCount << std::endl;
      std::cout << "Connect with: openssl s_client -connect localhost:" << port << " -quiet" << std::endl;
    }));

    pool.join();
    return result;
  } catch (const std::exception& error) {
    std::cerr << "TLS example failed: " << error.what() << std::endl;
    return EXIT_FAILURE;
  }
}
