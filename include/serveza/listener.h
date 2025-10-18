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

#ifndef SERVEZA_LISTENER_H
#define SERVEZA_LISTENER_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/v6_only.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/error_code.hpp>

#include <serveza/detail/observer_registry.h>
#include <serveza/export.h>
#include <serveza/listener_options.h>
#include <serveza/listener_status.h>
#include <serveza/session_context.h>

namespace serveza {

/** @brief Type-erased listener control plane with CompletionToken front ends. */
class SERVEZA_API listener : public std::enable_shared_from_this<listener> {
public:
  using completion_handler = boost::asio::any_completion_handler<void(boost::system::error_code)>;

  virtual ~listener();

  /**
   * @brief Starts or joins this listener generation.
   *
   * The completion signature is `void(boost::system::error_code)`. After
   * successful initiation, completion is scheduled exactly once and never
   * inline. State allocation or initial executor submission may throw before
   * initiation succeeds.
   */
  template<typename CompletionToken>
  auto async_start(CompletionToken&& token)
  {
    return boost::asio::async_initiate<CompletionToken, void(boost::system::error_code)>(
        [self = shared_from_this()](auto handler) mutable {
          self->do_async_start(completion_handler{std::move(handler)});
        },
        token);
  }

  /**
   * @brief Waits for the current listener generation to stop.
   *
   * The completion signature is `void(boost::system::error_code)`. Cancelling
   * this operation cancels only this wait. State allocation or initial executor
   * submission may throw before initiation succeeds.
   */
  template<typename CompletionToken>
  auto async_wait(CompletionToken&& token)
  {
    return boost::asio::async_initiate<CompletionToken, void(boost::system::error_code)>(
        [self = shared_from_this()](auto handler) mutable {
          self->do_async_wait(completion_handler{std::move(handler)});
        },
        token);
  }

  /**
   * @brief Requests idempotent cooperative shutdown from any thread.
   *
   * The caller-owned executor must remain valid and able to accept work.
   */
  virtual void request_stop() noexcept = 0;
  /** @brief Returns the stable server-assigned listener identifier. */
  [[nodiscard]] virtual std::uint64_t id() const noexcept = 0;
  /** @brief Returns the current lifecycle state. */
  [[nodiscard]] virtual listener_state state() const noexcept = 0;
  /** @brief Returns the current number of active sessions. */
  [[nodiscard]] virtual std::size_t active_sessions() const noexcept = 0;
  /** @brief Returns the printable bound endpoint, or the configured endpoint before binding. */
  [[nodiscard]] virtual std::string endpoint() const = 0;
  /** @brief Returns a thread-safe lifecycle and diagnostic snapshot. */
  [[nodiscard]] virtual listener_status status() const = 0;

private:
  virtual void do_async_start(completion_handler handler) = 0;
  virtual void do_async_wait(completion_handler handler) = 0;
};

namespace detail {

template<typename Protocol, typename = void>
struct has_acceptor : std::false_type {};

template<typename Protocol>
struct has_acceptor<Protocol, std::void_t<typename Protocol::acceptor>> : std::true_type {};

template<typename Endpoint, typename = void>
struct has_ip_endpoint : std::false_type {};

template<typename Endpoint>
struct has_ip_endpoint<Endpoint, std::void_t<decltype(std::declval<const Endpoint&>().address()),
                                             decltype(std::declval<const Endpoint&>().port())>> : std::true_type {};

template<typename Endpoint, typename = void>
struct has_path_endpoint : std::false_type {};

template<typename Endpoint>
struct has_path_endpoint<Endpoint, std::void_t<decltype(std::declval<const Endpoint&>().path())>> : std::true_type {};

template<typename Endpoint>
std::string endpoint_string(const Endpoint& endpoint)
{
  if constexpr (has_ip_endpoint<Endpoint>::value) {
    std::string address = endpoint.address().to_string();
    if (address.find(':') != std::string::npos) address = '[' + address + ']';
    return address + ":" + std::to_string(endpoint.port());
  } else if constexpr (has_path_endpoint<Endpoint>::value) {
    return endpoint.path();
  } else {
    return {};
  }
}

SERVEZA_API void complete_on(boost::asio::any_io_executor fallback, listener::completion_handler handler,
                             boost::system::error_code ec);
SERVEZA_API void complete_on_associated(listener::completion_handler handler, boost::system::error_code ec);
[[nodiscard]] SERVEZA_API listener::completion_handler track_completion_work(boost::asio::any_io_executor fallback,
                                                                             listener::completion_handler handler);
SERVEZA_API void validate_acceptor_options(const listener_options& options);
SERVEZA_API void validate_socket_options(const listener_options& options);
[[nodiscard]] SERVEZA_API bool is_transient_accept_error(const boost::system::error_code& ec) noexcept;
[[nodiscard]] SERVEZA_API std::chrono::milliseconds accept_retry_delay(const listener_options& options,
                                                                       std::size_t consecutive_errors) noexcept;

class SERVEZA_API session_control {
public:
  virtual ~session_control();
  virtual void request_stop() noexcept = 0;
  virtual void force_close() noexcept = 0;
};

} // namespace detail

template<typename Protocol, typename SessionFactory>
class basic_acceptor_listener final : public listener {
  using acceptor_type = typename Protocol::acceptor;
  using socket_type = typename Protocol::socket;
  using endpoint_type = typename Protocol::endpoint;
  using session_type = std::decay_t<std::invoke_result_t<SessionFactory&>>;
  using strand_factory_type = std::function<boost::asio::any_io_executor()>;

  struct wait_operation {
    std::uint64_t id{};
    std::shared_ptr<std::atomic_bool> cancelled;
    boost::asio::cancellation_slot cancellation;
    completion_handler handler;
  };

  class connection_state final : public detail::session_control, public std::enable_shared_from_this<connection_state> {
  public:
    connection_state(socket_type socket, session_type session, std::uint64_t listener_id, connection_info info,
                     std::weak_ptr<basic_acceptor_listener> owner)
      : m_socket{std::move(socket)}
      , m_ctx{m_socket, listener_id, std::move(info), m_cancellation.slot(),
              [this](std::exception_ptr ep) { m_ep = std::move(ep); }}
      , m_session{std::move(session)}
      , m_owner{std::move(owner)}
      , m_id{m_ctx.info().id}
    {}

    void start()
    {
      std::shared_ptr<connection_state> self = this->shared_from_this();
      try {
        m_session.async_run(
            m_ctx, boost::asio::bind_cancellation_slot(
                       m_cancellation.slot(), boost::asio::bind_executor(m_socket.get_executor(),
                                                                         [self](boost::system::error_code ec) mutable {
                                                                           self->finish(ec, std::move(self->m_ep));
                                                                         })));
      } catch (...) {
        std::exception_ptr ep = std::current_exception();
        const boost::system::error_code ec = detail::exception_to_error(ep);
        finish(ec, std::move(ep));
      }
    }

    [[nodiscard]] boost::asio::any_io_executor get_executor()
    {
      return m_socket.get_executor();
    }

    [[nodiscard]] std::uint64_t id() const noexcept
    {
      return m_id;
    }

    [[nodiscard]] const connection_info& info() const noexcept
    {
      return m_ctx.info();
    }

    void request_stop() noexcept override
    {
      auto self = this->shared_from_this();
      try {
        // GCOVR_EXCL_START: shutdown tests execute this posted lambda; GCC duplicates its template mapping.
        boost::asio::post(m_socket.get_executor(), [self] {
          self->m_cancellation.emit(boost::asio::cancellation_type::all);
          boost::system::error_code ignored;
          self->m_socket.cancel(ignored);
        });
        // GCOVR_EXCL_STOP
      } catch (...) { // GCOVR_EXCL_START: requires executor or allocation failure in a noexcept shutdown boundary
        force_close();
        // GCOVR_EXCL_STOP
      }
    }

    void force_close() noexcept override
    {
      auto self = this->shared_from_this();
      try {
        boost::asio::post(m_socket.get_executor(), [self] { self->finish(boost::asio::error::operation_aborted, {}); });
      } catch (...) { // GCOVR_EXCL_START: requires executor or allocation failure in a noexcept shutdown boundary
        finish(boost::asio::error::operation_aborted, {});
        // GCOVR_EXCL_STOP
      }
    }

  private:
    void finish(boost::system::error_code ec, std::exception_ptr ep) noexcept
    {
      if (m_finished) return;
      m_finished = true;
      close_now();
      try {
        if (std::shared_ptr<basic_acceptor_listener> owner = m_owner.lock()) {
          owner->session_finished(m_ctx.info(), ec, std::move(ep));
        }
      } catch (...) {} // GCOVR_EXCL_LINE: owner notification can fail only through executor or allocation failure
    }

    void close_now() noexcept
    {
      boost::system::error_code ignored;
      m_socket.cancel(ignored);
      m_socket.close(ignored);
    }

    socket_type m_socket;
    boost::asio::cancellation_signal m_cancellation;
    session_context<Protocol> m_ctx;
    session_type m_session;
    std::weak_ptr<basic_acceptor_listener> m_owner;
    std::uint64_t m_id{};
    std::exception_ptr m_ep;
    bool m_finished{};
  };

public:
  basic_acceptor_listener(boost::asio::any_io_executor executor, std::uint64_t id,
                          std::shared_ptr<detail::observer_registry> observers, endpoint_type endpoint,
                          SessionFactory factory, listener_options options = {})
    : basic_acceptor_listener{[executor] {
                                return boost::asio::any_io_executor{
                                    boost::asio::strand<boost::asio::any_io_executor>{executor}};
                              },
                              id,
                              std::move(observers),
                              std::move(endpoint),
                              std::move(factory),
                              std::move(options)}
  {}

  basic_acceptor_listener(strand_factory_type strand_factory, std::uint64_t id,
                          std::shared_ptr<detail::observer_registry> observers, endpoint_type endpoint,
                          SessionFactory factory, listener_options options = {})
    : m_strand_factory{std::move(strand_factory)}
    , m_strand{m_strand_factory()}
    , m_acceptor{m_strand}
    , m_accept_retry_timer{m_strand}
    , m_shutdown_timer{m_strand}
    , m_id{id}
    , m_observers{std::move(observers)}
    , m_configured_endpoint{std::move(endpoint)}
    , m_endpoint{detail::endpoint_string(m_configured_endpoint)}
    , m_factory{std::move(factory)}
    , m_options{std::move(options)}
  {
    detail::validate_acceptor_options(m_options);
  }

  [[nodiscard]] std::uint64_t id() const noexcept override
  {
    return m_id;
  }

  void request_stop() noexcept override
  {
    if (state() == listener_state::stopped) return;
    auto self = std::static_pointer_cast<basic_acceptor_listener>(shared_from_this());
    try {
      boost::asio::post(m_strand, [self] { self->stop_on_strand(); });
    } catch (...) {} // GCOVR_EXCL_LINE: requires executor or allocation failure in a noexcept control boundary
  }

  [[nodiscard]] listener_state state() const noexcept override
  {
    return m_state.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::size_t active_sessions() const noexcept override
  {
    return m_active_sessions.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::string endpoint() const override
  {
    std::lock_guard lock{m_metadata_mutex};
    return m_endpoint;
  }

  [[nodiscard]] listener_status status() const override
  {
    listener_status value;
    value.state = state();
    value.listener_id = m_id;
    value.generation = m_generation.load(std::memory_order_relaxed);
    value.active_sessions = active_sessions();
    std::lock_guard lock{m_metadata_mutex};
    value.stop_reason = m_stop_reason;
    value.endpoint = m_endpoint;
    value.last_error = m_last_error;
    value.last_exception = m_last_exception;
    return value;
  }

private:
  void do_async_start(completion_handler handler) override
  {
    handler = detail::track_completion_work(m_strand, std::move(handler));
    auto self = std::static_pointer_cast<basic_acceptor_listener>(shared_from_this());
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>();
    boost::asio::cancellation_slot cancellation = boost::asio::get_associated_cancellation_slot(handler);
    if (cancellation.is_connected()) {
      cancellation.assign([cancelled](boost::asio::cancellation_type type) noexcept {
        // GCOVR_EXCL_START: asserted by CancellationBeforeStartLeavesListenerIdle; GCC duplicates this lambda.
        if (type != boost::asio::cancellation_type::none) cancelled->store(true, std::memory_order_release);
        // GCOVR_EXCL_STOP
      });
    }
    std::shared_ptr<completion_handler> pending = std::make_shared<completion_handler>(std::move(handler));
    try {
      boost::asio::post(m_strand, [self, cancelled, cancellation, pending]() mutable {
        cancellation.clear();
        completion_handler ready = std::move(*pending);
        if (cancelled->load(std::memory_order_acquire)) {
          // GCOVR_EXCL_START: asserted by CancellationBeforeStartLeavesListenerIdle; GCC duplicates this lambda.
          detail::complete_on(self->m_strand, std::move(ready), boost::asio::error::operation_aborted);
          return;
          // GCOVR_EXCL_STOP
        }
        self->start_on_strand(std::move(ready));
      });
    } catch (...) { // GCOVR_EXCL_LINE: requires executor or allocation failure during initiation
      detail::complete_on_associated(std::move(*pending), detail::exception_to_error(std::current_exception()));
    }
  }

  void do_async_wait(completion_handler handler) override
  {
    handler = detail::track_completion_work(m_strand, std::move(handler));
    auto self = std::static_pointer_cast<basic_acceptor_listener>(shared_from_this());
    const std::uint64_t id = m_next_waiter_id.fetch_add(1, std::memory_order_relaxed) + 1;
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>();
    boost::asio::cancellation_slot cancellation = boost::asio::get_associated_cancellation_slot(handler);
    if (cancellation.is_connected()) {
      std::weak_ptr<basic_acceptor_listener> weak = self;
      cancellation.assign([weak, id, cancelled](boost::asio::cancellation_type type) noexcept {
        // GCOVR_EXCL_START: asserted by CancellingWaitDoesNotStopListener; GCC duplicates this lambda.
        if (type == boost::asio::cancellation_type::none) return;
        cancelled->store(true, std::memory_order_release);
        try {
          if (std::shared_ptr<basic_acceptor_listener> value = weak.lock()) {
            boost::asio::post(value->m_strand, [value, id] { value->cancel_wait(id); });
          }
        } catch (...) {} // requires executor or allocation failure in a noexcept cancellation boundary
        // GCOVR_EXCL_STOP
      });
    }
    std::shared_ptr<completion_handler> pending = std::make_shared<completion_handler>(std::move(handler));
    try {
      boost::asio::post(m_strand, [self, id, cancelled, cancellation, pending]() mutable {
        completion_handler ready = std::move(*pending);
        if (cancelled->load(std::memory_order_acquire)) {
          // GCOVR_EXCL_START: asserted by cancellation tests; GCC duplicates this posted lambda.
          cancellation.clear();
          detail::complete_on(self->m_strand, std::move(ready), boost::asio::error::operation_aborted);
        } else if (self->state() == listener_state::stopped) {
          cancellation.clear();
          detail::complete_on(self->m_strand, std::move(ready), self->m_terminal_error);
          // GCOVR_EXCL_STOP
        } else {
          self->m_waiters.push_back(
              wait_operation{id, std::move(cancelled), std::move(cancellation), std::move(ready)});
        }
      });
    } catch (...) {
      cancellation.clear();
      detail::complete_on_associated(std::move(*pending), detail::exception_to_error(std::current_exception()));
    }
  }

  void cancel_wait(std::uint64_t id)
  {
    // GCOVR_EXCL_START: asserted by CancellingWaitDoesNotStopListener; GCC duplicates this template body.
    const typename std::list<wait_operation>::iterator item =
        std::find_if(m_waiters.begin(), m_waiters.end(), [id](const wait_operation& value) { return value.id == id; });
    if (item == m_waiters.end()) return;
    wait_operation operation = std::move(*item);
    m_waiters.erase(item);
    operation.cancellation.clear();
    detail::complete_on(m_strand, std::move(operation.handler), boost::asio::error::operation_aborted);
  }
  // GCOVR_EXCL_STOP

  void start_on_strand(completion_handler handler)
  {
    const listener_state current = state();
    if (current == listener_state::running) {
      detail::complete_on(m_strand, std::move(handler), {});
      return;
    }
    if (current != listener_state::idle && current != listener_state::stopped) {
      detail::complete_on(m_strand, std::move(handler), boost::asio::error::operation_aborted);
      return;
    }

    m_generation.fetch_add(1, std::memory_order_relaxed);
    m_terminal_error.clear();
    clear_diagnostics();
    transition_to(listener_state::starting);
    boost::system::error_code ec;
    m_acceptor.open(m_configured_endpoint.protocol(), ec);
    if constexpr (detail::has_ip_endpoint<endpoint_type>::value) {
      if (!ec && m_configured_endpoint.address().is_v6()) {
        m_acceptor.set_option(boost::asio::ip::v6_only{m_options.v6_only}, ec);
      }
      if (!ec) {
        m_acceptor.set_option(boost::asio::socket_base::reuse_address{m_options.reuse_address}, ec);
      }
    }
    if (!ec) m_acceptor.bind(m_configured_endpoint, ec);
    if (!ec) m_acceptor.listen(m_options.listen_backlog, ec);
    if (ec) {
      detail::complete_on(m_strand, std::move(handler), ec);
      finish(ec, listener_stop_reason::startup_failure);
      return;
    }

    const auto bound = m_acceptor.local_endpoint(ec);
    if (!ec) {
      std::lock_guard lock{m_metadata_mutex};
      m_endpoint = detail::endpoint_string(bound);
    }
    const boost::system::error_code accept_ec = accept_next(listener_stop_reason::startup_failure);
    if (accept_ec) {
      detail::complete_on(m_strand, std::move(handler), accept_ec);
      return;
    }
    transition_to(listener_state::running);
    detail::complete_on(m_strand, std::move(handler), {});
  }

  [[nodiscard]] boost::system::error_code
  accept_next(listener_stop_reason failure_reason = listener_stop_reason::runtime_failure)
  {
    const listener_state current = state();
    if ((current != listener_state::starting && current != listener_state::running) || m_accept_pending) {
      maybe_finish();
      return {};
    }
    if (m_sessions.size() >= m_options.max_active_sessions) return {};

    m_accept_pending = true;
    try {
      auto self = std::static_pointer_cast<basic_acceptor_listener>(shared_from_this());
      auto socket = std::make_shared<socket_type>(m_strand_factory());
      m_acceptor.async_accept(
          *socket, boost::asio::bind_executor(m_strand, [self, socket](boost::system::error_code ec) mutable {
            self->accepted(std::move(socket), ec);
          }));
      return {};
    } catch (...) {
      m_accept_pending = false;
      std::exception_ptr ep = std::current_exception();
      const boost::system::error_code ec = detail::exception_to_error(ep);
      record_diagnostic(ec, ep);
      emit(listener_event_type::accept_failed, {}, ec, std::move(ep));
      begin_stop(ec, failure_reason);
      return ec;
    }
  }

  void accepted(std::shared_ptr<socket_type> socket, boost::system::error_code ec)
  {
    m_accept_pending = false;
    if (state() != listener_state::running) {
      maybe_finish();
      return;
    }
    if (ec) {
      retry_accept(ec);
      return;
    }

    m_consecutive_accept_errors = 0;

    const std::uint64_t connection_id = ++m_next_connection_id;
    boost::system::error_code endpoint_ec;
    const endpoint_type local = socket->local_endpoint(endpoint_ec);
    const std::string local_endpoint = endpoint_ec ? std::string{} : detail::endpoint_string(local);
    endpoint_ec.clear();
    const endpoint_type remote = socket->remote_endpoint(endpoint_ec);
    const std::string remote_endpoint = endpoint_ec ? std::string{} : detail::endpoint_string(remote);
    const connection_info info{connection_id, local_endpoint, remote_endpoint};
    std::shared_ptr<connection_state> connection;
    try {
      session_type session = m_factory();
      connection =
          std::make_shared<connection_state>(std::move(*socket), std::move(session), m_id, info,
                                             std::static_pointer_cast<basic_acceptor_listener>(shared_from_this()));
    } catch (...) {
      std::exception_ptr ep = std::current_exception();
      const boost::system::error_code factory_ec = detail::exception_to_error(ep);
      record_diagnostic(factory_ec, ep);
      emit(listener_event_type::session_factory_failed, info, factory_ec, std::move(ep));
      boost::system::error_code ignored;
      socket->close(ignored);
      static_cast<void>(accept_next());
      return;
    }
    m_sessions.push_back(connection);
    m_active_sessions.fetch_add(1, std::memory_order_relaxed);
    emit(listener_event_type::session_started, connection->info(), {}, {});
    try {
      boost::asio::post(connection->get_executor(), [connection] { connection->start(); });
    } catch (...) {
      connection->force_close();
    }
    static_cast<void>(accept_next());
  }

  void session_finished(connection_info info, boost::system::error_code ec, std::exception_ptr ep)
  {
    auto self = std::static_pointer_cast<basic_acceptor_listener>(shared_from_this());
    boost::asio::post(m_strand, [self, info = std::move(info), ec, ep = std::move(ep)]() mutable {
      const typename std::list<std::shared_ptr<connection_state>>::iterator item =
          std::find_if(self->m_sessions.begin(), self->m_sessions.end(),
                       [&info](const std::shared_ptr<connection_state>& value) { return value->id() == info.id; });
      if (item != self->m_sessions.end()) {
        self->m_sessions.erase(item);
        self->m_active_sessions.fetch_sub(1, std::memory_order_relaxed);
      }
      if (ec != boost::asio::error::operation_aborted && (ec || ep)) {
        // GCOVR_EXCL_START: asserted by synchronous and awaitable exception tests; GCC duplicates this lambda.
        self->record_diagnostic(ec, ep);
        // GCOVR_EXCL_STOP
      }
      self->emit(listener_event_type::session_stopped, info, ec, std::move(ep));
      if (self->state() == listener_state::running) static_cast<void>(self->accept_next());
      self->maybe_finish();
    });
  }

  void stop_on_strand()
  {
    begin_stop({}, listener_stop_reason::requested);
  }

  void begin_stop(boost::system::error_code ec, listener_stop_reason reason)
  {
    const listener_state current = state();
    if (current == listener_state::idle) {
      m_terminal_error = ec;
      set_stop_reason(reason);
      transition_to(listener_state::stopping);
      finish(ec, reason);
      return;
    }
    if (current != listener_state::starting && current != listener_state::running) return;

    m_terminal_error = ec;
    set_stop_reason(reason);
    transition_to(listener_state::stopping);
    boost::system::error_code ignored;
    m_acceptor.cancel(ignored);
    m_acceptor.close(ignored);
    try {
      m_accept_retry_timer.cancel();
    } catch (...) {} // GCOVR_EXCL_LINE: steady_timer::cancel(error_code) is non-throwing in supported Asio builds
    for (const auto& session : m_sessions)
      session->request_stop();

    if (!m_sessions.empty()) {
      auto self = std::static_pointer_cast<basic_acceptor_listener>(shared_from_this());
      try {
        m_shutdown_timer.expires_after(m_options.shutdown_grace_period);
        m_shutdown_timer.async_wait(boost::asio::bind_executor(m_strand, [self](boost::system::error_code timer_ec) {
          // GCOVR_EXCL_START: asserted by GraceDeadlineDrainsAnUncooperativeSession; GCC duplicates this lambda.
          if (timer_ec) return;
          const boost::system::error_code timeout = make_error_code(boost::system::errc::timed_out);
          self->record_diagnostic(timeout, {});
          self->emit(listener_event_type::shutdown_forced, {}, timeout, {});
          for (const auto& session : self->m_sessions)
            session->force_close();
          // GCOVR_EXCL_STOP
        }));
      } catch (...) { // GCOVR_EXCL_START: requires executor or allocation failure while arming the grace timer
        for (const auto& session : m_sessions)
          session->force_close();
        // GCOVR_EXCL_STOP
      }
    }
    maybe_finish();
  }

  void maybe_finish()
  {
    if (state() == listener_state::stopping && !m_accept_pending && m_sessions.empty()) {
      finish(m_terminal_error, stop_reason());
    }
  }

  void finish(boost::system::error_code ec, listener_stop_reason reason)
  {
    if (state() == listener_state::stopped) return;
    m_terminal_error = ec;
    set_stop_reason(reason);
    if (ec) {
      std::lock_guard lock{m_metadata_mutex};
      if (!m_last_error && !m_last_exception) m_last_error = ec;
    }
    boost::system::error_code ignored;
    m_acceptor.close(ignored);
    try {
      m_accept_retry_timer.cancel();
      m_shutdown_timer.cancel();
    } catch (...) {} // GCOVR_EXCL_LINE: error_code timer cancellation is non-throwing in supported Asio builds
    transition_to(listener_state::stopped);
    for (wait_operation& waiter : m_waiters) {
      waiter.cancellation.clear();
      detail::complete_on(m_strand, std::move(waiter.handler), m_terminal_error);
    }
    m_waiters.clear();
  }

  void retry_accept(boost::system::error_code accept_ec)
  {
    record_diagnostic(accept_ec, {});
    emit(listener_event_type::accept_failed, {}, accept_ec, {});
    if (!detail::is_transient_accept_error(accept_ec) ||
        ++m_consecutive_accept_errors >= m_options.max_consecutive_accept_errors) {
      begin_stop(accept_ec, listener_stop_reason::runtime_failure);
      return;
    }
    try {
      m_accept_retry_timer.expires_after(detail::accept_retry_delay(m_options, m_consecutive_accept_errors));
      auto self = std::static_pointer_cast<basic_acceptor_listener>(shared_from_this());
      m_accept_retry_timer.async_wait(boost::asio::bind_executor(m_strand, [self](boost::system::error_code ec) {
        // GCOVR_EXCL_START: asserted by StopCancelsPendingAcceptRetry; GCC duplicates this timer lambda.
        if (ec == boost::asio::error::operation_aborted) {
          self->maybe_finish();
          return;
        }
        // GCOVR_EXCL_STOP
        // GCOVR_EXCL_START: steady_timer reports only success or operation_aborted without executor fault injection.
        if (ec) {
          self->begin_stop(ec, listener_stop_reason::runtime_failure);
          return;
        }
        // GCOVR_EXCL_STOP
        // GCOVR_EXCL_START: asserted by RetriesAfterTransientAcceptFailure; GCC duplicates this timer lambda.
        static_cast<void>(self->accept_next());
        // GCOVR_EXCL_STOP
      }));
    } catch (...) { // GCOVR_EXCL_START: requires executor or allocation failure while arming the retry timer
      std::exception_ptr ep = std::current_exception();
      const boost::system::error_code ec = detail::exception_to_error(ep);
      record_diagnostic(ec, ep);
      begin_stop(ec, listener_stop_reason::runtime_failure);
      // GCOVR_EXCL_STOP
    }
  }

  void transition_to(listener_state value)
  {
    m_state.store(value, std::memory_order_release);
    emit(listener_event_type::state_changed, {}, {}, {});
  }

  void clear_diagnostics()
  {
    std::lock_guard lock{m_metadata_mutex};
    m_stop_reason = listener_stop_reason::none;
    m_last_error.clear();
    m_last_exception = {};
  }

  void record_diagnostic(boost::system::error_code ec, std::exception_ptr ep)
  {
    std::lock_guard lock{m_metadata_mutex};
    m_last_error = ec;
    m_last_exception = std::move(ep);
  }

  void set_stop_reason(listener_stop_reason reason)
  {
    std::lock_guard lock{m_metadata_mutex};
    m_stop_reason = reason;
  }

  [[nodiscard]] listener_stop_reason stop_reason() const
  {
    std::lock_guard lock{m_metadata_mutex};
    return m_stop_reason;
  }

  void emit(listener_event_type type, connection_info connection, boost::system::error_code ec,
            std::exception_ptr ep) noexcept
  {
    if (!m_observers) return;
    try {
      listener_event event;
      event.type = type;
      event.status = status();
      event.connection = std::move(connection);
      event.error = ec;
      event.exception = std::move(ep);
      m_observers->notify(event);
    } catch (...) {}
  }

  strand_factory_type m_strand_factory;
  boost::asio::any_io_executor m_strand;
  acceptor_type m_acceptor;
  boost::asio::steady_timer m_accept_retry_timer;
  boost::asio::steady_timer m_shutdown_timer;
  std::uint64_t m_id{};
  std::shared_ptr<detail::observer_registry> m_observers;
  endpoint_type m_configured_endpoint;
  mutable std::mutex m_metadata_mutex;
  std::string m_endpoint;
  listener_stop_reason m_stop_reason{listener_stop_reason::none};
  boost::system::error_code m_last_error;
  std::exception_ptr m_last_exception;
  SessionFactory m_factory;
  listener_options m_options;
  std::atomic<listener_state> m_state{listener_state::idle};
  std::atomic_uint64_t m_generation{};
  std::atomic_size_t m_active_sessions{};
  std::atomic_uint64_t m_next_waiter_id{};
  std::uint64_t m_next_connection_id{};
  std::size_t m_consecutive_accept_errors{};
  bool m_accept_pending{};
  boost::system::error_code m_terminal_error;
  std::list<std::shared_ptr<connection_state>> m_sessions;
  std::list<wait_operation> m_waiters;
};

} // namespace serveza

#endif // SERVEZA_LISTENER_H
