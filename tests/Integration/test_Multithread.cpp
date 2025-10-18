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
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
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

struct ParallelEcho {
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

class ConcurrentObserver final : public serveza::observer {
public:
  void on_event(const serveza::listener_event& event) override
  {
    if (m_inside.fetch_add(1, std::memory_order_acq_rel) != 0) {
      m_overlaps.fetch_add(1, std::memory_order_relaxed);
    }
    {
      std::lock_guard lock{m_mutex};
      m_threads.insert(std::this_thread::get_id());
    }
    if (event.type == serveza::listener_event_type::session_started) {
      m_started.fetch_add(1, std::memory_order_relaxed);
    } else if (event.type == serveza::listener_event_type::session_stopped) {
      m_stopped.fetch_add(1, std::memory_order_relaxed);
    }
    std::this_thread::yield();
    m_inside.fetch_sub(1, std::memory_order_release);
  }

  [[nodiscard]] std::size_t started() const noexcept
  {
    return m_started.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t stopped() const noexcept
  {
    return m_stopped.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t overlaps() const noexcept
  {
    return m_overlaps.load(std::memory_order_relaxed);
  }

private:
  std::atomic_size_t m_inside{};
  std::atomic_size_t m_overlaps{};
  std::atomic_size_t m_started{};
  std::atomic_size_t m_stopped{};
  std::mutex m_mutex;
  std::set<std::thread::id> m_threads;
};

class SerializationProbeObserver final : public serveza::observer {
public:
  void on_event(const serveza::listener_event&) override
  {
    const std::size_t active = m_active.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (active > 1) {
      m_overlaps.fetch_add(1, std::memory_order_relaxed);
      m_cv.notify_all();
    }

    bool expected = false;
    if (m_probed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      std::unique_lock lock{m_mutex};
      static_cast<void>(m_cv.wait_for(lock, std::chrono::seconds{1},
                                      [this] { return m_overlaps.load(std::memory_order_acquire) != 0; }));
    }
    m_active.fetch_sub(1, std::memory_order_acq_rel);
  }

  [[nodiscard]] std::size_t overlaps() const noexcept
  {
    return m_overlaps.load(std::memory_order_relaxed);
  }

private:
  std::atomic_size_t m_active{};
  std::atomic_size_t m_overlaps{};
  std::atomic_bool m_probed{};
  std::mutex m_mutex;
  std::condition_variable m_cv;
};

class EchoClient final : public std::enable_shared_from_this<EchoClient> {
public:
  using completion = std::function<void(bool)>;

  EchoClient(net::io_context& io, tcp::endpoint endpoint, char value, completion done)
    : m_socket{io}
    , m_endpoint{std::move(endpoint)}
    , m_value{value}
    , m_done{std::move(done)}
  {}

  void start()
  {
    std::shared_ptr<EchoClient> self = shared_from_this();
    m_socket.async_connect(m_endpoint, [self](sys::error_code ec) {
      if (ec) return self->finish(false);
      net::async_write(self->m_socket, net::buffer(&self->m_value, 1), [self](sys::error_code writeEc, std::size_t) {
        if (writeEc) return self->finish(false);
        net::async_read(self->m_socket, net::buffer(&self->m_reply, 1), [self](sys::error_code readEc, std::size_t) {
          self->finish(!readEc && self->m_reply == self->m_value);
        });
      });
    });
  }

  void cancel() noexcept
  {
    sys::error_code ignored;
    m_socket.cancel(ignored);
    m_socket.close(ignored);
  }

private:
  void finish(bool success)
  {
    if (!m_done) return;
    completion done = std::move(m_done);
    done(success);
  }

  tcp::socket m_socket;
  tcp::endpoint m_endpoint;
  char m_value{};
  char m_reply{};
  completion m_done;
};

class SequentialEchoClient final : public std::enable_shared_from_this<SequentialEchoClient> {
public:
  using completion = std::function<void(bool)>;

  SequentialEchoClient(net::io_context& io, tcp::endpoint endpoint, std::size_t count, completion done)
    : m_io{io}
    , m_endpoint{std::move(endpoint)}
    , m_count{count}
    , m_done{std::move(done)}
  {}

  void start()
  {
    connect();
  }

private:
  void connect()
  {
    m_socket = std::make_unique<tcp::socket>(m_io);
    std::shared_ptr<SequentialEchoClient> self = shared_from_this();
    m_socket->async_connect(m_endpoint, [self](sys::error_code ec) {
      if (ec) return self->finish(false);
      self->m_value = static_cast<char>('a' + self->m_i % 26);
      net::async_write(*self->m_socket, net::buffer(&self->m_value, 1), [self](sys::error_code writeEc, std::size_t) {
        if (writeEc) return self->finish(false);
        self->read();
      });
    });
  }

  void read()
  {
    std::shared_ptr<SequentialEchoClient> self = shared_from_this();
    net::async_read(*m_socket, net::buffer(&m_reply, 1), [self](sys::error_code ec, std::size_t) {
      if (ec || self->m_reply != self->m_value) return self->finish(false);
      sys::error_code ignored;
      self->m_socket->close(ignored);
      ++self->m_i;
      if (self->m_i == self->m_count) return self->finish(true);
      self->connect();
    });
  }

  void finish(bool success)
  {
    if (!m_done) return;
    completion done = std::move(m_done);
    done(success);
  }

  net::io_context& m_io;
  tcp::endpoint m_endpoint;
  std::size_t m_count{};
  completion m_done;
  std::unique_ptr<tcp::socket> m_socket;
  std::size_t m_i{};
  char m_value{};
  char m_reply{};
};

TEST(MultithreadTests, ParallelConnectionsKeepListenerBookkeepingSerialized)
{
  constexpr std::size_t workerCount = 4;
  constexpr std::size_t clientCount = 32;

  net::thread_pool pool{workerCount};
  serveza::server server{pool.get_executor()};
  std::shared_ptr<ConcurrentObserver> observer = std::make_shared<ConcurrentObserver>();
  [[maybe_unused]] serveza::observer_subscription observerSubscription = server.observe(observer);
  std::shared_ptr<std::atomic_size_t> created = std::make_shared<std::atomic_size_t>();
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>({net::ip::address_v4::loopback(), 0}, [created] {
    created->fetch_add(1, std::memory_order_relaxed);
    return serveza::callback_session{ParallelEcho{}};
  });

  std::promise<sys::error_code> startedPromise;
  std::future<sys::error_code> started = startedPromise.get_future();
  std::promise<sys::error_code> stoppedPromise;
  std::future<sys::error_code> stopped = stoppedPromise.get_future();
  server.async_wait([&](sys::error_code ec) { stoppedPromise.set_value(ec); });
  server.async_start([&](sys::error_code ec) { startedPromise.set_value(ec); });

  ASSERT_EQ(started.wait_for(std::chrono::seconds{2}), std::future_status::ready);
  const sys::error_code startEc = started.get();
  ASSERT_FALSE(startEc) << startEc.message();

  net::io_context clientIo;
  net::steady_timer deadline{clientIo, std::chrono::seconds{3}};
  std::vector<std::shared_ptr<EchoClient>> clients;
  clients.reserve(clientCount);
  std::size_t finished = 0;
  std::size_t succeeded = 0;
  const tcp::endpoint endpoint{net::ip::address_v4::loopback(), listenerPort(listener)};
  for (std::size_t i = 0; i < clientCount; ++i) {
    const char value = static_cast<char>('a' + i % 26);
    std::shared_ptr<EchoClient> client = std::make_shared<EchoClient>(clientIo, endpoint, value, [&](bool success) {
      if (success) ++succeeded;
      ++finished;
      if (finished == clientCount) deadline.cancel();
    });
    clients.push_back(client);
    client->start();
  }
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    for (const std::shared_ptr<EchoClient>& client : clients)
      client->cancel();
  });

  clientIo.run();
  server.request_stop();
  ASSERT_EQ(stopped.wait_for(std::chrono::seconds{2}), std::future_status::ready);
  const sys::error_code stopEc = stopped.get();
  pool.join();

  EXPECT_FALSE(stopEc) << stopEc.message();
  EXPECT_EQ(finished, clientCount);
  EXPECT_EQ(succeeded, clientCount);
  EXPECT_EQ(created->load(std::memory_order_relaxed), clientCount);
  EXPECT_EQ(observer->started(), clientCount);
  EXPECT_EQ(observer->stopped(), clientCount);
  EXPECT_EQ(observer->overlaps(), 0u);
  EXPECT_EQ(listener->status().state, serveza::listener_state::stopped);
  EXPECT_EQ(listener->active_sessions(), 0u);
}

TEST(MultithreadTests, ObserverCallbacksAreGloballySerializedAcrossListeners)
{
  constexpr std::size_t workerCount = 4;
  net::thread_pool pool{workerCount};
  serveza::server server{pool.get_executor()};
  std::shared_ptr<SerializationProbeObserver> observer = std::make_shared<SerializationProbeObserver>();
  [[maybe_unused]] serveza::observer_subscription subscription = server.observe(observer);
  std::shared_ptr<serveza::listener> first = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ParallelEcho{}}; });
  std::shared_ptr<serveza::listener> second = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ParallelEcho{}}; });

  first->async_start([first](sys::error_code ec) {
    EXPECT_FALSE(ec);
    first->request_stop();
  });
  second->async_start([second](sys::error_code ec) {
    EXPECT_FALSE(ec);
    second->request_stop();
  });
  pool.join();

  EXPECT_EQ(observer->overlaps(), 0u);
  EXPECT_EQ(first->state(), serveza::listener_state::stopped);
  EXPECT_EQ(second->state(), serveza::listener_state::stopped);
}

TEST(MultithreadTests, HandlesOneThousandSequentialConnections)
{
  constexpr std::size_t workerCount = 4;
  constexpr std::size_t connectionCount = 1000;
  net::thread_pool pool{workerCount};
  serveza::server server{pool.get_executor()};
  std::shared_ptr<std::atomic_size_t> created = std::make_shared<std::atomic_size_t>();
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>({net::ip::address_v4::loopback(), 0}, [created] {
    created->fetch_add(1, std::memory_order_relaxed);
    return serveza::callback_session{ParallelEcho{}};
  });

  std::promise<sys::error_code> startedPromise;
  std::future<sys::error_code> started = startedPromise.get_future();
  std::promise<sys::error_code> stoppedPromise;
  std::future<sys::error_code> stopped = stoppedPromise.get_future();
  server.async_wait([&](sys::error_code ec) { stoppedPromise.set_value(ec); });
  server.async_start([&](sys::error_code ec) { startedPromise.set_value(ec); });
  ASSERT_EQ(started.wait_for(std::chrono::seconds{2}), std::future_status::ready);
  ASSERT_FALSE(started.get());

  net::io_context clientIo;
  bool succeeded = false;
  std::shared_ptr<SequentialEchoClient> client = std::make_shared<SequentialEchoClient>(
      clientIo, tcp::endpoint{net::ip::address_v4::loopback(), listenerPort(listener)}, connectionCount,
      [&](bool success) { succeeded = success; });
  client->start();
  clientIo.run();

  server.request_stop();
  ASSERT_EQ(stopped.wait_for(std::chrono::seconds{5}), std::future_status::ready);
  EXPECT_FALSE(stopped.get());
  pool.join();

  EXPECT_TRUE(succeeded);
  EXPECT_EQ(created->load(std::memory_order_relaxed), connectionCount);
  EXPECT_EQ(listener->active_sessions(), 0u);
}

TEST(MultithreadTests, ConcurrentStopRequestsCompleteOnce)
{
  constexpr std::size_t workerCount = 4;
  constexpr std::size_t requestCount = 64;
  net::thread_pool pool{workerCount};
  serveza::server server{pool.get_executor()};
  server.listen<tcp>({net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ParallelEcho{}}; });

  std::promise<sys::error_code> startedPromise;
  std::future<sys::error_code> started = startedPromise.get_future();
  std::promise<sys::error_code> stoppedPromise;
  std::future<sys::error_code> stopped = stoppedPromise.get_future();
  std::atomic_size_t waitCompletions{};
  server.async_wait([&](sys::error_code ec) {
    waitCompletions.fetch_add(1, std::memory_order_relaxed);
    stoppedPromise.set_value(ec);
  });
  server.async_start([&](sys::error_code ec) { startedPromise.set_value(ec); });
  ASSERT_EQ(started.wait_for(std::chrono::seconds{2}), std::future_status::ready);
  ASSERT_FALSE(started.get());

  net::thread_pool callers{workerCount};
  for (std::size_t i = 0; i < requestCount; ++i)
    net::post(callers, [&server] { server.request_stop(); });
  callers.join();

  ASSERT_EQ(stopped.wait_for(std::chrono::seconds{2}), std::future_status::ready);
  EXPECT_FALSE(stopped.get());
  pool.join();
  EXPECT_EQ(waitCompletions.load(std::memory_order_relaxed), 1u);
  EXPECT_EQ(server.status().state, serveza::server_state::stopped);
}

} // namespace
