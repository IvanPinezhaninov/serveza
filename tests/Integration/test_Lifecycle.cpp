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
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio.hpp>

#include <gtest/gtest.h>

#include <serveza/serveza.h>
#include <serveza/yield_session.h>

namespace net = boost::asio;
namespace sys = boost::system;
using tcp = net::ip::tcp;

namespace {

unsigned short listenerPort(const std::shared_ptr<serveza::listener>& listener)
{
  const std::string endpoint = listener->endpoint();
  return static_cast<unsigned short>(std::stoul(endpoint.substr(endpoint.rfind(':') + 1)));
}

struct ImmediateSession {
  std::shared_ptr<std::function<void()>> started;

  template<typename Context, typename Handler>
  void operator()(Context&, Handler handler) const
  {
    if (started && *started) (*started)();
    handler(sys::error_code{});
  }
};

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

class ThrowingObserver final : public serveza::observer {
public:
  void on_event(const serveza::listener_event&) override
  {
    throw std::runtime_error{"observer failure"};
  }
};

class RecoveringFactory final {
public:
  RecoveringFactory(const std::shared_ptr<std::size_t>& attempts,
                    std::shared_ptr<std::function<void()>> session_started)
    : m_attempts{attempts}
    , m_session_started{std::move(session_started)}
  {}

  serveza::callback_session<ImmediateSession> operator()()
  {
    ++*m_attempts;
    if (*m_attempts == 1) throw std::runtime_error{"factory failure"};
    return serveza::callback_session{ImmediateSession{m_session_started}};
  }

private:
  std::shared_ptr<std::size_t> m_attempts;
  std::shared_ptr<std::function<void()>> m_session_started;
};

class RejectingExecutor {
public:
  using execution_context = net::io_context;

  execution_context& context() const noexcept
  {
    static execution_context ctx;
    return ctx;
  }

  void on_work_started() const noexcept {}
  void on_work_finished() const noexcept {}

  template<typename Function>
  void execute(Function&&) const
  {
    throw std::runtime_error{"executor rejected work"};
  }

  net::execution_context& query(net::execution::context_t) const noexcept
  {
    return context();
  }

  constexpr static net::execution::blocking_t::never_t query(net::execution::blocking_t) noexcept
  {
    return net::execution::blocking.never;
  }

  template<typename Function, typename Allocator>
  void dispatch(Function&&, const Allocator&) const
  {
    throw std::runtime_error{"executor rejected work"};
  }

  template<typename Function, typename Allocator>
  void post(Function&&, const Allocator&) const
  {
    throw std::runtime_error{"executor rejected work"};
  }

  template<typename Function, typename Allocator>
  void defer(Function&&, const Allocator&) const
  {
    throw std::runtime_error{"executor rejected work"};
  }

  bool operator==(const RejectingExecutor&) const noexcept
  {
    return true;
  }

  bool operator!=(const RejectingExecutor&) const noexcept
  {
    return false;
  }
};

} // namespace

namespace boost::asio::traits {

#if !defined(BOOST_ASIO_HAS_DEDUCED_EXECUTE_MEMBER_TRAIT)

template<typename Function>
struct execute_member<RejectingExecutor, Function> {
  static constexpr bool is_valid = true;
  static constexpr bool is_noexcept = false;
  using result_type = void;
};

#endif

#if !defined(BOOST_ASIO_HAS_DEDUCED_QUERY_MEMBER_TRAIT)

template<>
struct query_member<RejectingExecutor, execution::context_t> {
  static constexpr bool is_valid = true;
  static constexpr bool is_noexcept = true;
  using result_type = execution_context&;
};

#endif

#if !defined(BOOST_ASIO_HAS_DEDUCED_QUERY_STATIC_CONSTEXPR_MEMBER_TRAIT)

template<typename Property>
struct query_static_constexpr_member<RejectingExecutor, Property,
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
struct equality_comparable<RejectingExecutor> {
  static constexpr bool is_valid = true;
  static constexpr bool is_noexcept = true;
};

#endif

} // namespace boost::asio::traits

namespace {

struct HangingState {
  std::vector<serveza::listener::completion_handler> completions;
  std::function<void()> started;
};

struct HangingSession {
  std::shared_ptr<HangingState> state;

  template<typename Context, typename Handler>
  void operator()(Context&, Handler handler) const
  {
    state->completions.emplace_back(std::move(handler));
    if (state->started) state->started();
  }
};

struct DelayedSessionState {
  std::function<void()> started;
  bool completed{};
};

template<typename T>
class CountingAllocator {
public:
  using value_type = T;

  explicit CountingAllocator(std::shared_ptr<std::atomic_size_t> allocations) noexcept
    : m_allocations{std::move(allocations)}
  {}

  template<typename U>
  CountingAllocator(const CountingAllocator<U>& other) noexcept
    : m_allocations{other.allocations()}
  {}

  [[nodiscard]] T* allocate(std::size_t size)
  {
    m_allocations->fetch_add(1, std::memory_order_relaxed);
    return std::allocator<T>{}.allocate(size);
  }

  void deallocate(T* value, std::size_t size) noexcept
  {
    std::allocator<T>{}.deallocate(value, size);
  }

  [[nodiscard]] const std::shared_ptr<std::atomic_size_t>& allocations() const noexcept
  {
    return m_allocations;
  }

  template<typename U>
  friend bool operator==(const CountingAllocator& left, const CountingAllocator<U>& right) noexcept
  {
    return left.m_allocations == right.allocations();
  }

  template<typename U>
  friend bool operator!=(const CountingAllocator& left, const CountingAllocator<U>& right) noexcept
  {
    return !(left == right);
  }

private:
  std::shared_ptr<std::atomic_size_t> m_allocations;
};

TEST(LifecycleTests, ConcurrentStartsShareOneGenerationAndListenerCanRestart)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });

  std::size_t starts = 0;
  std::size_t stops = 0;
  server.async_start([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    ++starts;
  });
  server.async_start([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    ++starts;
    EXPECT_EQ(listener->status().generation, 1u);
    listener->async_wait([&](sys::error_code waitEc) {
      EXPECT_FALSE(waitEc);
      ++stops;
      EXPECT_EQ(listener->status().generation, 1u);
      listener->async_start([&](sys::error_code restartEc) {
        EXPECT_FALSE(restartEc);
        ++starts;
        const serveza::listener_status running = listener->status();
        EXPECT_EQ(running.state, serveza::listener_state::running);
        EXPECT_EQ(running.generation, 2u);
        listener->async_wait([&](sys::error_code secondWaitEc) {
          EXPECT_FALSE(secondWaitEc);
          ++stops;
        });
        listener->request_stop();
      });
    });
    listener->request_stop();
  });

  io.run();

  EXPECT_EQ(starts, 3u);
  EXPECT_EQ(stops, 2u);
  const serveza::listener_status stopped = listener->status();
  EXPECT_EQ(stopped.state, serveza::listener_state::stopped);
  EXPECT_EQ(stopped.generation, 2u);
  EXPECT_EQ(stopped.active_sessions, 0u);
}

TEST(LifecycleTests, ListenerSurvivesOneHundredGenerations)
{
  constexpr std::size_t generationCount = 100;
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });

  std::size_t starts = 0;
  std::size_t stops = 0;
  std::function<void()> startNext;
  startNext = [&] {
    listener->async_start([&](sys::error_code ec) {
      ASSERT_FALSE(ec);
      ++starts;
      listener->async_wait([&](sys::error_code waitEc) {
        EXPECT_FALSE(waitEc);
        ++stops;
        if (stops < generationCount) startNext();
      });
      listener->request_stop();
    });
  };
  startNext();
  io.run();

  EXPECT_EQ(starts, generationCount);
  EXPECT_EQ(stops, generationCount);
  EXPECT_EQ(listener->status().generation, generationCount);
  EXPECT_EQ(listener->status().stop_reason, serveza::listener_stop_reason::requested);
}

TEST(LifecycleTests, CompletionUsesAssociatedExecutorAndAllocator)
{
  net::io_context listenerIo;
  net::io_context completionIo;
  serveza::server server{listenerIo.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });
  std::shared_ptr<std::atomic_size_t> allocations = std::make_shared<std::atomic_size_t>();

  bool started = false;
  listener->async_start(net::bind_allocator(CountingAllocator<std::byte>{allocations},
                                            net::bind_executor(completionIo.get_executor(), [&](sys::error_code ec) {
                                              EXPECT_FALSE(ec);
                                              started = !ec;
                                            })));
  const std::size_t allocationsAfterInitiation = allocations->load(std::memory_order_relaxed);
  EXPECT_GT(allocationsAfterInitiation, 0u);
  EXPECT_EQ(completionIo.poll(), 0u);
  EXPECT_FALSE(completionIo.stopped());

  listenerIo.poll();
  EXPECT_FALSE(started);
  EXPECT_GE(allocations->load(std::memory_order_relaxed), allocationsAfterInitiation);
  completionIo.run();
  EXPECT_TRUE(started);

  bool stopped = false;
  completionIo.restart();
  listener->async_wait(net::bind_executor(completionIo.get_executor(), [&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
  }));
  listener->request_stop();
  listenerIo.restart();
  listenerIo.run();
  EXPECT_FALSE(stopped);
  completionIo.run();
  EXPECT_TRUE(stopped);
}

TEST(LifecycleTests, ExecutorSubmissionFailureCompletesExactlyOnceOnAssociatedExecutor)
{
  net::io_context completionIo;
  serveza::server server{RejectingExecutor{}};
  std::shared_ptr<serveza::listener> startListener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });
  std::shared_ptr<serveza::listener> waitListener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });
  std::shared_ptr<serveza::listener> reentrantListener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });

  std::size_t startCompletions = 0;
  std::size_t waitCompletions = 0;
  std::size_t reentrantCompletions = 0;
  startListener->async_start(net::bind_executor(completionIo.get_executor(), [&](sys::error_code ec) {
    EXPECT_EQ(ec, make_error_code(sys::errc::io_error));
    ++startCompletions;
  }));
  waitListener->async_wait(net::bind_executor(completionIo.get_executor(), [&](sys::error_code ec) {
    EXPECT_EQ(ec, make_error_code(sys::errc::io_error));
    ++waitCompletions;
  }));

  EXPECT_EQ(startCompletions, 0u);
  EXPECT_EQ(waitCompletions, 0u);
  completionIo.run();
  EXPECT_EQ(startCompletions, 1u);
  EXPECT_EQ(waitCompletions, 1u);
  EXPECT_EQ(startListener->state(), serveza::listener_state::idle);
  EXPECT_EQ(waitListener->state(), serveza::listener_state::idle);

  completionIo.restart();
  bool initiating = false;
  net::post(completionIo, [&] {
    initiating = true;
    reentrantListener->async_start(net::bind_executor(completionIo.get_executor(), [&](sys::error_code ec) {
      EXPECT_FALSE(initiating);
      EXPECT_EQ(ec, make_error_code(sys::errc::io_error));
      ++reentrantCompletions;
    }));
    initiating = false;
    EXPECT_EQ(reentrantCompletions, 0u);
  });
  completionIo.run();

  EXPECT_EQ(reentrantCompletions, 1u);
  EXPECT_EQ(reentrantListener->state(), serveza::listener_state::idle);
}

TEST(LifecycleTests, ObserverSubscriptionRejectsNullAndCanRemoveItself)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  EXPECT_THROW(static_cast<void>(server.observe({})), std::invalid_argument);

  std::shared_ptr<RecordingObserver> observer = std::make_shared<RecordingObserver>();
  serveza::observer_subscription subscription = server.observe(observer);
  observer->received = [&](const serveza::listener_event&) { subscription.reset(); };
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });

  listener->async_start([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    listener->request_stop();
  });
  io.run();

  EXPECT_FALSE(subscription);
  EXPECT_EQ(observer->events.size(), 1u);
  EXPECT_EQ(listener->status().stop_reason, serveza::listener_stop_reason::requested);
}

TEST(LifecycleTests, ConnectionEventsCarryStableEndpointMetadata)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<RecordingObserver> observer = std::make_shared<RecordingObserver>();
  [[maybe_unused]] serveza::observer_subscription subscription = server.observe(observer);
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });

  net::steady_timer deadline{io, std::chrono::seconds{2}};
  observer->received = [&](const serveza::listener_event& event) {
    if (event.type != serveza::listener_event_type::session_stopped) return;
    deadline.cancel();
    server.request_stop();
  };

  tcp::socket client{io};
  listener->async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client.async_connect({net::ip::address_v4::loopback(), listenerPort(listener)},
                         [](sys::error_code connectEc) { EXPECT_FALSE(connectEc); });
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    ADD_FAILURE() << "connection events were not emitted";
    server.request_stop();
  });
  io.run();

  const std::vector<serveza::listener_event>::const_iterator started =
      std::find_if(observer->events.begin(), observer->events.end(), [](const serveza::listener_event& event) {
        return event.type == serveza::listener_event_type::session_started;
      });
  const std::vector<serveza::listener_event>::const_iterator stopped =
      std::find_if(observer->events.begin(), observer->events.end(), [](const serveza::listener_event& event) {
        return event.type == serveza::listener_event_type::session_stopped;
      });
  ASSERT_NE(started, observer->events.end());
  ASSERT_NE(stopped, observer->events.end());
  EXPECT_NE(started->connection.id, 0u);
  EXPECT_FALSE(started->connection.local_endpoint.empty());
  EXPECT_FALSE(started->connection.remote_endpoint.empty());
  EXPECT_EQ(started->connection.id, stopped->connection.id);
  EXPECT_EQ(started->connection.local_endpoint, stopped->connection.local_endpoint);
  EXPECT_EQ(started->connection.remote_endpoint, stopped->connection.remote_endpoint);
}

TEST(LifecycleTests, CancellationBeforeStartLeavesListenerIdle)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });
  net::cancellation_signal cancellation;

  bool initiating = true;
  bool completed = false;
  listener->async_start(net::bind_cancellation_slot(cancellation.slot(), [&](sys::error_code ec) {
    EXPECT_FALSE(initiating);
    EXPECT_EQ(ec, net::error::operation_aborted);
    completed = true;
  }));
  cancellation.emit(net::cancellation_type::all);
  initiating = false;

  io.run();

  EXPECT_TRUE(completed);
  EXPECT_EQ(listener->state(), serveza::listener_state::idle);
  EXPECT_EQ(listener->status().generation, 0u);
}

TEST(LifecycleTests, CancellingWaitDoesNotStopListener)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ImmediateSession{}}; });
  net::cancellation_signal cancellation;

  bool waitCancelled = false;
  bool stopped = false;
  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
  });
  listener->async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    listener->async_wait(net::bind_cancellation_slot(cancellation.slot(), [&](sys::error_code waitEc) {
      EXPECT_EQ(waitEc, net::error::operation_aborted);
      EXPECT_EQ(listener->state(), serveza::listener_state::running);
      waitCancelled = true;
      listener->request_stop();
    }));
    cancellation.emit(net::cancellation_type::all);
  });

  io.run();

  EXPECT_TRUE(waitCancelled);
  EXPECT_TRUE(stopped);
  EXPECT_EQ(listener->state(), serveza::listener_state::stopped);
}

TEST(LifecycleTests, FactoryFailureIsObservedAndNextConnectionIsAccepted)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<RecordingObserver> recording = std::make_shared<RecordingObserver>();
  std::shared_ptr<ThrowingObserver> throwing = std::make_shared<ThrowingObserver>();
  [[maybe_unused]] serveza::observer_subscription throwingSubscription = server.observe(throwing);
  [[maybe_unused]] serveza::observer_subscription recordingSubscription = server.observe(recording);
  std::shared_ptr<std::size_t> attempts = std::make_shared<std::size_t>();
  std::shared_ptr<std::function<void()>> sessionStarted = std::make_shared<std::function<void()>>();
  std::shared_ptr<serveza::listener> listener =
      server.listen<tcp>({net::ip::address_v4::loopback(), 0}, RecoveringFactory{attempts, sessionStarted});

  tcp::socket first{io};
  tcp::socket second{io};
  server.async_wait([&](sys::error_code ec) { EXPECT_FALSE(ec); });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    const tcp::endpoint endpoint{net::ip::address_v4::loopback(), listenerPort(listener)};
    first.async_connect(endpoint, [&, endpoint](sys::error_code firstEc) {
      EXPECT_FALSE(firstEc);
      second.async_connect(endpoint, [&](sys::error_code secondEc) { EXPECT_FALSE(secondEc); });
    });
  });

  net::steady_timer deadline{io, std::chrono::seconds{2}};
  *sessionStarted = [&] {
    deadline.cancel();
    server.request_stop();
  };
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    ADD_FAILURE() << "listener did not recover after the factory exception";
    server.request_stop();
  });
  io.run();

  EXPECT_EQ(*attempts, 2u);
  const std::vector<serveza::listener_event>::const_iterator failed =
      std::find_if(recording->events.begin(), recording->events.end(), [](const serveza::listener_event& event) {
        return event.type == serveza::listener_event_type::session_factory_failed;
      });
  ASSERT_NE(failed, recording->events.end());
  ASSERT_TRUE(failed->exception);
  try {
    std::rethrow_exception(failed->exception);
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "factory failure");
  } catch (...) {
    FAIL() << "factory exception changed type";
  }

  const serveza::listener_status status = listener->status();
  EXPECT_EQ(status.state, serveza::listener_state::stopped);
  EXPECT_TRUE(status.last_error);
  EXPECT_TRUE(status.last_exception);
}

TEST(LifecycleTests, AllocationFailureDuringFactoryTriggeredShutdownCompletesOnce)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<RecordingObserver> observer = std::make_shared<RecordingObserver>();
  [[maybe_unused]] serveza::observer_subscription subscription = server.observe(observer);
  std::shared_ptr<serveza::listener> listener =
      server.listen<tcp>({net::ip::address_v4::loopback(), 0},
                         []() -> serveza::callback_session<ImmediateSession> { throw std::bad_alloc{}; });

  observer->received = [&](const serveza::listener_event& event) {
    if (event.type == serveza::listener_event_type::session_factory_failed) server.request_stop();
  };
  std::size_t waitCompletions = 0;
  tcp::socket client{io};
  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    ++waitCompletions;
  });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client.async_connect({net::ip::address_v4::loopback(), listenerPort(listener)},
                         [](sys::error_code connectEc) { EXPECT_FALSE(connectEc); });
  });
  io.run();

  EXPECT_EQ(waitCompletions, 1u);
  EXPECT_EQ(listener->status().stop_reason, serveza::listener_stop_reason::requested);
  ASSERT_TRUE(listener->status().last_exception);
  EXPECT_THROW(std::rethrow_exception(listener->status().last_exception), std::bad_alloc);
  const std::size_t failures = static_cast<std::size_t>(
      std::count_if(observer->events.begin(), observer->events.end(), [](const serveza::listener_event& event) {
        return event.type == serveza::listener_event_type::session_factory_failed;
      }));
  EXPECT_EQ(failures, 1u);
}

TEST(LifecycleTests, GraceDeadlineDrainsAnUncooperativeSession)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<RecordingObserver> observer = std::make_shared<RecordingObserver>();
  [[maybe_unused]] serveza::observer_subscription observerSubscription = server.observe(observer);
  std::shared_ptr<HangingState> state = std::make_shared<HangingState>();
  serveza::listener_options options;
  options.shutdown_grace_period = std::chrono::milliseconds{10};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [state] { return serveza::callback_session{HangingSession{state}}; },
      options);
  bool rejectedWhileStopping = false;
  state->started = [&] {
    server.request_stop();
    listener->async_start([&](sys::error_code ec) {
      EXPECT_EQ(ec, net::error::operation_aborted);
      rejectedWhileStopping = true;
    });
  };

  bool stopped = false;
  tcp::socket client{io};
  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
  });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client.async_connect({net::ip::address_v4::loopback(), listenerPort(listener)},
                         [&](sys::error_code connectEc) { ASSERT_FALSE(connectEc); });
  });

  io.run();

  EXPECT_TRUE(stopped);
  EXPECT_TRUE(rejectedWhileStopping);
  EXPECT_EQ(listener->active_sessions(), 0u);
  ASSERT_EQ(state->completions.size(), 1u);
  const std::size_t forced = static_cast<std::size_t>(
      std::count_if(observer->events.begin(), observer->events.end(), [](const serveza::listener_event& event) {
        return event.type == serveza::listener_event_type::shutdown_forced;
      }));
  EXPECT_EQ(forced, 1u);
  std::move(state->completions.front())(sys::error_code{});
}

TEST(LifecycleTests, ForcedShutdownKeepsYieldSessionStorageUntilCompletion)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<DelayedSessionState> state = std::make_shared<DelayedSessionState>();
  serveza::listener_options options;
  options.shutdown_grace_period = std::chrono::milliseconds{1};
  std::shared_ptr<serveza::listener> listener;
  state->started = [&] { listener->request_stop(); };
  listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0},
      [state] {
        return serveza::yield_session{[state](serveza::session_context<tcp>& ctx, net::yield_context yield) {
          yield.reset_cancellation_state(net::disable_cancellation());
          state->started();
          net::steady_timer timer{ctx.get_executor()};
          timer.expires_after(std::chrono::milliseconds{20});
          sys::error_code ec;
          timer.async_wait(yield[ec]);
          state->completed = !ec;
        }};
      },
      options);

  bool stopped = false;
  tcp::socket client{io};
  listener->async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
  });
  listener->async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client.async_connect({net::ip::address_v4::loopback(), listenerPort(listener)},
                         [](sys::error_code connectEc) { EXPECT_FALSE(connectEc); });
  });

  io.run();

  EXPECT_TRUE(stopped);
  EXPECT_TRUE(state->completed);
  EXPECT_EQ(listener->active_sessions(), 0u);
  EXPECT_EQ(listener->status().last_error, make_error_code(sys::errc::timed_out));
}

} // namespace
