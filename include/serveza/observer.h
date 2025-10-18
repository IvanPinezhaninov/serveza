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

#ifndef SERVEZA_OBSERVER_H
#define SERVEZA_OBSERVER_H

#include <cstdint>
#include <exception>
#include <memory>

#include <boost/system/error_code.hpp>

#include <serveza/connection_info.h>
#include <serveza/export.h>
#include <serveza/listener_status.h>
#include <serveza/server_status.h>

namespace serveza {

/** @brief Kind of diagnostic or lifecycle event emitted by a listener. */
enum class listener_event_type {
  state_changed,
  accept_failed,
  session_factory_failed,
  session_started,
  session_stopped,
  shutdown_forced
};

/** @brief Immutable event delivered on the listener's serialized executor. */
struct listener_event {
  listener_event_type type{listener_event_type::state_changed};
  listener_status status;
  connection_info connection;
  boost::system::error_code error;
  std::exception_ptr exception;
};

/** @brief Kind of coordinated server lifecycle event. */
enum class server_event_type { starting, running, stopping, stopped };

/** @brief Immutable aggregate event delivered through the server executor. */
struct server_event {
  server_event_type type{server_event_type::starting};
  server_status status;
  boost::system::error_code error;
};

namespace detail {
class observer_registry;
}

/** @brief Move-only registration handle that unregisters its observer on destruction. */
class SERVEZA_API observer_subscription {
public:
  observer_subscription() noexcept = default;
  ~observer_subscription();

  observer_subscription(observer_subscription&& other) noexcept;
  observer_subscription& operator=(observer_subscription&& other) noexcept;

  observer_subscription(const observer_subscription&) = delete;
  observer_subscription& operator=(const observer_subscription&) = delete;

  /** @brief Unregisters the observer. Safe to call from inside an observer callback. */
  void reset() noexcept;

  [[nodiscard]] explicit operator bool() const noexcept;

private:
  friend class server;
  observer_subscription(std::weak_ptr<detail::observer_registry> registry, std::uint64_t id) noexcept;

  std::weak_ptr<detail::observer_registry> m_registry;
  std::uint64_t m_id{};
};

/**
 * @brief Receives listener lifecycle, failure and session events.
 *
 * Listener callbacks run on the publishing listener strand and server callbacks
 * run on the server executor. Calls to all observers registered with one server
 * are globally serialized. They must not block. Exceptions thrown by an observer
 * are ignored so they cannot interrupt transport processing.
 */
class SERVEZA_API observer {
public:
  virtual ~observer();
  virtual void on_event(const listener_event& event) = 0;
  virtual void on_server_event(const server_event& event);
};

} // namespace serveza

#endif // SERVEZA_OBSERVER_H
