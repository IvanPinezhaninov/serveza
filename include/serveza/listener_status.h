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

#ifndef SERVEZA_LISTENER_STATUS_H
#define SERVEZA_LISTENER_STATUS_H

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

#include <boost/system/error_code.hpp>

namespace serveza {

/** @brief Lifecycle state of an accepting listener. */
enum class listener_state { idle, starting, running, stopping, stopped };

/** @brief Why the most recent listener generation stopped. */
enum class listener_stop_reason { none, requested, completed, startup_failure, runtime_failure };

/** @brief Thread-safe snapshot of listener lifecycle and diagnostic state. */
struct listener_status {
  listener_state state{listener_state::idle};
  listener_stop_reason stop_reason{listener_stop_reason::none};
  std::uint64_t listener_id{};
  std::uint64_t generation{};
  std::size_t active_sessions{};
  std::string endpoint;
  boost::system::error_code last_error;
  std::exception_ptr last_exception;
};

} // namespace serveza

#endif // SERVEZA_LISTENER_STATUS_H
