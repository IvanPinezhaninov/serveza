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

#ifndef SERVEZA_DETAIL_BASIC_SOCKET_LISTENER_H
#define SERVEZA_DETAIL_BASIC_SOCKET_LISTENER_H

#include <algorithm>
#include <atomic>
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

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
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

#include <serveza/bound_socket_context.h>
#include <serveza/listener.h>

namespace serveza::detail {

template<typename Protocol, typename SessionFactory>
class basic_socket_listener final : public listener {
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

  class session_state final : public std::enable_shared_from_this<session_state> {
  public:
    session_state(socket_type socket, session_type session, std::uint64_t listener_id, std::string local_endpoint,
                  std::weak_ptr<basic_socket_listener> owner)
      : m_socket{std::move(socket)}
      , m_ctx{m_socket, listener_id, std::move(local_endpoint), m_cancellation.slot(),
              [this](std::exception_ptr ep) { m_ep = std::move(ep); }}
      , m_session{std::move(session)}
      , m_owner{std::move(owner)}
    {}

    void start()
    {
      std::shared_ptr<session_state> self = this->shared_from_this();
      try {
        m_session.async_run(
            m_ctx, boost::asio::bind_cancellation_slot(
                       m_cancellation.slot(), boost::asio::bind_executor(m_socket.get_executor(),
                                                                         [self](boost::system::error_code ec) mutable {
                                                                           self->finish(ec, std::move(self->m_ep));
                                                                         })));
      } catch (...) {
        std::exception_ptr ep = std::current_exception();
        const boost::system::error_code ec = exception_to_error(ep);
        finish(ec, std::move(ep));
      }
    }

    [[nodiscard]] boost::asio::any_io_executor get_executor()
    {
      return m_socket.get_executor();
    }

    void request_stop() noexcept
    {
      std::shared_ptr<session_state> self = this->shared_from_this();
      try {
        boost::asio::post(m_socket.get_executor(), [self] {
          self->m_cancellation.emit(boost::asio::cancellation_type::all);
          boost::system::error_code ignored;
          self->m_socket.cancel(ignored);
        });
      } catch (...) { // GCOVR_EXCL_START: requires executor or allocation failure in a noexcept shutdown boundary
        force_close();
        // GCOVR_EXCL_STOP
      }
    }

    void force_close() noexcept
    {
      std::shared_ptr<session_state> self = this->shared_from_this();
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
        if (std::shared_ptr<basic_socket_listener> owner = m_owner.lock()) {
          owner->session_finished(ec, std::move(ep));
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
    bound_socket_context<Protocol> m_ctx;
    session_type m_session;
    std::weak_ptr<basic_socket_listener> m_owner;
    std::exception_ptr m_ep;
    bool m_finished{};
  };

public:
  basic_socket_listener(strand_factory_type strand_factory, std::uint64_t id,
                        std::shared_ptr<observer_registry> observers, endpoint_type endpoint, SessionFactory factory,
                        listener_options options = {})
    : m_strand_factory{std::move(strand_factory)}
    , m_strand{m_strand_factory()}
    , m_shutdown_timer{m_strand}
    , m_id{id}
    , m_observers{std::move(observers)}
    , m_configured_endpoint{std::move(endpoint)}
    , m_endpoint{endpoint_string(m_configured_endpoint)}
    , m_factory{std::move(factory)}
    , m_options{std::move(options)}
  {
    validate_socket_options(m_options);
  }

  void request_stop() noexcept override
  {
    if (state() == listener_state::stopped) return;
    std::shared_ptr<basic_socket_listener> self = std::static_pointer_cast<basic_socket_listener>(shared_from_this());
    try {
      boost::asio::post(m_strand, [self] { self->stop_on_strand(); });
    } catch (...) {} // GCOVR_EXCL_LINE: requires executor or allocation failure in a noexcept control boundary
  }

  [[nodiscard]] std::uint64_t id() const noexcept override
  {
    return m_id;
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
    handler = track_completion_work(m_strand, std::move(handler));
    std::shared_ptr<basic_socket_listener> self = std::static_pointer_cast<basic_socket_listener>(shared_from_this());
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>();
    boost::asio::cancellation_slot cancellation = boost::asio::get_associated_cancellation_slot(handler);
    if (cancellation.is_connected()) {
      cancellation.assign([cancelled](boost::asio::cancellation_type type) noexcept {
        // GCOVR_EXCL_START: asserted by CancellationBeforeUdpStartLeavesListenerIdle; GCC duplicates this lambda.
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
          // GCOVR_EXCL_START: asserted by CancellationBeforeUdpStartLeavesListenerIdle; GCC duplicates this lambda.
          complete_on(self->m_strand, std::move(ready), boost::asio::error::operation_aborted);
          return;
          // GCOVR_EXCL_STOP
        }
        self->start_on_strand(std::move(ready));
      });
    } catch (...) { // GCOVR_EXCL_START: requires executor or allocation failure during initiation
      complete_on_associated(std::move(*pending), exception_to_error(std::current_exception()));
      // GCOVR_EXCL_STOP
    }
  }

  void do_async_wait(completion_handler handler) override
  {
    handler = track_completion_work(m_strand, std::move(handler));
    std::shared_ptr<basic_socket_listener> self = std::static_pointer_cast<basic_socket_listener>(shared_from_this());
    const std::uint64_t id = m_next_waiter_id.fetch_add(1, std::memory_order_relaxed) + 1;
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>();
    boost::asio::cancellation_slot cancellation = boost::asio::get_associated_cancellation_slot(handler);
    if (cancellation.is_connected()) {
      std::weak_ptr<basic_socket_listener> weak = self;
      cancellation.assign([weak, id, cancelled](boost::asio::cancellation_type type) noexcept {
        // GCOVR_EXCL_START: asserted by CancellingUdpWaitDoesNotStopBoundSession; GCC duplicates this lambda.
        if (type == boost::asio::cancellation_type::none) return;
        cancelled->store(true, std::memory_order_release);
        try {
          if (std::shared_ptr<basic_socket_listener> value = weak.lock()) {
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
          complete_on(self->m_strand, std::move(ready), boost::asio::error::operation_aborted);
        } else if (self->state() == listener_state::stopped) {
          cancellation.clear();
          complete_on(self->m_strand, std::move(ready), self->m_terminal_error);
          // GCOVR_EXCL_STOP
        } else {
          self->m_waiters.push_back(
              wait_operation{id, std::move(cancelled), std::move(cancellation), std::move(ready)});
        }
      });
    } catch (...) { // GCOVR_EXCL_START: requires executor or allocation failure during initiation
      cancellation.clear();
      complete_on_associated(std::move(*pending), exception_to_error(std::current_exception()));
      // GCOVR_EXCL_STOP
    }
  }

  void cancel_wait(std::uint64_t id)
  {
    // GCOVR_EXCL_START: asserted by CancellingUdpWaitDoesNotStopBoundSession; GCC duplicates this template body.
    const typename std::list<wait_operation>::iterator item =
        std::find_if(m_waiters.begin(), m_waiters.end(), [id](const wait_operation& value) { return value.id == id; });
    if (item == m_waiters.end()) return;
    wait_operation operation = std::move(*item);
    m_waiters.erase(item);
    operation.cancellation.clear();
    complete_on(m_strand, std::move(operation.handler), boost::asio::error::operation_aborted);
  }
  // GCOVR_EXCL_STOP

  void start_on_strand(completion_handler handler)
  {
    const listener_state current = state();
    if (current == listener_state::running) {
      complete_on(m_strand, std::move(handler), {});
      return;
    }
    if (current != listener_state::idle && current != listener_state::stopped) {
      complete_on(m_strand, std::move(handler), boost::asio::error::operation_aborted);
      return;
    }

    m_generation.fetch_add(1, std::memory_order_relaxed);
    m_terminal_error.clear();
    clear_diagnostics();
    transition_to(listener_state::starting);

    socket_type socket{m_strand_factory()};
    boost::system::error_code ec;
    socket.open(m_configured_endpoint.protocol(), ec);
    if constexpr (has_ip_endpoint<endpoint_type>::value) {
      if (!ec && m_configured_endpoint.address().is_v6()) {
        socket.set_option(boost::asio::ip::v6_only{m_options.v6_only}, ec);
      }
      if (!ec) socket.set_option(boost::asio::socket_base::reuse_address{m_options.reuse_address}, ec);
    }
    if (!ec) socket.bind(m_configured_endpoint, ec);
    if (ec) {
      boost::system::error_code ignored;
      socket.close(ignored);
      complete_on(m_strand, std::move(handler), ec);
      finish(ec, listener_stop_reason::startup_failure);
      return;
    }

    const endpoint_type bound = socket.local_endpoint(ec);
    const std::string bound_endpoint = ec ? endpoint_string(m_configured_endpoint) : endpoint_string(bound);
    {
      std::lock_guard lock{m_metadata_mutex};
      m_endpoint = bound_endpoint;
    }

    try {
      session_type session = m_factory();
      m_session = std::make_shared<session_state>(std::move(socket), std::move(session), m_id, bound_endpoint,
                                                  std::static_pointer_cast<basic_socket_listener>(shared_from_this()));
    } catch (...) {
      std::exception_ptr ep = std::current_exception();
      const boost::system::error_code factory_ec = exception_to_error(ep);
      record_diagnostic(factory_ec, ep);
      emit(listener_event_type::session_factory_failed, factory_ec, std::move(ep));
      complete_on(m_strand, std::move(handler), factory_ec);
      finish(factory_ec, listener_stop_reason::startup_failure);
      return;
    }

    m_active_sessions.store(1, std::memory_order_relaxed);
    transition_to(listener_state::running);
    emit(listener_event_type::session_started, {}, {});
    complete_on(m_strand, std::move(handler), {});
    std::shared_ptr<session_state> session = m_session;
    boost::asio::post(session->get_executor(), [session] { session->start(); });
  }

  void session_finished(boost::system::error_code ec, std::exception_ptr ep)
  {
    std::shared_ptr<basic_socket_listener> self = std::static_pointer_cast<basic_socket_listener>(shared_from_this());
    boost::asio::post(m_strand, [self, ec, ep = std::move(ep)]() mutable {
      self->m_session.reset();
      self->m_active_sessions.store(0, std::memory_order_relaxed);
      if (ec != boost::asio::error::operation_aborted && (ec || ep)) {
        // GCOVR_EXCL_START: asserted by SynchronousUdpSessionExceptionIsPreserved; GCC duplicates this lambda.
        self->record_diagnostic(ec, ep);
        // GCOVR_EXCL_STOP
      }
      self->emit(listener_event_type::session_stopped, ec, std::move(ep));
      if (self->state() == listener_state::running) {
        // GCOVR_EXCL_START: asserted by synchronous and callback session failure tests; GCC duplicates this lambda.
        self->transition_to(listener_state::stopping);
        const listener_stop_reason reason =
            ec ? listener_stop_reason::runtime_failure : listener_stop_reason::completed;
        self->finish(ec, reason);
        // GCOVR_EXCL_STOP
      } else {
        self->maybe_finish();
      }
    });
  }

  void stop_on_strand()
  {
    const listener_state current = state();
    if (current == listener_state::idle) {
      set_stop_reason(listener_stop_reason::requested);
      transition_to(listener_state::stopping);
      finish({}, listener_stop_reason::requested);
      return;
    }
    if (current != listener_state::starting && current != listener_state::running) return;

    set_stop_reason(listener_stop_reason::requested);
    transition_to(listener_state::stopping);
    if (m_session) m_session->request_stop();
    if (m_session) {
      std::shared_ptr<basic_socket_listener> self = std::static_pointer_cast<basic_socket_listener>(shared_from_this());
      m_shutdown_timer.expires_after(m_options.shutdown_grace_period);
      m_shutdown_timer.async_wait(boost::asio::bind_executor(m_strand, [self](boost::system::error_code ec) {
        // GCOVR_EXCL_START: asserted by GraceDeadlineDrainsUncooperativeBoundSession; GCC duplicates this lambda.
        if (ec) return;
        const boost::system::error_code timeout = make_error_code(boost::system::errc::timed_out);
        self->record_diagnostic(timeout, {});
        self->emit(listener_event_type::shutdown_forced, timeout, {});
        if (self->m_session) self->m_session->force_close();
        // GCOVR_EXCL_STOP
      }));
    }
    maybe_finish();
  }

  void maybe_finish()
  {
    if (state() == listener_state::stopping && !m_session) finish(m_terminal_error, stop_reason());
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
    try {
      m_shutdown_timer.cancel();
    } catch (...) {} // GCOVR_EXCL_LINE: steady_timer::cancel(error_code) is non-throwing in supported Asio builds
    transition_to(listener_state::stopped);
    for (wait_operation& waiter : m_waiters) {
      waiter.cancellation.clear();
      complete_on(m_strand, std::move(waiter.handler), m_terminal_error);
    }
    m_waiters.clear();
  }

  void transition_to(listener_state value)
  {
    m_state.store(value, std::memory_order_release);
    emit(listener_event_type::state_changed, {}, {});
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

  void emit(listener_event_type type, boost::system::error_code ec, std::exception_ptr ep) noexcept
  {
    if (!m_observers) return;
    try {
      listener_event event;
      event.type = type;
      event.status = status();
      event.error = ec;
      event.exception = std::move(ep);
      m_observers->notify(event);
    } catch (...) {}
  }

  strand_factory_type m_strand_factory;
  boost::asio::any_io_executor m_strand;
  boost::asio::steady_timer m_shutdown_timer;
  std::uint64_t m_id{};
  std::shared_ptr<observer_registry> m_observers;
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
  boost::system::error_code m_terminal_error;
  std::shared_ptr<session_state> m_session;
  std::list<wait_operation> m_waiters;
};

} // namespace serveza::detail

#endif // SERVEZA_DETAIL_BASIC_SOCKET_LISTENER_H
