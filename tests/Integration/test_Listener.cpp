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
#include <atomic>
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

namespace {

unsigned short listenerPort(const std::shared_ptr<serveza::listener>& listener)
{
  const std::string endpoint = listener->endpoint();
  return static_cast<unsigned short>(std::stoul(endpoint.substr(endpoint.rfind(':') + 1)));
}

template<typename Predicate>
bool driveUntil(net::io_context& io, Predicate predicate, const bool& timedOut)
{
  while (!predicate() && !timedOut) {
    if (io.stopped()) io.restart();
    if (io.run_one() == 0) break;
  }
  return predicate();
}

struct HoldingState {
  std::size_t started{};
  std::vector<serveza::listener::completion_handler> completions;
};

struct HoldingSession {
  std::shared_ptr<HoldingState> state;

  template<typename Context, typename Handler>
  void operator()(Context&, Handler handler) const
  {
    ++state->started;
    state->completions.emplace_back(std::move(handler));
  }
};

struct FactoryState {
  std::atomic_size_t created{};
  std::atomic_size_t started{};
  std::atomic_uint64_t serialSum{};
};

struct FactorySession {
  std::shared_ptr<FactoryState> state;
  std::uint64_t serial{};

  template<typename Context, typename Handler>
  void operator()(Context&, Handler handler) const
  {
    state->serialSum.fetch_add(serial, std::memory_order_relaxed);
    state->started.fetch_add(1, std::memory_order_release);
    handler(sys::error_code{});
  }
};

struct ExecutorTargetState {
  bool started{};
  bool concreteStrand{};
  bool erasedStrand{};
};

struct ExecutorTargetSession {
  std::shared_ptr<ExecutorTargetState> state;

  template<typename Context, typename Handler>
  void operator()(Context& ctx, Handler handler) const
  {
    const net::any_io_executor executor = ctx.socket().get_executor();
    state->concreteStrand = executor.template target<net::strand<net::io_context::executor_type>>() != nullptr;
    state->erasedStrand = executor.template target<net::strand<net::any_io_executor>>() != nullptr;
    state->started = true;
    handler(sys::error_code{});
  }
};

class MoveOnlyFactory final {
public:
  explicit MoveOnlyFactory(std::shared_ptr<FactoryState> state)
    : m_state{std::move(state)}
    , m_lifetime{std::make_unique<int>(42)}
  {}

  MoveOnlyFactory(const MoveOnlyFactory&) = delete;
  MoveOnlyFactory& operator=(const MoveOnlyFactory&) = delete;
  MoveOnlyFactory(MoveOnlyFactory&&) noexcept = default;
  MoveOnlyFactory& operator=(MoveOnlyFactory&&) noexcept = default;

  serveza::callback_session<FactorySession> operator()()
  {
    const std::uint64_t serial = m_state->created.fetch_add(1, std::memory_order_relaxed) + 1;
    return serveza::callback_session{FactorySession{m_state, serial}};
  }

private:
  std::shared_ptr<FactoryState> m_state;
  std::unique_ptr<int> m_lifetime;
};

void completeFirst(const std::shared_ptr<HoldingState>& state)
{
  serveza::listener::completion_handler completion = std::move(state->completions.front());
  state->completions.erase(state->completions.begin());
  std::move(completion)(sys::error_code{});
}

std::shared_ptr<ExecutorTargetState> observeConnectionExecutor(bool eraseExecutor)
{
  net::io_context io;
  std::unique_ptr<serveza::server> server;
  if (eraseExecutor) {
    net::any_io_executor executor = io.get_executor();
    server = std::make_unique<serveza::server>(std::move(executor));
  } else {
    server = std::make_unique<serveza::server>(io.get_executor());
  }

  std::shared_ptr<ExecutorTargetState> state = std::make_shared<ExecutorTargetState>();
  std::shared_ptr<serveza::listener> listener = server->listen<tcp>({net::ip::address_v4::loopback(), 0}, [state] {
    return serveza::callback_session{ExecutorTargetSession{state}};
  });
  bool connected = false;
  bool stopped = false;
  bool timedOut = false;
  tcp::socket client{io};
  net::steady_timer deadline{io, std::chrono::seconds{2}};

  server->async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
  });
  server->async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client.async_connect({net::ip::address_v4::loopback(), listenerPort(listener)}, [&](sys::error_code connectEc) {
      EXPECT_FALSE(connectEc);
      connected = !connectEc;
    });
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    timedOut = true;
    server->request_stop();
  });

  EXPECT_TRUE(driveUntil(io, [&] { return connected && state->started; }, timedOut));
  server->request_stop();
  EXPECT_TRUE(driveUntil(io, [&] { return stopped; }, timedOut));
  deadline.cancel();
  EXPECT_FALSE(timedOut);
  return state;
}

TEST(ListenerTests, PreservesConcreteExecutorUntilConnectionStrandCreation)
{
  const std::shared_ptr<ExecutorTargetState> state = observeConnectionExecutor(false);

  EXPECT_TRUE(state->concreteStrand);
  EXPECT_FALSE(state->erasedStrand);
}

TEST(ListenerTests, SupportsAlreadyErasedExecutorWithFallbackStrand)
{
  const std::shared_ptr<ExecutorTargetState> state = observeConnectionExecutor(true);

  EXPECT_FALSE(state->concreteStrand);
  EXPECT_TRUE(state->erasedStrand);
}

TEST(ListenerTests, ReportsCallerOwnedExecutor)
{
  net::io_context io;
  serveza::server server{io.get_executor()};

  const net::any_io_executor executor = server.get_executor();
  const net::io_context::executor_type* concreteExecutor = executor.target<net::io_context::executor_type>();
  ASSERT_NE(concreteExecutor, nullptr);
  EXPECT_EQ(*concreteExecutor, io.get_executor());
}

TEST(ListenerTests, ActiveSessionLimitAppliesBackpressure)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<HoldingState> state = std::make_shared<HoldingState>();
  serveza::listener_options options;
  options.max_active_sessions = 1;
  options.shutdown_grace_period = std::chrono::milliseconds{100};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [state] { return serveza::callback_session{HoldingSession{state}}; },
      options);

  bool started = false;
  bool stopped = false;
  bool timedOut = false;
  std::size_t connected = 0;
  sys::error_code firstEc;
  sys::error_code secondEc;
  tcp::socket first{io};
  tcp::socket second{io};
  net::steady_timer deadline{io, std::chrono::seconds{2}};

  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
  });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    started = true;
    const tcp::endpoint endpoint{net::ip::address_v4::loopback(), listenerPort(listener)};
    first.async_connect(endpoint, [&](sys::error_code connectEc) {
      firstEc = connectEc;
      ++connected;
    });
    second.async_connect(endpoint, [&](sys::error_code connectEc) {
      secondEc = connectEc;
      ++connected;
    });
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    timedOut = true;
    server.request_stop();
  });

  ASSERT_TRUE(driveUntil(io, [&] { return started && connected == 2 && state->started == 1; }, timedOut));
  EXPECT_FALSE(firstEc);
  EXPECT_FALSE(secondEc);
  io.poll();
  EXPECT_EQ(state->started, 1u);
  ASSERT_EQ(state->completions.size(), 1u);

  completeFirst(state);
  ASSERT_TRUE(driveUntil(io, [&] { return state->started == 2; }, timedOut));
  ASSERT_EQ(state->completions.size(), 1u);
  completeFirst(state);
  ASSERT_TRUE(driveUntil(io, [&] { return listener->active_sessions() == 0; }, timedOut));

  server.request_stop();
  ASSERT_TRUE(driveUntil(io, [&] { return stopped; }, timedOut));
  deadline.cancel();
  EXPECT_FALSE(timedOut);
}

TEST(ListenerTests, MoveOnlyFactoryCreatesOneSessionPerConnection)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<FactoryState> state = std::make_shared<FactoryState>();
  std::shared_ptr<serveza::listener> listener =
      server.listen<tcp>({net::ip::address_v4::loopback(), 0}, MoveOnlyFactory{state},
                         serveza::listener_options{128, std::chrono::milliseconds{100}});

  bool stopped = false;
  bool timedOut = false;
  std::size_t connected = 0;
  tcp::socket first{io};
  tcp::socket second{io};
  net::steady_timer deadline{io, std::chrono::seconds{2}};

  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
  });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    const tcp::endpoint endpoint{net::ip::address_v4::loopback(), listenerPort(listener)};
    first.async_connect(endpoint, [&](sys::error_code connectEc) {
      EXPECT_FALSE(connectEc);
      ++connected;
    });
    second.async_connect(endpoint, [&](sys::error_code connectEc) {
      EXPECT_FALSE(connectEc);
      ++connected;
    });
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    timedOut = true;
    server.request_stop();
  });

  ASSERT_TRUE(
      driveUntil(io, [&] { return connected == 2 && state->started.load(std::memory_order_acquire) == 2; }, timedOut));
  ASSERT_TRUE(driveUntil(io, [&] { return listener->active_sessions() == 0; }, timedOut));
  server.request_stop();
  ASSERT_TRUE(driveUntil(io, [&] { return stopped; }, timedOut));
  deadline.cancel();

  EXPECT_FALSE(timedOut);
  EXPECT_EQ(state->created.load(std::memory_order_relaxed), 2u);
  EXPECT_EQ(state->serialSum.load(std::memory_order_relaxed), 3u);
}

class InjectedAcceptor {
public:
  template<typename Executor>
  explicit InjectedAcceptor(const Executor& executor)
    : m_acceptor{executor}
  {}

  static void failAccepts(int count, bool fatal = false, int successes = 0) noexcept
  {
    s_failures.store(count, std::memory_order_release);
    s_fatal.store(fatal, std::memory_order_release);
    s_successes.store(successes, std::memory_order_release);
  }

  void open(const tcp& protocol, sys::error_code& error)
  {
    m_acceptor.open(protocol, error);
  }

  template<typename SocketOption>
  void set_option(const SocketOption& option, sys::error_code& error)
  {
    m_acceptor.set_option(option, error);
  }

  void bind(const tcp::endpoint& endpoint, sys::error_code& error)
  {
    m_acceptor.bind(endpoint, error);
  }

  void listen(int backlog, sys::error_code& error)
  {
    m_acceptor.listen(backlog, error);
  }

  tcp::endpoint local_endpoint(sys::error_code& error) const
  {
    return m_acceptor.local_endpoint(error);
  }

  void cancel(sys::error_code& error)
  {
    m_acceptor.cancel(error);
  }

  void close(sys::error_code& error)
  {
    m_acceptor.close(error);
  }

  template<typename CompletionToken>
  auto async_accept(tcp::socket& socket, CompletionToken&& token)
  {
    int successes = s_successes.load(std::memory_order_acquire);
    if (successes > 0 && s_successes.compare_exchange_strong(successes, successes - 1, std::memory_order_acq_rel)) {
      return m_acceptor.async_accept(socket, std::forward<CompletionToken>(token));
    }
    int failures = s_failures.load(std::memory_order_acquire);
    if (failures > 0 && s_failures.compare_exchange_strong(failures, failures - 1, std::memory_order_acq_rel)) {
      const bool fatal = s_fatal.load(std::memory_order_acquire);
      return net::async_initiate<CompletionToken, void(sys::error_code)>(
          [executor = m_acceptor.get_executor(), fatal](auto handler) mutable {
            net::post(executor, [handler = std::move(handler), fatal]() mutable {
              handler(fatal ? net::error::bad_descriptor : net::error::no_descriptors);
            });
          },
          token);
    }
    return m_acceptor.async_accept(socket, std::forward<CompletionToken>(token));
  }

private:
  inline static std::atomic_int s_failures{0};
  inline static std::atomic_bool s_fatal{false};
  inline static std::atomic_int s_successes{0};
  tcp::acceptor m_acceptor;
};

struct InjectedAcceptProtocol {
  using endpoint = tcp::endpoint;
  using socket = tcp::socket;
  using acceptor = InjectedAcceptor;
};

class ThrowingAcceptor final {
public:
  template<typename Executor>
  explicit ThrowingAcceptor(const Executor& executor)
    : m_acceptor{executor}
  {}

  void open(const tcp& protocol, sys::error_code& ec)
  {
    m_acceptor.open(protocol, ec);
  }

  template<typename SocketOption>
  void set_option(const SocketOption& option, sys::error_code& ec)
  {
    m_acceptor.set_option(option, ec);
  }

  void bind(const tcp::endpoint& endpoint, sys::error_code& ec)
  {
    m_acceptor.bind(endpoint, ec);
  }

  void listen(int backlog, sys::error_code& ec)
  {
    m_acceptor.listen(backlog, ec);
  }

  tcp::endpoint local_endpoint(sys::error_code& ec) const
  {
    return m_acceptor.local_endpoint(ec);
  }

  void cancel(sys::error_code& ec)
  {
    m_acceptor.cancel(ec);
  }

  void close(sys::error_code& ec)
  {
    m_acceptor.close(ec);
  }

  template<typename CompletionToken>
  void async_accept(tcp::socket&, CompletionToken&&)
  {
    throw std::runtime_error{"accept initiation failure"};
  }

private:
  tcp::acceptor m_acceptor;
};

struct ThrowingAcceptProtocol {
  using endpoint = tcp::endpoint;
  using socket = tcp::socket;
  using acceptor = ThrowingAcceptor;
};

struct OneShotEcho {
  template<typename Context, typename Handler>
  void operator()(Context& ctx, Handler handler) const
  {
    std::shared_ptr<std::array<char, 1>> data = std::make_shared<std::array<char, 1>>();
    typename Context::socket_type* socket = &ctx.socket();
    socket->async_read_some(
        net::buffer(*data), [socket, data, handler = std::move(handler)](sys::error_code ec, std::size_t size) mutable {
          if (ec) {
            handler(ec);
            return;
          }
          net::async_write(
              *socket, net::buffer(data->data(), size),
              [data, handler = std::move(handler)](sys::error_code writeEc, std::size_t) mutable { handler(writeEc); });
        });
  }
};

class CallbackObserver final : public serveza::observer {
public:
  void on_event(const serveza::listener_event& event) override
  {
    if (received) received(event);
  }

  std::function<void(const serveza::listener_event&)> received;
};

TEST(ListenerTests, RetriesAfterTransientAcceptFailure)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  serveza::listener_options options;
  options.accept_error_backoff = std::chrono::milliseconds{30};
  std::shared_ptr<serveza::listener> listener = server.listen<InjectedAcceptProtocol>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{OneShotEcho{}}; }, options);
  InjectedAcceptor::failAccepts(1);

  bool echoed = false;
  bool stopped = false;
  bool timedOut = false;
  char request = 'x';
  char response{};
  tcp::socket client{io};
  net::steady_timer deadline{io, std::chrono::seconds{2}};
  const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
    deadline.cancel();
  });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client.async_connect({net::ip::address_v4::loopback(), listenerPort(listener)}, [&](sys::error_code connectEc) {
      ASSERT_FALSE(connectEc);
      net::async_write(client, net::buffer(&request, 1), [&](sys::error_code writeEc, std::size_t) {
        ASSERT_FALSE(writeEc);
        net::async_read(client, net::buffer(&response, 1), [&](sys::error_code readEc, std::size_t) {
          EXPECT_FALSE(readEc);
          echoed = !readEc && response == request;
          server.request_stop();
        });
      });
    });
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    timedOut = true;
    server.request_stop();
  });

  io.run();

  EXPECT_TRUE(echoed);
  EXPECT_TRUE(stopped);
  EXPECT_FALSE(timedOut);
  EXPECT_GE(std::chrono::steady_clock::now() - begin, std::chrono::milliseconds{20});
}

TEST(ListenerTests, StopsAfterBoundedConsecutiveAcceptFailures)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  serveza::listener_options options;
  options.accept_error_backoff = std::chrono::milliseconds{1};
  options.max_accept_error_backoff = std::chrono::milliseconds{3};
  options.max_consecutive_accept_errors = 4;
  std::shared_ptr<serveza::listener> listener = server.listen<InjectedAcceptProtocol>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{OneShotEcho{}}; }, options);
  InjectedAcceptor::failAccepts(4);

  sys::error_code waitError;
  server.async_wait([&](sys::error_code ec) { waitError = ec; });
  server.async_start([](sys::error_code ec) { EXPECT_FALSE(ec); });
  io.run();

  EXPECT_EQ(waitError, net::error::no_descriptors);
  EXPECT_EQ(listener->status().stop_reason, serveza::listener_stop_reason::runtime_failure);
}

TEST(ListenerTests, StopCancelsPendingAcceptRetry)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<CallbackObserver> observer = std::make_shared<CallbackObserver>();
  [[maybe_unused]] serveza::observer_subscription subscription = server.observe(observer);
  serveza::listener_options options;
  options.accept_error_backoff = std::chrono::seconds{1};
  std::shared_ptr<serveza::listener> listener = server.listen<InjectedAcceptProtocol>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{OneShotEcho{}}; }, options);
  InjectedAcceptor::failAccepts(1);
  bool stopped = false;

  observer->received = [&](const serveza::listener_event& event) {
    if (event.type == serveza::listener_event_type::accept_failed) server.request_stop();
  };
  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = true;
  });
  server.async_start([](sys::error_code ec) { EXPECT_EQ(ec, net::error::operation_aborted); });
  io.run();

  EXPECT_TRUE(stopped);
  EXPECT_EQ(listener->status().stop_reason, serveza::listener_stop_reason::requested);
  EXPECT_EQ(listener->status().last_error, net::error::no_descriptors);
}

TEST(ListenerTests, DoesNotRetryPermanentAcceptFailure)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  serveza::listener_options options;
  options.accept_error_backoff = std::chrono::seconds{1};
  std::shared_ptr<serveza::listener> listener = server.listen<InjectedAcceptProtocol>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{OneShotEcho{}}; }, options);
  InjectedAcceptor::failAccepts(1, true);

  const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
  sys::error_code waitError;
  server.async_wait([&](sys::error_code ec) { waitError = ec; });
  server.async_start([](sys::error_code ec) { EXPECT_FALSE(ec); });
  io.run();

  EXPECT_EQ(waitError, net::error::bad_descriptor);
  EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::milliseconds{500});
  EXPECT_EQ(listener->status().stop_reason, serveza::listener_stop_reason::runtime_failure);
}

TEST(ListenerTests, FatalAcceptFailureDrainsActiveSessionBeforeWaitCompletes)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<HoldingState> state = std::make_shared<HoldingState>();
  serveza::listener_options options;
  options.shutdown_grace_period = std::chrono::seconds{1};
  std::shared_ptr<serveza::listener> listener = server.listen<InjectedAcceptProtocol>(
      {net::ip::address_v4::loopback(), 0}, [state] { return serveza::callback_session{HoldingSession{state}}; },
      options);
  InjectedAcceptor::failAccepts(1, true, 1);

  bool waited = false;
  bool timedOut = false;
  sys::error_code waitEc;
  tcp::socket client{io};
  net::steady_timer deadline{io, std::chrono::seconds{2}};
  listener->async_wait([&](sys::error_code ec) {
    waitEc = ec;
    waited = true;
  });
  listener->async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client.async_connect({net::ip::address_v4::loopback(), listenerPort(listener)},
                         [](sys::error_code connectEc) { EXPECT_FALSE(connectEc); });
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (!ec) timedOut = true;
  });

  ASSERT_TRUE(driveUntil(
      io,
      [&] {
        return listener->state() == serveza::listener_state::stopping && listener->active_sessions() == 1 &&
               state->completions.size() == 1;
      },
      timedOut));
  EXPECT_FALSE(waited);
  completeFirst(state);
  ASSERT_TRUE(driveUntil(io, [&] { return waited; }, timedOut));
  deadline.cancel();

  EXPECT_FALSE(timedOut);
  EXPECT_EQ(waitEc, net::error::bad_descriptor);
  EXPECT_EQ(listener->state(), serveza::listener_state::stopped);
  EXPECT_EQ(listener->active_sessions(), 0u);
  EXPECT_EQ(listener->status().stop_reason, serveza::listener_stop_reason::runtime_failure);
}

TEST(ListenerTests, AcceptInitiationExceptionFailsStartAndWaitWithoutEscaping)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.listen<ThrowingAcceptProtocol>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{OneShotEcho{}}; });
  sys::error_code startEc;
  sys::error_code waitEc;

  listener->async_wait([&](sys::error_code ec) { waitEc = ec; });
  listener->async_start([&](sys::error_code ec) { startEc = ec; });
  EXPECT_NO_THROW(io.run());

  const sys::error_code expected = make_error_code(sys::errc::io_error);
  EXPECT_EQ(startEc, expected);
  EXPECT_EQ(waitEc, expected);
  EXPECT_EQ(listener->state(), serveza::listener_state::stopped);
  EXPECT_EQ(listener->status().stop_reason, serveza::listener_stop_reason::startup_failure);
  ASSERT_TRUE(listener->status().last_exception);
  EXPECT_THROW(std::rethrow_exception(listener->status().last_exception), std::runtime_error);
}

TEST(ListenerTests, AcceptsIpv6LoopbackConnections)
{
  net::io_context io;
  sys::error_code probeEc;
  tcp::acceptor probe{io};
  probe.open(tcp::v6(), probeEc);
  if (!probeEc) probe.bind({net::ip::address_v6::loopback(), 0}, probeEc);
  sys::error_code closeEc;
  probe.close(closeEc);
  if (probeEc) GTEST_SKIP() << "IPv6 loopback is unavailable: " << probeEc.message();

  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener =
      server.listen<tcp>({net::ip::address_v6::loopback(), 0}, [] { return serveza::callback_session{OneShotEcho{}}; });
  tcp::socket client{io};
  char request = '6';
  char response{};
  bool echoed = false;
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
    const std::string endpoint = listener->endpoint();
    EXPECT_EQ(endpoint.front(), '[');
    EXPECT_NE(endpoint.find("]:"), std::string::npos);
    client.async_connect({net::ip::address_v6::loopback(), listenerPort(listener)}, [&](sys::error_code connectEc) {
      ASSERT_FALSE(connectEc);
      net::async_write(client, net::buffer(&request, 1), [&](sys::error_code writeEc, std::size_t) {
        ASSERT_FALSE(writeEc);
        net::async_read(client, net::buffer(&response, 1), [&](sys::error_code readEc, std::size_t) {
          EXPECT_FALSE(readEc);
          echoed = !readEc && response == request;
          server.request_stop();
        });
      });
    });
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    timedOut = true;
    server.request_stop();
  });

  io.run();

  EXPECT_TRUE(echoed);
  EXPECT_TRUE(stopped);
  EXPECT_FALSE(timedOut);
}

TEST(ListenerTests, DualStackIpv6ListenerAcceptsIpv4Connection)
{
  net::io_context io;
  sys::error_code probeEc;
  tcp::acceptor probe{io};
  probe.open(tcp::v6(), probeEc);
  if (!probeEc) probe.set_option(net::ip::v6_only{false}, probeEc);
  if (!probeEc) probe.bind({net::ip::address_v6::any(), 0}, probeEc);
  sys::error_code closeEc;
  probe.close(closeEc);
  if (probeEc) GTEST_SKIP() << "dual-stack TCP is unavailable: " << probeEc.message();

  serveza::server server{io.get_executor()};
  serveza::listener_options options;
  options.v6_only = false;
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
      {net::ip::address_v6::any(), 0}, [] { return serveza::callback_session{OneShotEcho{}}; }, options);
  tcp::socket client{io};
  char request = '4';
  char response{};
  bool echoed = false;

  server.async_wait([](sys::error_code ec) { EXPECT_FALSE(ec); });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client.async_connect({net::ip::address_v4::loopback(), listenerPort(listener)}, [&](sys::error_code connectEc) {
      ASSERT_FALSE(connectEc);
      net::async_write(client, net::buffer(&request, 1), [&](sys::error_code writeEc, std::size_t) {
        ASSERT_FALSE(writeEc);
        net::async_read(client, net::buffer(&response, 1), [&](sys::error_code readEc, std::size_t) {
          echoed = !readEc && response == request;
          server.request_stop();
        });
      });
    });
  });
  io.run();

  EXPECT_TRUE(echoed);
}

} // namespace
