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
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <utility>

#include <boost/asio.hpp>

#include <serveza/serveza.h>

namespace net = boost::asio;
namespace sys = boost::system;
using tcp = net::ip::tcp;

namespace {

constexpr std::size_t workerCount = 4;
constexpr unsigned short port = 54321;
constexpr std::string_view greeting = "Hello! I'm an echo server.\n";
std::mutex logMutex;

void logConnection(const char* action, const serveza::connection_info& info)
{
  std::lock_guard<std::mutex> lock{logMutex};
  std::cout << "Client " << info.id << ' ' << action;
  if (!info.remote_endpoint.empty()) std::cout << " (" << info.remote_endpoint << ')';
  std::cout << std::endl;
}

template<typename Handler>
class CallbackEchoConnection final : public std::enable_shared_from_this<CallbackEchoConnection<Handler>> {
public:
  CallbackEchoConnection(tcp::socket& socket, serveza::connection_info info, Handler handler)
    : m_socket{socket}
    , m_info{std::move(info)}
    , m_handler{std::move(handler)}
  {}

  void run()
  {
    logConnection("connected", m_info);
    writeGreeting();
  }

private:
  void writeGreeting()
  {
    std::shared_ptr<CallbackEchoConnection> self = this->shared_from_this();
    net::async_write(m_socket, net::buffer(greeting.data(), greeting.size()), [self](sys::error_code ec, std::size_t) {
      if (ec) {
        self->complete(ec);
        return;
      }
      self->read();
    });
  }

  void read()
  {
    std::shared_ptr<CallbackEchoConnection> self = this->shared_from_this();
    m_socket.async_read_some(net::buffer(m_buffer), [self](sys::error_code ec, std::size_t size) {
      if (ec) {
        self->complete(ec);
        return;
      }
      self->write(size);
    });
  }

  void write(std::size_t size)
  {
    std::shared_ptr<CallbackEchoConnection> self = this->shared_from_this();
    net::async_write(m_socket, net::buffer(m_buffer.data(), size), [self](sys::error_code ec, std::size_t) {
      if (ec) {
        self->complete(ec);
        return;
      }
      self->read();
    });
  }

  void complete(const sys::error_code& ec)
  {
    {
      std::lock_guard<std::mutex> lock{logMutex};
      std::cout << "Client " << m_info.id << " disconnected";
      if (!m_info.remote_endpoint.empty()) std::cout << " (" << m_info.remote_endpoint << ')';
      if (ec != net::error::eof && ec != net::error::connection_reset && ec != net::error::operation_aborted)
        std::cout << ": " << ec.message();
      std::cout << std::endl;
    }
    m_handler(sys::error_code{});
  }

  tcp::socket& m_socket;
  serveza::connection_info m_info;
  Handler m_handler;
  std::array<char, 4096> m_buffer{};
};

class CallbackEcho final {
public:
  template<typename Handler>
  void operator()(serveza::session_context<tcp>& ctx, Handler handler) const
  {
    using Connection = CallbackEchoConnection<std::decay_t<Handler>>;
    std::shared_ptr<Connection> connection = std::make_shared<Connection>(ctx.socket(), ctx.info(), std::move(handler));
    connection->run();
  }
};

} // namespace

int main()
{
  net::thread_pool pool{workerCount};
  serveza::server server{pool.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), port}, [] { return serveza::callback_session{CallbackEcho{}}; });

  net::strand<net::thread_pool::executor_type> control = net::make_strand(pool);
  net::signal_set signals{control, SIGINT, SIGTERM};
  signals.async_wait([&server](sys::error_code ec, int) {
    if (ec) return;
    {
      std::lock_guard<std::mutex> lock{logMutex};
      std::cout << "Stopping Serveza callback echo example." << std::endl;
    }
    server.request_stop();
  });

  int result = EXIT_SUCCESS;
  server.async_wait(net::bind_executor(control, [&](sys::error_code ec) {
    if (ec) {
      std::lock_guard<std::mutex> lock{logMutex};
      std::cerr << "Listener stopped with error: " << ec.message() << std::endl;
      result = EXIT_FAILURE;
    }
    {
      std::lock_guard<std::mutex> lock{logMutex};
      std::cout << "Serveza callback echo example stopped." << std::endl;
    }
    signals.cancel();
  }));
  server.async_start(net::bind_executor(control, [listener, &server, &result](sys::error_code ec) {
    if (ec) {
      {
        std::lock_guard<std::mutex> lock{logMutex};
        std::cerr << "Listener start failed: " << ec.message() << std::endl;
      }
      result = EXIT_FAILURE;
      server.request_stop();
      return;
    }
    std::lock_guard<std::mutex> lock{logMutex};
    std::cout << "Serveza callback echo example is running." << std::endl;
    std::cout << "Listening on tcp://" << listener->endpoint() << std::endl;
    std::cout << "I/O worker threads: " << workerCount << std::endl;
    std::cout << "Connect with: socat STDIO TCP4:localhost:" << port << std::endl;
  }));

  pool.join();
  return result;
}
