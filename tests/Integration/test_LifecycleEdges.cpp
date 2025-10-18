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
#include <functional>
#include <memory>
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

struct ImmediateSession {
  template<typename Context, typename Handler>
  void operator()(Context&, Handler handler) const
  {
    handler(sys::error_code{});
  }
};

class RecordingObserver final : public serveza::observer {
public:
  void on_event(const serveza::listener_event& event) override
  {
    if (received) received(event);
  }

  std::function<void(const serveza::listener_event&)> received;
};

unsigned short listenerPort(const std::shared_ptr<serveza::listener>& listener)
{
  const std::string endpoint = listener->endpoint();
  return static_cast<unsigned short>(std::stoul(endpoint.substr(endpoint.rfind(':') + 1)));
}

TEST(LifecycleEdgeTests, IdleTcpListenerStopsAndSubsequentWaitCompletesAsynchronously)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });
  bool firstWait = false;

  EXPECT_EQ(listener->id(), 1u);
  EXPECT_EQ(listener->active_sessions(), 0u);
  listener->request_stop();
  listener->async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    firstWait = true;
  });
  io.run();

  EXPECT_TRUE(firstWait);
  EXPECT_EQ(listener->state(), serveza::listener_state::stopped);
  EXPECT_EQ(listener->status().stop_reason, serveza::listener_stop_reason::requested);

  io.restart();
  bool initiating = true;
  bool secondWait = false;
  listener->async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(initiating);
    EXPECT_FALSE(ec);
    secondWait = true;
  });
  initiating = false;
  io.run();
  EXPECT_TRUE(secondWait);
}

TEST(LifecycleEdgeTests, IdleUdpListenerStopsAndSubsequentWaitCompletesAsynchronously)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.bind<udp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });
  bool firstWait = false;

  EXPECT_EQ(listener->id(), 1u);
  listener->request_stop();
  listener->async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    firstWait = true;
  });
  io.run();

  EXPECT_TRUE(firstWait);
  EXPECT_EQ(listener->state(), serveza::listener_state::stopped);

  io.restart();
  bool initiating = true;
  bool secondWait = false;
  listener->async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(initiating);
    EXPECT_FALSE(ec);
    secondWait = true;
  });
  initiating = false;
  io.run();
  EXPECT_TRUE(secondWait);
}

TEST(LifecycleEdgeTests, CancellationBeforeUdpStartLeavesListenerIdle)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.bind<udp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });
  net::cancellation_signal cancellation;
  bool completed = false;

  listener->async_start(net::bind_cancellation_slot(cancellation.slot(), [&](sys::error_code ec) {
    EXPECT_EQ(ec, net::error::operation_aborted);
    completed = true;
  }));
  cancellation.emit(net::cancellation_type::all);
  io.run();

  EXPECT_TRUE(completed);
  EXPECT_EQ(listener->state(), serveza::listener_state::idle);
  EXPECT_EQ(listener->status().generation, 0u);
}

TEST(LifecycleEdgeTests, CancellingUdpWaitDoesNotStopBoundSession)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<std::vector<serveza::listener::completion_handler>> completions =
      std::make_shared<std::vector<serveza::listener::completion_handler>>();
  serveza::listener_options options;
  options.shutdown_grace_period = std::chrono::milliseconds{1};
  std::shared_ptr<serveza::listener> listener = server.bind<udp>(
      {net::ip::address_v4::loopback(), 0},
      [completions] {
        return serveza::callback_session{
            [completions](auto&, auto handler) { completions->emplace_back(std::move(handler)); }};
      },
      options);
  net::cancellation_signal cancellation;
  bool cancelled = false;
  bool stopped = false;

  listener->async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = true;
  });
  listener->async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    listener->async_wait(net::bind_cancellation_slot(cancellation.slot(), [&](sys::error_code waitEc) {
      EXPECT_EQ(waitEc, net::error::operation_aborted);
      EXPECT_EQ(listener->state(), serveza::listener_state::running);
      cancelled = true;
      listener->request_stop();
    }));
    cancellation.emit(net::cancellation_type::all);
  });
  io.run();

  EXPECT_TRUE(cancelled);
  EXPECT_TRUE(stopped);
  ASSERT_EQ(completions->size(), 1u);
  std::move(completions->front())(sys::error_code{});
}

TEST(LifecycleEdgeTests, SynchronousTcpSessionExceptionIsPreserved)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<RecordingObserver> observer = std::make_shared<RecordingObserver>();
  [[maybe_unused]] serveza::observer_subscription subscription = server.observe(observer);
  const sys::error_code expected = make_error_code(sys::errc::permission_denied);
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>({net::ip::address_v4::loopback(), 0}, [expected] {
    return serveza::callback_session{[expected](auto&, auto) { throw sys::system_error{expected}; }};
  });
  tcp::socket client{io};
  bool stopped = false;
  net::steady_timer deadline{io, std::chrono::seconds{2}};

  observer->received = [&](const serveza::listener_event& event) {
    if (event.type != serveza::listener_event_type::session_stopped) return;
    EXPECT_EQ(event.error, expected);
    EXPECT_TRUE(event.exception);
    deadline.cancel();
    server.request_stop();
  };
  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = true;
  });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client.async_connect({net::ip::address_v4::loopback(), listenerPort(listener)},
                         [](sys::error_code connectEc) { EXPECT_FALSE(connectEc); });
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    ADD_FAILURE() << "synchronous session exception was not observed";
    server.request_stop();
  });
  io.run();

  EXPECT_TRUE(stopped);
  EXPECT_EQ(listener->status().last_error, expected);
  ASSERT_TRUE(listener->status().last_exception);
  EXPECT_THROW(std::rethrow_exception(listener->status().last_exception), sys::system_error);
}

TEST(LifecycleEdgeTests, SynchronousUdpSessionExceptionIsPreserved)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  const sys::error_code expected = make_error_code(sys::errc::permission_denied);
  std::shared_ptr<serveza::listener> listener = server.bind<udp>({net::ip::address_v4::loopback(), 0}, [expected] {
    return serveza::callback_session{[expected](auto&, auto) { throw sys::system_error{expected}; }};
  });
  bool stopped = false;

  listener->async_wait([&](sys::error_code ec) {
    EXPECT_EQ(ec, expected);
    stopped = true;
  });
  listener->async_start([](sys::error_code ec) { EXPECT_FALSE(ec); });
  io.run();

  EXPECT_TRUE(stopped);
  EXPECT_EQ(listener->status().last_error, expected);
  EXPECT_EQ(listener->status().stop_reason, serveza::listener_stop_reason::runtime_failure);
  ASSERT_TRUE(listener->status().last_exception);
  EXPECT_THROW(std::rethrow_exception(listener->status().last_exception), sys::system_error);
}

} // namespace
