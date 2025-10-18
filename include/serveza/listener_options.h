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

#ifndef SERVEZA_LISTENER_OPTIONS_H
#define SERVEZA_LISTENER_OPTIONS_H

#include <chrono>
#include <cstddef>

#include <boost/asio/socket_base.hpp>

namespace serveza {

/**
 * @brief Operational limits for an accepting listener.
 *
 * A stream listener pauses acceptance while @ref max_active_sessions sessions
 * are active. Transient accept errors use bounded exponential backoff.
 * Shutdown first requests cooperative cancellation and force-closes remaining
 * sockets after @ref shutdown_grace_period.
 */
struct listener_options {
  /** @brief Backlog supplied to the protocol's listen operation. */
  int listen_backlog{128};

  /** @brief Time allowed for cooperative session shutdown. */
  std::chrono::milliseconds shutdown_grace_period{5000};

  /** @brief Maximum number of concurrently active sessions. */
  std::size_t max_active_sessions{1024};

  /** @brief Initial delay before retrying a transient asynchronous accept failure. */
  std::chrono::milliseconds accept_error_backoff{100};

  /** @brief Maximum delay between transient accept retries. */
  std::chrono::milliseconds max_accept_error_backoff{5000};

  /** @brief Consecutive transient accept failures allowed before stopping. */
  std::size_t max_consecutive_accept_errors{8};

  /** @brief Whether an IPv6 acceptor accepts IPv6 traffic only. */
  bool v6_only{true};

#if defined(_WIN32)
  /** @brief Whether IP acceptors enable address reuse. */
  bool reuse_address{false};
#else
  /** @brief Whether IP acceptors enable address reuse. */
  bool reuse_address{true};
#endif
};

} // namespace serveza

#endif // SERVEZA_LISTENER_OPTIONS_H
