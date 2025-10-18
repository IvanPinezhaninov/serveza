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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio.hpp>

#include <gtest/gtest.h>

#include <serveza/serveza.h>

namespace net = boost::asio;
namespace sys = boost::system;
using tcp = net::ip::tcp;
using udp = net::ip::udp;

namespace {

unsigned short listenerPort(const std::shared_ptr<serveza::listener>& listener)
{
  const std::string endpoint = listener->endpoint();
  return static_cast<unsigned short>(std::stoul(endpoint.substr(endpoint.rfind(':') + 1)));
}

class RecordingObserver final : public serveza::observer {
public:
  void on_event(const serveza::listener_event& event) override
  {
    events.push_back(event);
    if (received) received(event);
  }

  std::vector<serveza::listener_event> events;
  std::function<void(const serveza::listener_event&)> received;
};

struct DelayedState {
  std::function<void()> started;
  bool completed{};
};

TEST(CoroutineTests, AwaitableSessionAndTokensEchoData)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>({net::ip::address_v4::loopback(), 0}, [] {
    return serveza::awaitable_session{[](serveza::session_context<tcp>& ctx) -> net::awaitable<void> {
      std::array<char, 1024> buffer{};
      sys::error_code ec;
      for (;;) {
        const std::size_t size =
            co_await ctx.socket().async_read_some(net::buffer(buffer), net::redirect_error(net::use_awaitable, ec));
        if (ec) co_return;
        co_await net::async_write(ctx.socket(), net::buffer(buffer.data(), size),
                                  net::redirect_error(net::use_awaitable, ec));
        if (ec) co_return;
      }
    }};
  });
  bool echoed = false;
  bool stopped = false;

  net::co_spawn(
      io,
      [&]() -> net::awaitable<void> {
        sys::error_code ec;
        co_await server.async_start(net::redirect_error(net::use_awaitable, ec));
        EXPECT_FALSE(ec);
        if (ec) co_return;

        tcp::socket client{io};
        co_await client.async_connect({net::ip::address_v4::loopback(), listenerPort(listener)},
                                      net::redirect_error(net::use_awaitable, ec));
        EXPECT_FALSE(ec);
        const std::string input{"serveza"};
        if (!ec) co_await net::async_write(client, net::buffer(input), net::redirect_error(net::use_awaitable, ec));
        std::string output(input.size(), '\0');
        if (!ec) co_await net::async_read(client, net::buffer(output), net::redirect_error(net::use_awaitable, ec));
        EXPECT_FALSE(ec);
        echoed = !ec && output == input;

        server.request_stop();
        co_await server.async_wait(net::redirect_error(net::use_awaitable, ec));
        EXPECT_FALSE(ec);
        stopped = !ec;
      },
      net::detached);

  io.run();

  EXPECT_TRUE(echoed);
  EXPECT_TRUE(stopped);
}

TEST(CoroutineTests, AwaitableExceptionReachesObserver)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<RecordingObserver> observer = std::make_shared<RecordingObserver>();
  [[maybe_unused]] serveza::observer_subscription subscription = server.observe(observer);
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>({net::ip::address_v4::loopback(), 0}, [] {
    return serveza::awaitable_session{[](serveza::session_context<tcp>&) -> net::awaitable<void> {
      throw std::runtime_error{"awaitable failure"};
      co_return;
    }};
  });
  net::steady_timer deadline{io, std::chrono::seconds{2}};
  tcp::socket client{io};

  observer->received = [&](const serveza::listener_event& event) {
    if (event.type != serveza::listener_event_type::session_stopped) return;
    deadline.cancel();
    server.request_stop();
  };
  server.async_wait([](sys::error_code ec) { EXPECT_FALSE(ec); });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client.async_connect({net::ip::address_v4::loopback(), listenerPort(listener)},
                         [](sys::error_code connectEc) { EXPECT_FALSE(connectEc); });
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (!ec) {
      ADD_FAILURE() << "awaitable session did not report its exception";
      server.request_stop();
    }
  });

  io.run();

  const std::vector<serveza::listener_event>::const_iterator failed =
      std::find_if(observer->events.begin(), observer->events.end(), [](const serveza::listener_event& event) {
        return event.type == serveza::listener_event_type::session_stopped && event.exception;
      });
  ASSERT_NE(failed, observer->events.end());
  EXPECT_TRUE(failed->error);
  try {
    std::rethrow_exception(failed->exception);
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "awaitable failure");
  } catch (...) {
    FAIL() << "awaitable exception changed type";
  }
}

TEST(CoroutineTests, ForcedTcpShutdownKeepsSessionStorageUntilCompletion)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<DelayedState> state = std::make_shared<DelayedState>();
  serveza::listener_options options;
  options.shutdown_grace_period = std::chrono::milliseconds{1};
  std::shared_ptr<serveza::listener> listener;
  state->started = [&] { listener->request_stop(); };
  listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0},
      [state] {
        return serveza::awaitable_session{[state](serveza::session_context<tcp>& ctx) -> net::awaitable<void> {
          co_await net::this_coro::reset_cancellation_state(net::disable_cancellation());
          state->started();
          net::steady_timer timer{ctx.get_executor()};
          timer.expires_after(std::chrono::milliseconds{20});
          sys::error_code ec;
          co_await timer.async_wait(net::redirect_error(net::use_awaitable, ec));
          state->completed = !ec;
        }};
      },
      options);
  tcp::socket client{io};

  listener->async_wait([](sys::error_code ec) { EXPECT_FALSE(ec); });
  listener->async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client.async_connect({net::ip::address_v4::loopback(), listenerPort(listener)},
                         [](sys::error_code connectEc) { EXPECT_FALSE(connectEc); });
  });
  io.run();

  EXPECT_TRUE(state->completed);
  EXPECT_EQ(listener->active_sessions(), 0u);
  EXPECT_EQ(listener->status().last_error, make_error_code(sys::errc::timed_out));
}

TEST(CoroutineTests, ForcedUdpShutdownKeepsSessionStorageUntilCompletion)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<DelayedState> state = std::make_shared<DelayedState>();
  serveza::listener_options options;
  options.shutdown_grace_period = std::chrono::milliseconds{1};
  std::shared_ptr<serveza::listener> listener;
  state->started = [&] { listener->request_stop(); };
  listener = server.bind<udp>(
      {net::ip::address_v4::loopback(), 0},
      [state] {
        return serveza::awaitable_session{[state](serveza::bound_socket_context<udp>& ctx) -> net::awaitable<void> {
          co_await net::this_coro::reset_cancellation_state(net::disable_cancellation());
          state->started();
          net::steady_timer timer{ctx.get_executor()};
          timer.expires_after(std::chrono::milliseconds{20});
          sys::error_code ec;
          co_await timer.async_wait(net::redirect_error(net::use_awaitable, ec));
          state->completed = !ec;
        }};
      },
      options);

  listener->async_wait([](sys::error_code ec) { EXPECT_FALSE(ec); });
  listener->async_start([](sys::error_code ec) { EXPECT_FALSE(ec); });
  io.run();

  EXPECT_TRUE(state->completed);
  EXPECT_EQ(listener->active_sessions(), 0u);
  EXPECT_EQ(listener->status().last_error, make_error_code(sys::errc::timed_out));
}

} // namespace
