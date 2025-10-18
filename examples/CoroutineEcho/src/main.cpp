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
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <serveza/serveza.h>

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

class AwaitableEcho final {
public:
  net::awaitable<void> operator()(serveza::session_context<tcp>& ctx) const
  {
    logConnection("connected", ctx.info());
    std::array<char, 4096> buffer{};
    sys::error_code ec;
    co_await net::async_write(ctx.socket(), net::buffer(greeting.data(), greeting.size()),
                              net::redirect_error(net::use_awaitable, ec));
    for (;;) {
      if (ec) break;
      const std::size_t size =
          co_await ctx.socket().async_read_some(net::buffer(buffer), net::redirect_error(net::use_awaitable, ec));
      if (ec) break;
      co_await net::async_write(ctx.socket(), net::buffer(buffer.data(), size),
                                net::redirect_error(net::use_awaitable, ec));
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
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), port}, [] { return serveza::awaitable_session{AwaitableEcho{}}; });

  net::strand<net::thread_pool::executor_type> control = net::make_strand(pool);
  net::signal_set signals{control, SIGINT, SIGTERM};
  signals.async_wait([&server](sys::error_code ec, int) {
    if (ec) return;
    {
      std::lock_guard<std::mutex> lock{logMutex};
      std::cout << "Stopping Serveza coroutine echo example." << std::endl;
    }
    server.request_stop();
  });

  int result = EXIT_SUCCESS;
  net::co_spawn(
      control,
      [&]() -> net::awaitable<void> {
        sys::error_code ec;
        co_await server.async_start(net::redirect_error(net::use_awaitable, ec));
        if (ec) {
          {
            std::lock_guard<std::mutex> lock{logMutex};
            std::cerr << "Listener start failed: " << ec.message() << std::endl;
          }
          result = EXIT_FAILURE;
          server.request_stop();
        } else {
          std::lock_guard<std::mutex> lock{logMutex};
          std::cout << "Serveza coroutine echo example is running." << std::endl;
          std::cout << "Listening on tcp://" << listener->endpoint() << std::endl;
          std::cout << "I/O worker threads: " << workerCount << std::endl;
          std::cout << "Connect with: socat STDIO TCP4:localhost:" << port << std::endl;
        }
        co_await server.async_wait(net::redirect_error(net::use_awaitable, ec));
        if (ec) {
          std::lock_guard<std::mutex> lock{logMutex};
          std::cerr << "Listener stopped with error: " << ec.message() << std::endl;
          result = EXIT_FAILURE;
        }
        {
          std::lock_guard<std::mutex> lock{logMutex};
          std::cout << "Serveza coroutine echo example stopped." << std::endl;
        }
        signals.cancel();
      },
      [&](std::exception_ptr ep) {
        if (!ep) return;
        result = EXIT_FAILURE;
        try {
          std::rethrow_exception(std::move(ep));
        } catch (const std::exception& e) {
          std::lock_guard<std::mutex> lock{logMutex};
          std::cerr << "Coroutine failed: " << e.what() << std::endl;
        }
      });

  pool.join();
  return result;
}
