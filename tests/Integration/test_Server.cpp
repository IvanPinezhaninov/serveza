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
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
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

struct CountingSession {
  std::shared_ptr<std::size_t> count;
  serveza::server& server;

  template<typename Context, typename Handler>
  void operator()(Context&, Handler handler) const
  {
    ++*count;
    if (*count == 2) server.request_stop();
    handler(sys::error_code{});
  }
};

struct ImmediateSession {
  template<typename Context, typename Handler>
  void operator()(Context&, Handler handler) const
  {
    handler(sys::error_code{});
  }
};

struct GatedExecutorState {
  net::io_context io;
  std::mutex mutex;
  std::condition_variable cv;
  bool armed{};
  bool blocked{};
  bool released{};
};

class GatedExecutor final {
public:
  using execution_context = net::io_context;

  explicit GatedExecutor(std::shared_ptr<GatedExecutorState> state)
    : m_state{std::move(state)}
  {}

  execution_context& context() const noexcept
  {
    return m_state->io;
  }

  void on_work_started() const noexcept
  {
    m_state->io.get_executor().on_work_started();
  }

  void on_work_finished() const noexcept
  {
    m_state->io.get_executor().on_work_finished();
  }

  template<typename Function>
  void execute(Function&& fn) const
  {
    {
      std::unique_lock lock{m_state->mutex};
      if (m_state->armed && !m_state->blocked) {
        m_state->blocked = true;
        m_state->cv.notify_all();
        m_state->cv.wait(lock, [this] { return m_state->released; });
      }
    }
    net::post(m_state->io, std::forward<Function>(fn));
  }

  net::execution_context& query(net::execution::context_t) const noexcept
  {
    return context();
  }

  constexpr static net::execution::blocking_t::never_t query(net::execution::blocking_t) noexcept
  {
    return net::execution::blocking.never;
  }

  bool operator==(const GatedExecutor& other) const noexcept
  {
    return m_state == other.m_state;
  }

  bool operator!=(const GatedExecutor& other) const noexcept
  {
    return !(*this == other);
  }

private:
  std::shared_ptr<GatedExecutorState> m_state;
};

} // namespace

namespace boost::asio::traits {

#if !defined(BOOST_ASIO_HAS_DEDUCED_EXECUTE_MEMBER_TRAIT)

template<typename Function>
struct execute_member<GatedExecutor, Function> {
  static constexpr bool is_valid = true;
  static constexpr bool is_noexcept = false;
  using result_type = void;
};

#endif

#if !defined(BOOST_ASIO_HAS_DEDUCED_QUERY_MEMBER_TRAIT)

template<>
struct query_member<GatedExecutor, execution::context_t> {
  static constexpr bool is_valid = true;
  static constexpr bool is_noexcept = true;
  using result_type = execution_context&;
};

#endif

#if !defined(BOOST_ASIO_HAS_DEDUCED_QUERY_STATIC_CONSTEXPR_MEMBER_TRAIT)

template<typename Property>
struct query_static_constexpr_member<GatedExecutor, Property,
                                     std::enable_if_t<std::is_convertible_v<Property, execution::blocking_t>>> {
  static constexpr bool is_valid = true;
  static constexpr bool is_noexcept = true;
  using result_type = execution::blocking_t::never_t;

  static constexpr result_type value() noexcept
  {
    return result_type{};
  }
};

#endif

#if !defined(BOOST_ASIO_HAS_DEDUCED_EQUALITY_COMPARABLE_TRAIT)

template<>
struct equality_comparable<GatedExecutor> {
  static constexpr bool is_valid = true;
  static constexpr bool is_noexcept = true;
};

#endif

} // namespace boost::asio::traits

namespace {

class ServerObserver final : public serveza::observer {
public:
  void on_event(const serveza::listener_event&) override {}

  void on_server_event(const serveza::server_event& event) override
  {
    events.push_back(event);
  }

  std::vector<serveza::server_event> events;
};

TEST(ServerTests, ReportsAggregateReadinessAndLifecycleEvents)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<ServerObserver> observer = std::make_shared<ServerObserver>();
  [[maybe_unused]] serveza::observer_subscription subscription = server.observe(observer);
  server.listen<tcp>({net::ip::address_v4::loopback(), 0},
                     [] { return serveza::callback_session{ImmediateSession{}}; });

  EXPECT_EQ(server.status().state, serveza::server_state::idle);
  EXPECT_FALSE(server.is_ready());
  bool stopped = false;
  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = true;
  });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    EXPECT_TRUE(server.is_ready());
    EXPECT_EQ(server.status().generation, 1u);
    EXPECT_EQ(server.active_sessions(), 0u);
    server.request_stop();
  });
  io.run();

  EXPECT_TRUE(stopped);
  const serveza::server_status status = server.status();
  EXPECT_EQ(status.state, serveza::server_state::stopped);
  EXPECT_FALSE(status.ready);
  ASSERT_EQ(status.listeners.size(), 1u);
  EXPECT_EQ(status.listeners.front().stop_reason, serveza::listener_stop_reason::requested);
  ASSERT_EQ(observer->events.size(), 4u);
  EXPECT_EQ(observer->events[0].type, serveza::server_event_type::starting);
  EXPECT_EQ(observer->events[1].type, serveza::server_event_type::running);
  EXPECT_EQ(observer->events[2].type, serveza::server_event_type::stopping);
  EXPECT_EQ(observer->events[3].type, serveza::server_event_type::stopped);
}

TEST(ServerTests, MultipleListenersAcceptAndStopTogether)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<std::size_t> sessions = std::make_shared<std::size_t>();
  std::shared_ptr<serveza::listener> first =
      server.listen<tcp>({net::ip::address_v4::loopback(), 0},
                         [sessions, &server] { return serveza::callback_session{CountingSession{sessions, server}}; });
  std::shared_ptr<serveza::listener> second =
      server.listen<tcp>({net::ip::address_v4::loopback(), 0},
                         [sessions, &server] { return serveza::callback_session{CountingSession{sessions, server}}; });

  tcp::socket firstClient{io};
  tcp::socket secondClient{io};
  bool started = false;
  bool stopped = false;
  bool timedOut = false;
  net::steady_timer deadline{io, std::chrono::seconds{2}};
  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
    deadline.cancel();
  });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    started = true;
    firstClient.async_connect({net::ip::address_v4::loopback(), listenerPort(first)},
                              [](sys::error_code connectEc) { EXPECT_FALSE(connectEc); });
    secondClient.async_connect({net::ip::address_v4::loopback(), listenerPort(second)},
                               [](sys::error_code connectEc) { EXPECT_FALSE(connectEc); });
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    timedOut = true;
    server.request_stop();
  });

  io.run();

  EXPECT_TRUE(started);
  EXPECT_TRUE(stopped);
  EXPECT_FALSE(timedOut);
  EXPECT_EQ(*sessions, 2u);
  EXPECT_EQ(first->state(), serveza::listener_state::stopped);
  EXPECT_EQ(second->state(), serveza::listener_state::stopped);
}

TEST(ServerTests, ListenerRegisteredDuringRunJoinsTheNextStartSnapshot)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> first = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });
  std::shared_ptr<serveza::listener> second;

  server.async_wait([](sys::error_code ec) { EXPECT_FALSE(ec); });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    second = server.listen<tcp>({net::ip::address_v4::loopback(), 0},
                                [] { return serveza::callback_session{ImmediateSession{}}; });
    EXPECT_EQ(first->state(), serveza::listener_state::running);
    EXPECT_EQ(second->state(), serveza::listener_state::idle);
    EXPECT_FALSE(server.is_ready());
    server.request_stop();
  });
  io.run();

  ASSERT_TRUE(second);
  EXPECT_EQ(first->state(), serveza::listener_state::stopped);
  EXPECT_EQ(second->state(), serveza::listener_state::stopped);
  EXPECT_EQ(first->status().generation, 1u);
  EXPECT_EQ(second->status().generation, 0u);

  io.restart();
  second->async_start([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    second->request_stop();
  });
  io.run();
  EXPECT_EQ(second->status().generation, 1u);
}

TEST(ServerTests, StartupFailureStopsListenersThatDidStart)
{
  net::io_context io;
  tcp::acceptor blocker{io, {net::ip::address_v4::loopback(), 0}};
  const tcp::endpoint blockedEndpoint = blocker.local_endpoint();
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> healthy = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });
  std::shared_ptr<serveza::listener> blocked =
      server.listen<tcp>(blockedEndpoint, [] { return serveza::callback_session{ImmediateSession{}}; });

  sys::error_code startError;
  bool stopped = false;
  bool timedOut = false;
  net::steady_timer deadline{io, std::chrono::seconds{2}};
  server.async_wait([&](sys::error_code ec) {
    EXPECT_TRUE(ec);
    stopped = true;
    deadline.cancel();
  });
  server.async_start([&](sys::error_code ec) { startError = ec; });
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    timedOut = true;
    server.request_stop();
  });

  io.run();

  EXPECT_TRUE(startError);
  EXPECT_TRUE(stopped);
  EXPECT_FALSE(timedOut);
  EXPECT_EQ(healthy->state(), serveza::listener_state::stopped);
  EXPECT_EQ(blocked->state(), serveza::listener_state::stopped);
  EXPECT_EQ(healthy->status().stop_reason, serveza::listener_stop_reason::requested);
  EXPECT_EQ(blocked->status().stop_reason, serveza::listener_stop_reason::startup_failure);
  const serveza::server_status status = server.status();
  ASSERT_EQ(status.listeners.size(), 2u);
  EXPECT_FALSE(status.listeners[0].last_error);
  EXPECT_TRUE(status.listeners[1].last_error);
}

TEST(ServerTests, CancellingWaitDoesNotStopAnyListener)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> first = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });
  std::shared_ptr<serveza::listener> second = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });
  net::cancellation_signal cancellation;

  bool waitCancelled = false;
  bool stopped = false;
  bool timedOut = false;
  net::steady_timer deadline{io, std::chrono::seconds{2}};
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    server.async_wait([&](sys::error_code waitEc) {
      EXPECT_FALSE(waitEc);
      stopped = !waitEc;
      deadline.cancel();
    });
    server.async_wait(net::bind_cancellation_slot(cancellation.slot(), [&](sys::error_code waitEc) {
      EXPECT_EQ(waitEc, net::error::operation_aborted);
      EXPECT_EQ(first->state(), serveza::listener_state::running);
      EXPECT_EQ(second->state(), serveza::listener_state::running);
      waitCancelled = true;
      server.request_stop();
    }));
    cancellation.emit(net::cancellation_type::all);
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    timedOut = true;
    server.request_stop();
  });

  io.run();

  EXPECT_TRUE(waitCancelled);
  EXPECT_TRUE(stopped);
  EXPECT_FALSE(timedOut);
}

TEST(ServerTests, StartWhileStoppingIsRejectedWithoutRestartingListeners)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });

  bool restartRejected = false;
  bool stopped = false;
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    server.request_stop();
    listener->async_wait([&](sys::error_code waitEc) {
      ASSERT_FALSE(waitEc);
      EXPECT_EQ(server.status().state, serveza::server_state::stopping);
      server.async_start([&](sys::error_code restartEc) {
        EXPECT_EQ(restartEc, net::error::operation_aborted);
        EXPECT_EQ(listener->state(), serveza::listener_state::stopped);
        EXPECT_EQ(server.status().generation, 1u);
        restartRejected = restartEc == net::error::operation_aborted;
        server.async_wait([&](sys::error_code finalEc) {
          EXPECT_FALSE(finalEc);
          stopped = !finalEc;
        });
      });
    });
  });

  io.run();

  EXPECT_TRUE(restartRejected);
  EXPECT_TRUE(stopped);
  EXPECT_EQ(server.status().state, serveza::server_state::stopped);
  EXPECT_EQ(listener->status().generation, 1u);
}

TEST(ServerTests, StopDuringStartCannotBeOvertakenByQueuedListenerStarts)
{
  std::shared_ptr<GatedExecutorState> state = std::make_shared<GatedExecutorState>();
  serveza::server server{GatedExecutor{state}};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });
  std::shared_ptr<serveza::listener> datagrams = server.bind<udp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });

  sys::error_code startEc;
  sys::error_code waitEc;
  bool started = false;
  bool stopped = false;
  {
    std::lock_guard lock{state->mutex};
    state->armed = true;
  }
  std::thread starter{[&] {
    server.async_start([&](sys::error_code ec) {
      startEc = ec;
      started = true;
    });
  }};
  {
    std::unique_lock lock{state->mutex};
    state->cv.wait(lock, [&] { return state->blocked; });
  }

  server.request_stop();
  server.async_wait([&](sys::error_code ec) {
    waitEc = ec;
    stopped = true;
  });
  {
    std::lock_guard lock{state->mutex};
    state->released = true;
  }
  state->cv.notify_all();
  starter.join();
  state->io.run();

  EXPECT_TRUE(started);
  EXPECT_EQ(startEc, net::error::operation_aborted);
  EXPECT_TRUE(stopped);
  EXPECT_FALSE(waitEc);
  EXPECT_EQ(server.status().state, serveza::server_state::stopped);
  EXPECT_EQ(listener->state(), serveza::listener_state::stopped);
  EXPECT_EQ(datagrams->state(), serveza::listener_state::stopped);
}

TEST(ServerTests, DestructionRequestsListenerShutdown)
{
  net::io_context io;
  std::unique_ptr<serveza::server> server = std::make_unique<serveza::server>(io.get_executor());
  std::shared_ptr<serveza::listener> listener = server->listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });
  bool stopped = false;

  listener->async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
  });
  server->async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    server.reset();
  });
  io.run();

  EXPECT_TRUE(stopped);
  EXPECT_EQ(listener->state(), serveza::listener_state::stopped);
  EXPECT_EQ(listener->status().stop_reason, serveza::listener_stop_reason::requested);
}

} // namespace
