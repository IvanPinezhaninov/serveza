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

#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

#include <boost/asio.hpp>

#include <gtest/gtest.h>

#include <serveza/detail/observer_registry.h>
#include <serveza/serveza.h>

namespace net = boost::asio;
namespace sys = boost::system;
using tcp = net::ip::tcp;

namespace {

class NoopObserver final : public serveza::observer {
public:
  void on_event(const serveza::listener_event&) override {}
};

serveza::observer_subscription&& moveSubscription(serveza::observer_subscription& subscription) noexcept
{
  return static_cast<serveza::observer_subscription&&>(subscription);
}

TEST(CoreUtilitiesTests, ObserverSubscriptionSupportsMoveAndReset)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<NoopObserver> firstObserver = std::make_shared<NoopObserver>();
  std::shared_ptr<NoopObserver> secondObserver = std::make_shared<NoopObserver>();
  serveza::observer_subscription first = server.observe(firstObserver);
  serveza::observer_subscription moved{std::move(first)};

  EXPECT_FALSE(first);
  EXPECT_TRUE(moved);

  serveza::observer_subscription assigned = server.observe(secondObserver);
  assigned = std::move(moved);
  EXPECT_FALSE(moved);
  EXPECT_TRUE(assigned);

  assigned = moveSubscription(assigned);
  EXPECT_TRUE(assigned);
  assigned.reset();
  EXPECT_FALSE(assigned);
  assigned.reset();
}

TEST(CoreUtilitiesTests, SessionContextExposesStateAndContainsReporterExceptions)
{
  net::io_context io;
  tcp::socket socket{io};
  net::cancellation_signal cancellation;
  serveza::connection_info info{7, "127.0.0.1:10", "127.0.0.1:20"};
  bool reported = false;
  serveza::session_context<tcp> ctx{socket, 9, info, cancellation.slot(), [&](std::exception_ptr exception) {
                                      reported = true;
                                      EXPECT_THROW(std::rethrow_exception(std::move(exception)), std::runtime_error);
                                      throw std::runtime_error{"observer failure"};
                                    }};

  EXPECT_EQ(&ctx.socket(), &socket);
  EXPECT_EQ(ctx.get_executor(), socket.get_executor());
  EXPECT_EQ(ctx.listener_id(), 9u);
  EXPECT_EQ(ctx.info().id, 7u);
  EXPECT_TRUE(ctx.cancellation_slot().is_connected());
  EXPECT_NO_THROW(ctx.report_exception(std::make_exception_ptr(std::runtime_error{"session failure"})));
  EXPECT_TRUE(reported);

  serveza::session_context<tcp> withoutReporter{socket, 10, {}, cancellation.slot()};
  EXPECT_NO_THROW(withoutReporter.report_exception(std::make_exception_ptr(std::runtime_error{"ignored"})));
}

TEST(CoreUtilitiesTests, ExceptionConversionPreservesSystemErrors)
{
  const sys::error_code expected = make_error_code(sys::errc::permission_denied);
  EXPECT_FALSE(serveza::detail::exception_to_error({}));
  EXPECT_EQ(serveza::detail::exception_to_error(std::make_exception_ptr(sys::system_error{expected})), expected);
  EXPECT_EQ(serveza::detail::exception_to_error(std::make_exception_ptr(std::runtime_error{"failure"})),
            make_error_code(sys::errc::io_error));
}

TEST(CoreUtilitiesTests, EmptyServerLifecycleCompletesAsynchronously)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  bool starting = true;
  bool started = false;
  bool waited = false;

  EXPECT_FALSE(server.is_ready());
  EXPECT_EQ(server.active_sessions(), 0u);
  EXPECT_TRUE(server.listeners().empty());
  server.async_start([&](sys::error_code ec) {
    EXPECT_FALSE(starting);
    EXPECT_FALSE(ec);
    started = true;
  });
  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(starting);
    EXPECT_FALSE(ec);
    waited = true;
  });
  starting = false;

  io.run();
  EXPECT_TRUE(started);
  EXPECT_TRUE(waited);
  EXPECT_EQ(server.status().state, serveza::server_state::stopped);
}

TEST(CoreUtilitiesTests, EmptyServerCannotRestartWhileStopping)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  bool rejected = false;
  bool stopped = false;

  server.async_start([](sys::error_code ec) { EXPECT_FALSE(ec); });
  server.request_stop();
  server.async_start([&](sys::error_code ec) {
    EXPECT_EQ(ec, net::error::operation_aborted);
    rejected = ec == net::error::operation_aborted;
  });
  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
  });

  io.run();
  EXPECT_TRUE(rejected);
  EXPECT_TRUE(stopped);
  EXPECT_EQ(server.status().generation, 1u);
  EXPECT_EQ(server.status().state, serveza::server_state::stopped);
}

} // namespace
