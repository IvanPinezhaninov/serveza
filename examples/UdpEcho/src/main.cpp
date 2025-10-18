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
#include <iostream>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

#include <boost/asio.hpp>

#include <serveza/serveza.h>

namespace net = boost::asio;
namespace sys = boost::system;
using udp = net::ip::udp;

namespace {

constexpr unsigned short port = 54321;
constexpr std::size_t workerCount = 4;
std::mutex logMutex;

template<typename Handler>
class UdpEchoOperation final : public std::enable_shared_from_this<UdpEchoOperation<Handler>> {
public:
  UdpEchoOperation(udp::socket& socket, Handler handler)
    : m_socket{socket}
    , m_handler{std::move(handler)}
  {}

  void run()
  {
    receive();
  }

private:
  void receive()
  {
    std::shared_ptr<UdpEchoOperation> self = this->shared_from_this();
    m_socket.async_receive_from(net::buffer(m_data), m_remote, [self](sys::error_code ec, std::size_t size) {
      if (ec == net::error::operation_aborted) {
        self->finish();
        return;
      }
      if (ec) {
        {
          std::lock_guard<std::mutex> lock{logMutex};
          std::cerr << "Receive failed: " << ec.message() << std::endl;
        }
        self->receive();
        return;
      }
      {
        std::lock_guard<std::mutex> lock{logMutex};
        std::cout << "Received " << size << " bytes from " << self->m_remote << std::endl;
      }
      self->send(size);
    });
  }

  void send(std::size_t size)
  {
    std::shared_ptr<UdpEchoOperation> self = this->shared_from_this();
    m_socket.async_send_to(net::buffer(m_data.data(), size), m_remote, [self](sys::error_code ec, std::size_t) {
      if (ec == net::error::operation_aborted) {
        self->finish();
        return;
      }
      if (ec) {
        std::lock_guard<std::mutex> lock{logMutex};
        std::cerr << "Send to " << self->m_remote << " failed: " << ec.message() << std::endl;
      }
      self->receive();
    });
  }

  void finish()
  {
    Handler handler = std::move(m_handler);
    handler(sys::error_code{});
  }

  udp::socket& m_socket;
  Handler m_handler;
  std::array<char, 4096> m_data{};
  udp::endpoint m_remote;
};

class UdpEcho final {
public:
  template<typename Handler>
  void operator()(serveza::bound_socket_context<udp>& ctx, Handler handler) const
  {
    using Operation = UdpEchoOperation<std::decay_t<Handler>>;
    std::shared_ptr<Operation> operation = std::make_shared<Operation>(ctx.socket(), std::move(handler));
    operation->run();
  }
};

} // namespace

int main()
{
  net::thread_pool pool{workerCount};
  serveza::server server{pool.get_executor()};
  std::shared_ptr<serveza::listener> listener =
      server.bind<udp>({net::ip::address_v4::loopback(), port}, [] { return serveza::callback_session{UdpEcho{}}; });

  net::strand<net::thread_pool::executor_type> control = net::make_strand(pool);
  net::signal_set signals{control, SIGINT, SIGTERM};
  signals.async_wait([&server](sys::error_code ec, int) {
    if (ec) return;
    {
      std::lock_guard<std::mutex> lock{logMutex};
      std::cout << "Stopping Serveza UDP echo example." << std::endl;
    }
    server.request_stop();
  });

  int result = EXIT_SUCCESS;
  server.async_wait(net::bind_executor(control, [&](sys::error_code ec) {
    if (ec) {
      std::lock_guard<std::mutex> lock{logMutex};
      std::cerr << "UDP listener stopped with error: " << ec.message() << std::endl;
      result = EXIT_FAILURE;
    }
    signals.cancel();
  }));
  server.async_start(net::bind_executor(control, [listener, &server, &result](sys::error_code ec) {
    if (ec) {
      {
        std::lock_guard<std::mutex> lock{logMutex};
        std::cerr << "UDP listener start failed: " << ec.message() << std::endl;
      }
      result = EXIT_FAILURE;
      server.request_stop();
      return;
    }
    std::lock_guard<std::mutex> lock{logMutex};
    std::cout << "Serveza UDP echo is listening on " << listener->endpoint() << std::endl;
    std::cout << "Try: echo hello | socat - UDP4:localhost:" << port << std::endl;
  }));

  pool.join();
  return result;
}
