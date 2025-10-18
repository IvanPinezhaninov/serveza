/******************************************************************************
**
** Copyright (C) 2026 Ivan Pinezhaninov <ivan.pinezhaninov@gmail.com>
**
** This file is part of the serveza - which can be found at
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

#include <filesystem>
#include <iostream>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <serveza/serveza.hpp>
#include <serveza/tls_cert_utils.h>

#include "config.h"

// To connect to the server use:
// curl -X POST "https://localhost:8443/" -d "Hello World" -H "Content-Type: text/plain" -k

namespace fs = std::filesystem;
namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ip = net::ip;
namespace ssl = net::ssl;
namespace sys = boost::system;

namespace {

void logError(std::exception_ptr err, std::string_view where)
{
  try {
    if (err) std::rethrow_exception(err);
  } catch (const sys::system_error& e) {
    std::cout << "Error in the " << where << ": " << e.code().message() << std::endl;
  } catch (const std::exception& e) {
    std::cout << "Error in the " << where << ": " << e.what() << std::endl;
  }
}

class HttpsEcho final : public serveza::session<ip::tcp> {
public:
  explicit HttpsEcho(ssl::context& sslCtx)
    : m_sslCtx{sslCtx}
  {}

  void operator()(protocol_type::socket socket, net::yield_context yield) override
  {
    beast::ssl_stream<beast::tcp_stream> stream{std::move(socket), m_sslCtx};

    auto endpoint = getEndpoint(beast::get_lowest_layer(stream));
    std::cout << "Client connected: " << endpoint << std::endl;

    try {
      using namespace std::chrono_literals;

      std::chrono::seconds timeout = 10s;

      auto& lowestLayer = boost::beast::get_lowest_layer(stream);
      lowestLayer.expires_after(timeout);
      stream.async_handshake(ssl::stream_base::server, yield);

      bool keepAlive = false;

      while (yield.cancelled() == net::cancellation_type::none) {
        lowestLayer.expires_after(timeout);

        beast::flat_buffer buffer;
        http::request_parser<http::string_body> parser;
        http::async_read(stream, buffer, parser, yield);

        const auto& req = parser.get();
        http::async_write(stream, echoResponse(req), yield);

        if (!req.keep_alive()) break;
        if (keepAlive) continue;
        keepAlive = true;
        timeout = 60s;
      }
    } catch (...) {
      close(beast::get_lowest_layer(stream));
      throw;
    }

    close(beast::get_lowest_layer(stream));
  }

private:
  template<typename Stream>
  static typename Stream::endpoint_type getEndpoint(Stream& stream)
  {
    sys::error_code ec;
    return stream.socket().remote_endpoint(ec);
  }

  static http::response<http::string_body> echoResponse(const http::request<http::string_body>& req)
  {
    http::response<http::string_body> res{http::status::ok, req.version()};
    if (auto it = req.find(http::field::content_type); it != req.end()) res.set(http::field::content_type, it->value());
    res.keep_alive(req.keep_alive());
    res.body() = std::move(req.body());
    res.prepare_payload();
    return res;
  }

  template<typename Stream>
  static void close(Stream& stream)
  {
    if (!stream.socket().is_open()) return;
    sys::error_code ec;
    stream.socket().close(ec);
  }

  ssl::context& m_sslCtx;
};

} // namespace

int main()
{
  auto certPath = fs::path{sslDir} / "server.crt";
  auto keyPath = fs::path{sslDir} / "server.key";
  serveza::tls_cert_utils::generate_key_and_cert(certPath, keyPath);

  ssl::context sslCtx{ssl::context::tls_server};
  sslCtx.use_certificate_chain_file(certPath.string());
  sslCtx.use_private_key_file(keyPath.string(), ssl::context::pem);

  serveza::server server{std::thread::hardware_concurrency()};

  net::ip::tcp::endpoint ep{ip::tcp::v4(), 8443};
  auto listener = server.make_listener<HttpsEcho>(ep, sslCtx);
  listener->on_state_changed([](serveza::lifecycle_state state) {
    using state_type = serveza::lifecycle_state;
    if (state == state_type::started || state == state_type::stopped)
      std::cout << "Listener " << (state == state_type::started ? "started" : "stopped") << std::endl;
  });
  listener->on_error([](std::exception_ptr err, std::string_view where) { logError(err, where); });
  listener->start();

  net::signal_set signals{server.get_executor(), SIGINT, SIGTERM};
  signals.async_wait([&server, listener](const sys::error_code& ec, int) {
    if (ec) return;
    server.stop();
  });

  server.on_state_changed([](serveza::lifecycle_state state) {
    using state_type = serveza::lifecycle_state;
    if (state == state_type::started || state == state_type::stopped)
      std::cout << "Server " << (state == state_type::started ? "started" : "stopped") << std::endl;
  });

  try {
    server.start();
    return EXIT_SUCCESS;
  } catch (const std::exception& e) {
    logError(std::current_exception(), "server::start()");
    return EXIT_FAILURE;
  }
}
