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
#include <boost/asio/ssl.hpp>
#include <serveza/serveza.hpp>
#include <serveza/tls_cert_utils.h>

#include "config.h"

// To connect to the server use:
// socat STDIO OPENSSL:localhost:54321,verify=0

namespace fs = std::filesystem;
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

class TlsEcho final : public serveza::session<ip::tcp> {
public:
  TlsEcho(std::string greetings, ssl::context& sslCtx)
    : m_greetings{std::move(greetings)}
    , m_sslCtx{sslCtx}
  {}

  void operator()(protocol_type::socket socket, net::yield_context yield) override
  {
    ssl::stream<protocol_type::socket> stream{std::move(socket), m_sslCtx};

    auto endpoint = getEndpoint(stream.lowest_layer());
    std::cout << "Client connected: " << endpoint << std::endl;

    try {
      stream.async_handshake(ssl::stream_base::server, yield);

      net::streambuf buffer;
      std::ostream os{&buffer};
      os << m_greetings << '\n';
      net::async_write(stream, buffer, yield);

      while (yield.cancelled() == net::cancellation_type::none) {
        net::async_read_until(stream, buffer, "\n", yield);
        net::async_write(stream, buffer, yield);
        buffer.consume(buffer.size());
      }
    } catch (...) {
      close(stream.lowest_layer());
      throw;
    }

    close(stream.lowest_layer());
  }

private:
  template<typename Socket>
  static typename Socket::endpoint_type getEndpoint(Socket& socket)
  {
    sys::error_code ec;
    return socket.remote_endpoint(ec);
  }

  template<typename Socket>
  static void close(Socket& socket)
  {
    if (!socket.is_open()) return;
    sys::error_code ec;
    socket.close(ec);
  }

  std::string m_greetings;
  ssl::context& m_sslCtx;
  net::cancellation_signal m_signal;
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

  ip::tcp::endpoint ep{ip::tcp::v4(), 54321};
  auto listener = server.make_listener<TlsEcho>(ep, "Hello! I'm an echo server.", sslCtx);
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
  } catch (const std::exception& ex) {
    logError(std::current_exception(), "server::start()");
    return EXIT_FAILURE;
  }
}
