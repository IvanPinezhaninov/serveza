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

#include <serveza/server.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <utility>

#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/post.hpp>

#include <serveza/detail/observer_registry.h>

namespace serveza {
namespace {

class server_control final {
public:
  server_control(boost::asio::any_io_executor executor, std::shared_ptr<detail::observer_registry> observers)
    : m_executor{std::move(executor)}
    , m_observers{std::move(observers)}
  {}

  [[nodiscard]] server_status status(const std::vector<std::shared_ptr<listener>>& listeners) const
  {
    server_status value;
    value.state = m_state.load(std::memory_order_acquire);
    value.generation = m_generation.load(std::memory_order_relaxed);
    value.listeners.reserve(listeners.size());
    value.ready = !listeners.empty() && value.state == server_state::running;
    for (const std::shared_ptr<listener>& item : listeners) {
      listener_status listener_value = item->status();
      value.active_sessions += listener_value.active_sessions;
      value.ready = value.ready && listener_value.state == listener_state::running;
      value.listeners.push_back(std::move(listener_value));
    }
    return value;
  }

  [[nodiscard]] bool begin_start(const std::vector<std::shared_ptr<listener>>& listeners)
  {
    server_state current = m_state.load(std::memory_order_acquire);
    for (;;) {
      if (current == server_state::starting || current == server_state::running) return true;
      if (current == server_state::stopping) return false;
      if (m_state.compare_exchange_weak(current, server_state::starting, std::memory_order_acq_rel)) {
        m_generation.fetch_add(1, std::memory_order_relaxed);
        emit(server_event_type::starting, listeners, {});
        return true;
      }
    }
  }

  boost::system::error_code finish_start(const std::vector<std::shared_ptr<listener>>& listeners,
                                         boost::system::error_code ec)
  {
    if (ec) {
      server_state current = m_state.load(std::memory_order_acquire);
      while (current != server_state::stopping && current != server_state::stopped) {
        if (m_state.compare_exchange_weak(current, server_state::stopping, std::memory_order_acq_rel)) {
          emit(server_event_type::stopping, listeners, ec);
          break;
        }
      }
      return ec;
    }
    server_state expected = server_state::starting;
    if (m_state.compare_exchange_strong(expected, server_state::running, std::memory_order_acq_rel)) {
      emit(server_event_type::running, listeners, {});
      return {};
    }
    if (expected == server_state::running) return {};
    return boost::asio::error::operation_aborted;
  }

  void request_stop(const std::vector<std::shared_ptr<listener>>& listeners) noexcept
  {
    try {
      server_state current = m_state.load(std::memory_order_acquire);
      while (current != server_state::stopping && current != server_state::stopped) {
        if (m_state.compare_exchange_weak(current, server_state::stopping, std::memory_order_acq_rel)) {
          emit(server_event_type::stopping, listeners, {});
          return;
        }
      }
    } catch (...) {} // GCOVR_EXCL_LINE: requires executor or allocation failure in a noexcept control boundary
  }

  boost::system::error_code finish_wait(const std::vector<std::shared_ptr<listener>>& listeners,
                                        boost::system::error_code ec)
  {
    const server_state previous = m_state.exchange(server_state::stopped, std::memory_order_acq_rel);
    if (previous != server_state::stopped) emit(server_event_type::stopped, listeners, ec);
    return ec;
  }

  [[nodiscard]] bool stop_requested() const noexcept
  {
    const server_state current = m_state.load(std::memory_order_acquire);
    return current == server_state::stopping || current == server_state::stopped;
  }

private:
  void emit(server_event_type type, const std::vector<std::shared_ptr<listener>>& listeners,
            boost::system::error_code ec) noexcept
  {
    try {
      server_event event;
      event.type = type;
      event.status = status(listeners);
      event.error = ec;
      std::shared_ptr<detail::observer_registry> observers = m_observers;
      boost::asio::post(m_executor,
                        [observers = std::move(observers), event = std::move(event)] { observers->notify(event); });
    } catch (...) {} // GCOVR_EXCL_LINE: requires executor or allocation failure in a noexcept observer boundary
  }

  boost::asio::any_io_executor m_executor;
  std::shared_ptr<detail::observer_registry> m_observers;
  std::atomic<server_state> m_state{server_state::idle};
  std::atomic_uint64_t m_generation{};
};

class listener_operation_group final : public std::enable_shared_from_this<listener_operation_group> {
public:
  listener_operation_group(std::vector<std::shared_ptr<listener>> listeners, bool stop_on_error,
                           boost::asio::any_io_executor executor,
                           std::function<boost::system::error_code(boost::system::error_code)> finished,
                           listener::completion_handler handler)
    : m_listeners{std::move(listeners)}
    , m_remaining{m_listeners.size()}
    , m_initiation_ecs(m_remaining)
    , m_stop_on_error{stop_on_error}
    , m_fallback_executor{std::move(executor)}
    , m_finished{std::move(finished)}
    , m_completion{std::move(handler)}
  {
    m_cancellations.reserve(m_remaining);
    for (std::size_t i = 0; i < m_remaining; ++i)
      m_cancellations.push_back(std::make_shared<boost::asio::cancellation_signal>());
  }

  void prepare(boost::asio::cancellation_slot cancellation)
  {
    std::shared_ptr<listener_operation_group> self = shared_from_this();
    m_cancellation = std::move(cancellation);
    if (m_cancellation.is_connected()) {
      std::weak_ptr<listener_operation_group> weak = self;
      m_cancellation.assign([weak](boost::asio::cancellation_type type) noexcept {
        if (std::shared_ptr<listener_operation_group> value = weak.lock()) value->cancel(type);
      });
    }
  }

  void start(bool wait)
  {
    std::shared_ptr<listener_operation_group> self = shared_from_this();
    for (std::size_t i = 0; i < m_listeners.size(); ++i) {
      try {
        auto done = boost::asio::bind_cancellation_slot(m_cancellations[i]->slot(),
                                                        [self](boost::system::error_code ec) { self->one(ec); });
        if (wait)
          m_listeners[i]->async_wait(std::move(done));
        else
          m_listeners[i]->async_start(std::move(done));
        boost::asio::cancellation_type cancelled;
        {
          std::lock_guard lock{m_mutex};
          cancelled = m_cancelled;
        }
        if (cancelled != boost::asio::cancellation_type::none) m_cancellations[i]->emit(cancelled);
      } catch (...) {
        m_initiation_ecs[i] = detail::exception_to_error(std::current_exception());
      }
    }
    for (const boost::system::error_code ec : m_initiation_ecs) {
      if (ec) one(ec);
    }
  }

  void reject(boost::system::error_code ec)
  {
    m_cancellation.clear();
    detail::complete_on(m_fallback_executor, std::move(m_completion), ec);
  }

  void request_stop() noexcept
  {
    for (const std::shared_ptr<listener>& value : m_listeners)
      value->request_stop();
  }

private:
  void one(boost::system::error_code ec)
  {
    listener::completion_handler ready;
    boost::system::error_code result;
    bool stop = false;
    bool complete = false;
    {
      std::lock_guard lock{m_mutex};
      if (ec && !m_first_ec) {
        m_first_ec = ec;
        stop = m_stop_on_error;
      }
      if (--m_remaining == 0) {
        result = m_first_ec;
        m_cancellation.clear();
        ready = std::move(m_completion);
        complete = true;
      }
    }
    if (stop) request_stop();
    if (!complete) return;
    try {
      result = m_finished(result);
    } catch (...) { // GCOVR_EXCL_START: requires an exception from an internal lifecycle completion callback
    }
    // GCOVR_EXCL_STOP
    detail::complete_on(m_fallback_executor, std::move(ready), result);
  }

  void cancel(boost::asio::cancellation_type type) noexcept
  {
    if (type == boost::asio::cancellation_type::none) return;
    try {
      {
        std::lock_guard lock{m_mutex};
        m_cancelled = m_cancelled | type;
      }
      for (const std::shared_ptr<boost::asio::cancellation_signal>& cancellation : m_cancellations)
        cancellation->emit(type);
    } catch (...) { // GCOVR_EXCL_START: requires an exception from cancellation_signal::emit
    }
    // GCOVR_EXCL_STOP
  }

  std::mutex m_mutex;
  std::vector<std::shared_ptr<listener>> m_listeners;
  std::vector<std::shared_ptr<boost::asio::cancellation_signal>> m_cancellations;
  std::size_t m_remaining;
  std::vector<boost::system::error_code> m_initiation_ecs;
  boost::system::error_code m_first_ec;
  boost::asio::cancellation_type m_cancelled{boost::asio::cancellation_type::none};
  bool m_stop_on_error{};
  boost::asio::cancellation_slot m_cancellation;
  boost::asio::any_io_executor m_fallback_executor;
  std::function<boost::system::error_code(boost::system::error_code)> m_finished;
  listener::completion_handler m_completion;
};

} // namespace

namespace detail {

class server_implementation final {
public:
  server_implementation(boost::asio::any_io_executor exec, std::function<boost::asio::any_io_executor()> strand_factory)
    : m_executor{std::move(exec)}
    , m_strand_factory{std::move(strand_factory)}
    , m_observers{std::make_shared<detail::observer_registry>()}
    , m_control{std::make_shared<server_control>(m_executor, m_observers)}
  {}

  [[nodiscard]] std::vector<std::shared_ptr<listener>> snapshot() const
  {
    std::lock_guard lock{m_mutex};
    return m_listeners;
  }

  boost::asio::any_io_executor m_executor;
  std::function<boost::asio::any_io_executor()> m_strand_factory;
  std::shared_ptr<detail::observer_registry> m_observers;
  std::shared_ptr<server_control> m_control;
  mutable std::mutex m_mutex;
  std::vector<std::shared_ptr<listener>> m_listeners;
  std::atomic_uint64_t m_next_listener_id{};
};

} // namespace detail

server::server(boost::asio::any_io_executor executor)
  : server{executor, [executor] {
             return boost::asio::any_io_executor{boost::asio::strand<boost::asio::any_io_executor>{executor}};
           }}
{}

server::server(boost::asio::any_io_executor executor, strand_factory_type strand_factory)
  : m_impl{std::make_unique<detail::server_implementation>(std::move(executor), std::move(strand_factory))}
{}

server::~server()
{
  request_stop();
}

observer_subscription server::observe(std::shared_ptr<observer> value)
{
  if (!value) throw std::invalid_argument{"observer must not be null"};
  const std::uint64_t id = m_impl->m_observers->add(std::move(value));
  return observer_subscription{m_impl->m_observers, id};
}

void server::request_stop() noexcept
{
  try {
    std::lock_guard lock{m_impl->m_mutex};
    m_impl->m_control->request_stop(m_impl->m_listeners);
    for (const std::shared_ptr<listener>& value : m_impl->m_listeners)
      value->request_stop();
  } catch (...) {} // GCOVR_EXCL_LINE: requires a platform mutex failure in a noexcept control boundary
}

boost::asio::any_io_executor server::get_executor() const
{
  return m_impl->m_executor;
}

server::strand_factory_type server::strand_factory() const
{
  return m_impl->m_strand_factory;
}

server_status server::status() const
{
  return m_impl->m_control->status(m_impl->snapshot());
}

bool server::is_ready() const
{
  return status().ready;
}

std::size_t server::active_sessions() const
{
  return status().active_sessions;
}

std::vector<std::shared_ptr<listener>> server::listeners() const
{
  return m_impl->snapshot();
}

std::uint64_t server::next_listener_id() noexcept
{
  return m_impl->m_next_listener_id.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::shared_ptr<detail::observer_registry> server::observer_registry() const
{
  return m_impl->m_observers;
}

void server::add_listener(std::shared_ptr<listener> value)
{
  std::lock_guard lock{m_impl->m_mutex};
  m_impl->m_listeners.push_back(std::move(value));
}

void server::do_async_start(listener::completion_handler handler)
{
  handler = detail::track_completion_work(m_impl->m_executor, std::move(handler));
  std::vector<std::shared_ptr<listener>> values = m_impl->snapshot();
  std::shared_ptr<server_control> control = m_impl->m_control;
  std::function<boost::system::error_code(boost::system::error_code)> finished =
      [control, values](boost::system::error_code ec) { return control->finish_start(values, ec); };
  boost::asio::cancellation_slot cancellation = boost::asio::get_associated_cancellation_slot(handler);
  std::shared_ptr<listener_operation_group> state;
  if (!values.empty()) {
    state = std::make_shared<listener_operation_group>(values, true, m_impl->m_executor, finished, std::move(handler));
    state->prepare(std::move(cancellation));
  }
  if (!control->begin_start(values)) {
    if (state)
      state->reject(boost::asio::error::operation_aborted);
    else
      detail::complete_on(m_impl->m_executor, std::move(handler), boost::asio::error::operation_aborted);
    return;
  }
  if (values.empty()) {
    const boost::system::error_code ec = finished({});
    detail::complete_on(m_impl->m_executor, std::move(handler), ec);
    return;
  }
  state->start(false);
  if (control->stop_requested()) state->request_stop();
}

void server::do_async_wait(listener::completion_handler handler)
{
  handler = detail::track_completion_work(m_impl->m_executor, std::move(handler));
  std::vector<std::shared_ptr<listener>> values = m_impl->snapshot();
  std::shared_ptr<server_control> control = m_impl->m_control;
  std::function<boost::system::error_code(boost::system::error_code)> finished =
      [control, values](boost::system::error_code ec) { return control->finish_wait(values, ec); };
  if (values.empty()) {
    const boost::system::error_code ec = finished({});
    detail::complete_on(m_impl->m_executor, std::move(handler), ec);
    return;
  }
  boost::asio::cancellation_slot cancellation = boost::asio::get_associated_cancellation_slot(handler);
  std::shared_ptr<listener_operation_group> state = std::make_shared<listener_operation_group>(
      std::move(values), false, m_impl->m_executor, std::move(finished), std::move(handler));
  state->prepare(std::move(cancellation));
  state->start(true);
}

} // namespace serveza
