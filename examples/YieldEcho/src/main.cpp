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

#include <boost/asio.hpp>

#include <serveza/serveza.h>
#include <serveza/yield_session.h>

namespace net = boost::asio;
namespace sys = boost::system;
using tcp = net::ip::tcp;

namespace {

constexpr std::size_t workerCount = 4;
constexpr unsigned short port = 54321;
constexpr std::string_view greeting = "Hello! I'm an echo server.\n";
std::mutex logMutex;

void logConnection(const char* action, const serveza::connection_info& info, const sys::error_code& ec = {})
{
  std::lock_guard<std::mutex> lock{logMutex};
  std::cout << "Client " << info.id << ' ' << action;
  if (!info.remote_endpoint.empty()) std::cout << " (" << info.remote_endpoint << ')';
  if (ec && ec != net::error::eof && ec != net::error::connection_reset && ec != net::error::operation_aborted)
    std::cout << ": " << ec.message();
  std::cout << std::endl;
}

class YieldEcho final {
public:
  void operator()(serveza::session_context<tcp>& ctx, net::yield_context yield) const
  {
    logConnection("connected", ctx.info());
    std::array<char, 4096> buffer{};
    sys::error_code ec;
    net::async_write(ctx.socket(), net::buffer(greeting.data(), greeting.size()), yield[ec]);
    for (;;) {
      if (ec) break;
      const std::size_t size = ctx.socket().async_read_some(net::buffer(buffer), yield[ec]);
      if (ec) break;
      net::async_write(ctx.socket(), net::buffer(buffer.data(), size), yield[ec]);
      if (ec) break;
    }
    logConnection("disconnected", ctx.info(), ec);
  }
};

} // namespace

int main()
{
  net::thread_pool pool{workerCount};
  serveza::server server{pool.get_executor()};
  std::shared_ptr<serveza::listener> listener =
      server.listen<tcp>({net::ip::address_v4::loopback(), port}, [] { return serveza::yield_session{YieldEcho{}}; });

  net::strand<net::thread_pool::executor_type> control = net::make_strand(pool);
  net::signal_set signals{control, SIGINT, SIGTERM};
  signals.async_wait([&server](sys::error_code ec, int) {
    if (ec) return;
    {
      std::lock_guard<std::mutex> lock{logMutex};
      std::cout << "Stopping Serveza yield echo example." << std::endl;
    }
    server.request_stop();
  });

  int result = EXIT_SUCCESS;
  net::spawn(
      control,
      [&](net::yield_context yield) {
        sys::error_code ec;
        server.async_start(yield[ec]);
        if (ec) {
          {
            std::lock_guard<std::mutex> lock{logMutex};
            std::cerr << "Listener start failed: " << ec.message() << std::endl;
          }
          result = EXIT_FAILURE;
          server.request_stop();
        } else {
          std::lock_guard<std::mutex> lock{logMutex};
          std::cout << "Serveza yield echo example is running." << std::endl;
          std::cout << "Listening on tcp://" << listener->endpoint() << std::endl;
          std::cout << "I/O worker threads: " << workerCount << std::endl;
          std::cout << "Connect with: socat STDIO TCP4:localhost:" << port << std::endl;
        }
        server.async_wait(yield[ec]);
        if (ec) {
          std::lock_guard<std::mutex> lock{logMutex};
          std::cerr << "Listener stopped with error: " << ec.message() << std::endl;
          result = EXIT_FAILURE;
        }
        {
          std::lock_guard<std::mutex> lock{logMutex};
          std::cout << "Serveza yield echo example stopped." << std::endl;
        }
        signals.cancel();
      },
      net::detached);

  pool.join();
  return result;
}
