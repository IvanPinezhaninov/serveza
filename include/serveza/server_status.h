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

#ifndef SERVEZA_SERVER_STATUS_H
#define SERVEZA_SERVER_STATUS_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include <serveza/listener_status.h>

namespace serveza {

/** @brief Coordinated lifecycle state of a server listener snapshot. */
enum class server_state { idle, starting, running, stopping, stopped };

/** @brief Thread-safe aggregate snapshot of the server and all registered listeners. */
struct server_status {
  server_state state{server_state::idle};
  std::uint64_t generation{};
  std::size_t active_sessions{};
  bool ready{};
  std::vector<listener_status> listeners;
};

} // namespace serveza

#endif // SERVEZA_SERVER_STATUS_H
