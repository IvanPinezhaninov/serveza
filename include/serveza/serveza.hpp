/******************************************************************************
**
** Copyright (C) 2026 Ivan Pinezhaninov <ivan.pinezhaninov@gmail.com>
**
** This file is part of the serveza - which can be found at
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

#ifndef SERVEZA_H
#define SERVEZA_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/defer.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/asio/strand.hpp>

namespace serveza {

/**
 * @brief Lifecycle states for long-running components.
 *
 * The state machine is linear:
 *   stopped → starting → started → stopping → stopped
 */
enum class lifecycle_state : std::uint8_t { stopped, starting, started, stopping };

/**
 * @brief Callback type invoked on lifecycle state changes.
 */
using lifecycle_callback = std::function<void(lifecycle_state)>;

/**
 * @brief Exception aggregating multiple failures into a single throwable.
 *
 * Useful when several operations are attempted and more than one fails.
 * Each underlying failure is captured as an @c std::exception_ptr in @ref exceptions.
 */
struct multiple_exception final : public std::exception {
  /// Collected exception pointers describing individual failures.
  std::vector<std::exception_ptr> exceptions;

  explicit multiple_exception(std::vector<std::exception_ptr> e)
    : exceptions{std::move(e)}
  {}

  /// @return Static message indicating that multiple failures occurred.
  const char* what() const noexcept override
  {
    return "Multiple exceptions occurred";
  }
};

/**
 * @brief Abstract per-session handler for a given Boost.Asio protocol.
 *
 * Specialize this template for a concrete Boost.Asio protocol (e.g. @c ip::tcp
 * or @c ip::udp) and implement the call operator to handle a single session’s
 * lifecycle.
 *
 * @tparam Protocol A Boost.Asio protocol type providing a @c socket type.
 */
template<typename Protocol>
class session {
public:
  using protocol_type = Protocol;

  session() = default;
  session(const session&) = default;
  session(session&&) = default;
  session& operator=(const session&) = default;
  session& operator=(session&&) = default;
  virtual ~session() = default;

  /**
   * @brief Handle a single session.
   *
   * Invoked by the endpoint listener to process one logical session.
   *
   * @param socket A connected or bound socket instance for @ref protocol_type.
   * @param yield  Coroutine token for @c async_* operations.
   */
  virtual void operator()(typename protocol_type::socket socket, boost::asio::yield_context yield) = 0;
};

/**
 * @brief Abstract interface for a long-running endpoint listener.
 *
 * A listener owns the bind/accept loop and orchestrates per-session
 * @ref session instances.
 */
struct listener {
  listener() = default;
  listener(const listener&) = default;
  listener(listener&&) = default;
  listener& operator=(const listener&) = default;
  listener& operator=(listener&&) = default;
  virtual ~listener() = default;

  /**
   * @brief Start the listener.
   */
  virtual void start() = 0;

  /**
   * @brief Stop the listener.
   */
  virtual void stop() = 0;

  /**
   * @brief Get whether the listener is currently started.
   *
   * @return @c true if the listener started, otherwise @c false.
   */
  [[nodiscard]]
  virtual bool is_started() const = 0;

  /**
   * @brief Get the current lifecycle state.
   *
   * @return The listener’s @ref lifecycle_state.
   */
  [[nodiscard]]
  virtual lifecycle_state get_state() const = 0;

  /**
   * @brief Register a callback for lifecycle state changes.
   *
   * @param cb Function to be invoked with the new @ref lifecycle_state.
   */
  virtual void on_state_changed(lifecycle_callback cb) = 0;

  /**
   * @brief Register an error notification callback.
   *
   * @param cb Function receiving an exception pointer and a short context tag.
   */
  virtual void on_error(std::function<void(std::exception_ptr, std::string_view where)> cb) = 0;
};

[[nodiscard]]
inline std::pair<std::size_t, boost::asio::cancellation_signal&>
emplace_cancellation(std::size_t& cancels_key_seq,
                     std::unordered_map<std::size_t, boost::asio::cancellation_signal>& cancels)
{
  while (true) {
    if (cancels_key_seq == std::numeric_limits<std::size_t>::max()) cancels_key_seq = 0;
    auto [it, ok] = cancels.try_emplace(++cancels_key_seq);
    if (ok) return {it->first, it->second};
  }
}

namespace details {

template<typename T, typename = void>
struct has_acceptor : std::false_type {};

template<typename T>
struct has_acceptor<T, std::void_t<typename T::acceptor>> : std::true_type {};

template<typename Derived, typename Session, typename... Args>
class basic_listener : public listener, public std::enable_shared_from_this<Derived> {
public:
  using protocol_type = typename Session::protocol_type;
  using endpoint_type = typename protocol_type::endpoint;

  basic_listener(boost::asio::any_io_executor ex, endpoint_type endpoint, Args&&... args)
    : m_ex{std::move(ex)}
    , m_endpoint{std::move(endpoint)}
    , m_args{std::forward_as_tuple(std::forward<Args>(args)...)}
  {}

  void start() final
  {
    if (!can_start()) return;

    namespace net = boost::asio;
    net::spawn(
        m_strand,
        [self = this->shared_from_this()](net::yield_context yield) {
          if (!self->can_start()) return;
          self->set_state(lifecycle_state::starting);
          try {
            static_cast<Derived&>(*self.get()).start_impl(yield);
          } catch (...) {
            self->emit_error(std::current_exception(), "listener::start()");
            self->set_state(lifecycle_state::stopped);
          }
        },
        net::bind_cancellation_slot(m_sig.slot(), net::detached));
  }

  void stop() final
  {
    if (!can_stop()) return;

    namespace net = boost::asio;
    net::dispatch(m_strand, [self = this->shared_from_this()] {
      if (!self->can_stop()) return;
      self->set_state(lifecycle_state::stopping);
      try {
        static_cast<Derived&>(*self.get()).stop_impl();
      } catch (...) {
        self->emit_error(std::current_exception(), "listener::stop()");
        self->set_state(lifecycle_state::stopped);
      }
    });
  }

  bool is_started() const noexcept final
  {
    return get_state() == lifecycle_state::started;
  }

  lifecycle_state get_state() const noexcept final
  {
    return m_state.load(std::memory_order_relaxed);
  }

  void on_state_changed(lifecycle_callback cb) final
  {
    if (!cb) return;
    namespace net = boost::asio;
    net::dispatch(m_strand, [self = this->shared_from_this(), cb = std::move(cb)]() {
      self->m_state_cbs.push_back(std::move(cb));
    });
  }

  void on_error(std::function<void(std::exception_ptr, std::string_view)> cb) final
  {
    if (!cb) return;
    namespace net = boost::asio;
    net::dispatch(m_strand, [self = this->shared_from_this(), cb = std::move(cb)]() {
      self->m_error_cbs.push_back(std::move(cb));
    });
  }

protected:
  basic_listener() = default;

  void emit_error(std::exception_ptr ex, std::string where) noexcept
  {
    boost::asio::defer(m_strand, [this, ex, where = std::move(where)] {
      for (auto& cb : m_error_cbs) {
        try {
          cb(ex, where);
        } catch (...) {}
      }
    });
  }

  void emit_error(const boost::system::error_code& ec, std::string where) noexcept
  {
    emit_error(std::make_exception_ptr(boost::system::system_error{ec}), std::move(where));
  }

  void start_impl(boost::asio::yield_context)
  {
    static_assert(!sizeof(Derived), "start_impl is required");
  }

  void stop_impl()
  {
    static_assert(!sizeof(Derived), "stop_impl is required");
  }

  void set_state(lifecycle_state state) noexcept
  {
    lifecycle_state prev_state = m_state.exchange(state, std::memory_order_relaxed);
    if (prev_state == state) return;

    boost::asio::defer(m_ex, [state, cbs = m_state_cbs] {
      for (auto& cb : cbs) {
        try {
          cb(state);
        } catch (...) {}
      }
    });
  }

  bool can_start() const noexcept
  {
    return get_state() == lifecycle_state::stopped;
  }

  bool can_stop() const noexcept
  {
    lifecycle_state state = get_state();
    return state == lifecycle_state::starting || state == lifecycle_state::started;
  }

  boost::asio::any_io_executor m_ex;
  endpoint_type m_endpoint;
  std::tuple<Args...> m_args;
  boost::asio::strand<boost::asio::any_io_executor> m_strand{boost::asio::make_strand(m_ex)};
  boost::asio::cancellation_signal m_sig;
  std::vector<std::function<void(std::exception_ptr, std::string_view)>> m_error_cbs;
  std::vector<lifecycle_callback> m_state_cbs;
  std::atomic<lifecycle_state> m_state{lifecycle_state::stopped};
};

template<typename Session, typename... Args>
class socket_listener final : public basic_listener<socket_listener<Session, Args...>, Session, Args...> {
  using base = basic_listener<socket_listener<Session, Args...>, Session, Args...>;
  using socket_type = typename Session::protocol_type::socket;

public:
  using base::base;

protected:
  void start_impl(boost::asio::yield_context yield)
  {
    namespace sys = boost::system;

    socket_type socket{this->m_strand};

    sys::error_code ec;
    socket.open(this->m_endpoint.protocol(), ec);
    if (ec) return this->emit_error(ec, "socket::open()");

    socket.bind(this->m_endpoint, ec);
    if (ec) return this->emit_error(ec, "socket::bind()");

    this->set_state(lifecycle_state::started);

    try {
      std::make_from_tuple<Session>(this->m_args)(std::move(socket), yield);
    } catch (...) {
      this->emit_error(std::current_exception(), "session::operator()");
    }

    this->set_state(lifecycle_state::stopped);
  }

  void stop_impl()
  {
    this->m_sig.emit(boost::asio::cancellation_type::all);
  }

  template<typename, typename, typename...>
  friend class basic_listener;
};

template<typename Session, typename... Args>
class acceptor_listener final : public basic_listener<acceptor_listener<Session, Args...>, Session, Args...> {
  using base = basic_listener<acceptor_listener<Session, Args...>, Session, Args...>;
  using acceptor_type = typename Session::protocol_type::acceptor;
  using socket_type = typename Session::protocol_type::socket;

public:
  using base::base;

protected:
  void start_impl(boost::asio::yield_context yield)
  {
    namespace net = boost::asio;
    namespace sys = boost::system;

    sys::error_code ec;
    acceptor_type acceptor{this->m_strand, this->m_endpoint};
    this->set_state(lifecycle_state::started);

    while (yield.cancelled() == net::cancellation_type::none) {
      socket_type socket{net::make_strand(this->m_ex)};
      acceptor.async_accept(socket, yield[ec]);
      if (ec) {
        if (ec == net::error::operation_aborted) break;
        this->emit_error(ec, "acceptor::async_accept()");
        continue;
      }

      auto ex = socket.get_executor();
      auto [key, cancel] = emplace_cancellation(m_cancels_key_seq, m_cancels);

      net::spawn(
          ex,
          [self = this->shared_from_this(), socket = std::move(socket), key = key](net::yield_context yield) mutable {
            try {
              std::make_from_tuple<Session>(self->m_args)(std::move(socket), yield);
            } catch (...) {
              self->emit_error(std::current_exception(), "session::operator()");
            }

            auto slot = yield.get_cancellation_slot();
            slot.clear();

            self->async_erase_cancellation(key, yield);
          },
          net::bind_cancellation_slot(cancel.slot(), net::detached));
    }

    for (auto& [_, cancel] : m_cancels)
      cancel.emit(net::cancellation_type::all);
  }

  void stop_impl()
  {
    namespace net = boost::asio;
    this->m_sig.emit(net::cancellation_type::all);
    if (this->m_cancels.empty()) this->set_state(lifecycle_state::stopped);
    for (auto& [_, sig] : this->m_cancels)
      sig.emit(net::cancellation_type::all);
  }

private:
  template<typename CompletionToken>
  void async_erase_cancellation(std::size_t key, CompletionToken&& token)
  {
    namespace net = boost::asio;
    return net::async_initiate<CompletionToken, void()>(
        [self = this->shared_from_this(), key](auto handler) mutable {
          net::defer(self->m_strand, [self, key, handler = std::move(handler)]() mutable {
            self->m_cancels.erase(key);
            if (self->m_cancels.empty() && self->m_state == lifecycle_state::stopping)
              self->set_state(lifecycle_state::stopped);
            handler();
          });
        },
        token);
  }

  std::size_t m_cancels_key_seq{0};
  std::unordered_map<std::size_t, boost::asio::cancellation_signal> m_cancels;

  template<typename, typename, typename...>
  friend class basic_listener;
};

} // namespace details

/**
 * @brief Factory for creating concrete @ref listener instances.
 *
 * The factory binds created listeners to a given executor so they can
 * dispatch/post work consistently within the desired execution context.
 */
class listener_factory {
public:
  explicit listener_factory(boost::asio::any_io_executor ex)
    : m_ex{std::move(ex)}
  {}

  listener_factory(const listener_factory&) = default;
  listener_factory(listener_factory&&) = default;
  listener_factory& operator=(const listener_factory&) = default;
  listener_factory& operator=(listener_factory&&) = default;
  virtual ~listener_factory() = default;

  /**
   * @brief Create a listener for the specified @p Session and ep.
   *
   * @tparam Session A @ref session specialization.
   * @tparam Args    Argument pack forwarded to the @p Session constructor.
   *
   * @param ep       Network endpoint to bind/accept on.
   * @param args     Arguments used to construct the @p session instance.
   *
   * @return A shared pointer to the newly created @ref listener.
   */

  template<typename Session, typename... Args>
  [[nodiscard]]
  std::shared_ptr<listener> make_listener(typename Session::protocol_type::endpoint ep, Args&&... args)
  {
    static_assert(std::is_constructible_v<Session, Args...>, "Session must be constructible from the given arguments");

    if constexpr (details::has_acceptor<typename Session::protocol_type>::value) {
      using listener_type = details::acceptor_listener<Session, Args...>;
      return std::make_shared<listener_type>(m_ex, std::move(ep), std::forward<Args>(args)...);
    }

    using listener_type = details::socket_listener<Session, Args...>;
    return std::make_shared<listener_type>(m_ex, std::move(ep), std::forward<Args>(args)...);
  }

private:
  boost::asio::any_io_executor m_ex;
};

/**
 * @brief Server that wraps a Boost.Asio io_context and a listener factory.
 *
 * The server manages the execution context, spawns I/O threads, and coordinates
 * lifecycle transitions while notifying registered observers.
 */
class server {
public:
  explicit server(unsigned int threads_count = 1)
    : m_threads_count{std::max(1u, threads_count)}
    , m_impl{std::in_place, static_cast<int>(m_threads_count)}
    , m_ioc{m_impl.value()}
  {}

  explicit server(boost::asio::io_context& ioc, unsigned int threads_count = 1)
    : m_threads_count{std::max(1u, threads_count)}
    , m_ioc{ioc}
  {}

  server(const server&) = delete;
  server(server&&) = delete;
  server& operator=(const server&) = delete;
  server& operator=(server&&) = delete;
  virtual ~server() = default;

  /**
   * @brief Create and register a listener for the specified @p Session and endpoint.
   *
   * @tparam Session A @ref session specialization.
   * @tparam Args    Constructor arguments forwarded to @p Session.
   * @param ep       Network endpoint to bind/accept on.
   * @param args     Arguments used to construct the @p session instance.
   * @return Shared pointer to the created @ref listener.
   */
  template<typename Session, typename... Args, typename = std::enable_if_t<std::is_constructible_v<Session, Args...>>>
  [[nodiscard]]
  std::shared_ptr<listener> make_listener(typename Session::protocol_type::endpoint ep, Args&&... args)
  {
    std::shared_ptr<listener> listener = m_factory.make_listener<Session>(std::move(ep), std::forward<Args>(args)...);

    std::lock_guard lk{m_listeners_mutex};
    m_listeners.push_back(listener);

    return listener;
  }

  /**
   * @brief Starts the server and blocks until the I/O context stops.
   */
  void start()
  {
    namespace net = boost::asio;

    if (!try_transition(lifecycle_state::stopped, lifecycle_state::starting)) return;

    m_work_guard.emplace(net::make_work_guard(m_ioc));

    net::post(m_ioc, [this] { try_transition(lifecycle_state::starting, lifecycle_state::started); });

    unsigned int threads_count = m_threads_count - 1;
    std::vector<std::thread> threads;
    threads.reserve(threads_count);

    std::vector<std::exception_ptr> exceptions;
    std::mutex exceptions_mutex;

    for (unsigned int i = 0; i < threads_count; ++i) {
      threads.emplace_back([this, &exceptions, &exceptions_mutex] {
        try {
          m_ioc.run();
        } catch (...) {
          std::lock_guard lk{exceptions_mutex};
          exceptions.push_back(std::current_exception());
        }
      });
    }

    try {
      m_ioc.run();
    } catch (...) {
      std::lock_guard lk{exceptions_mutex};
      exceptions.push_back(std::current_exception());
    }

    for (std::thread& thread : threads) {
      if (thread.joinable()) thread.join();
    }

    set_state(lifecycle_state::stopped);

    if (exceptions.empty()) return;
    if (exceptions.size() == 1) std::rethrow_exception(exceptions.front());
    throw multiple_exception{std::move(exceptions)};
  }

  /**
   * @brief Initiate server shutdown.
   */
  void stop()
  {
    if (!try_transition({lifecycle_state::starting, lifecycle_state::started}, lifecycle_state::stopping)) return;

    std::vector<std::shared_ptr<listener>> listeners;
    {
      std::lock_guard lk{m_listeners_mutex};
      listeners.reserve(m_listeners.size());
      for (auto& weak : m_listeners)
        if (auto listener = weak.lock()) listeners.push_back(std::move(listener));
    }

    for (auto& listener : listeners)
      listener->stop();

    m_work_guard.reset();
  }

  /**
   * @brief Get whether the server is currently started.
   *
   * @return @c true if the server is started, otherwise @c false.
   */
  [[nodiscard]]
  bool is_started() const noexcept
  {
    return get_state() == lifecycle_state::started;
  }

  /**
   * @brief Get the current lifecycle state.
   *
   * @return The server’s @ref lifecycle_state.
   */
  [[nodiscard]]
  lifecycle_state get_state() const noexcept
  {
    return m_state.load(std::memory_order_relaxed);
  }

  /**
   * @brief Register a callback for lifecycle state changes.
   *
   * @param cb Function to be invoked with the new @ref lifecycle_state.
   */
  void on_state_changed(lifecycle_callback cb)
  {
    if (!cb) return;
    std::lock_guard lk{m_state_cbs_mutex};
    m_state_cbs.push_back(std::move(cb));
  }

  /**
   * @brief Obtain the server's executor.
   */
  [[nodiscard]]
  boost::asio::any_io_executor get_executor() const noexcept
  {
    return m_ioc.get_executor();
  }

private:
  bool try_transition(std::initializer_list<lifecycle_state> from, lifecycle_state to)
  {
    std::vector<lifecycle_callback> cbs;
    {
      if (std::find(from.begin(), from.end(), get_state()) == from.end()) return false;
      m_state.store(to, std::memory_order_relaxed);
      std::lock_guard lk{m_state_cbs_mutex};
      cbs = m_state_cbs;
    }

    call_lifecycle_cbs(cbs, to);

    return true;
  }

  bool try_transition(lifecycle_state from, lifecycle_state to)
  {
    return try_transition({from}, to);
  }

  void set_state(lifecycle_state state)
  {
    std::vector<lifecycle_callback> cbs;
    {
      if (get_state() == state) return;
      m_state.store(state, std::memory_order_relaxed);
      std::lock_guard lk{m_state_cbs_mutex};
      cbs = m_state_cbs;
    }

    call_lifecycle_cbs(cbs, state);
  }

  void call_lifecycle_cbs(std::vector<lifecycle_callback>& cbs, lifecycle_state state) noexcept
  {
    try {
      for (auto& cb : cbs)
        cb(state);
    } catch (...) {}
  }

  unsigned int m_threads_count;
  std::optional<boost::asio::io_context> m_impl;
  boost::asio::io_context& m_ioc;
  std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> m_work_guard;
  listener_factory m_factory{m_ioc.get_executor()};
  std::vector<lifecycle_callback> m_state_cbs;
  mutable std::mutex m_state_cbs_mutex;
  std::vector<std::weak_ptr<listener>> m_listeners;
  mutable std::mutex m_listeners_mutex;
  std::atomic<lifecycle_state> m_state{lifecycle_state::stopped};
};

} // namespace serveza

#endif // SERVEZA_H
