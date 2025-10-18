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

#ifndef SERVEZA_SESSION_CONTEXT_H
#define SERVEZA_SESSION_CONTEXT_H

#include <cstdint>
#include <exception>
#include <functional>
#include <utility>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>

#include <serveza/connection_info.h>
#include <serveza/detail/exception.h>

namespace serveza {

/** @brief Socket, metadata and cancellation state supplied to one session. */
template<typename Protocol>
class session_context {
public:
  using protocol_type = Protocol;
  using socket_type = typename protocol_type::socket;
  using exception_reporter = std::function<void(std::exception_ptr)>;

  session_context(socket_type& socket, std::uint64_t listener_id, connection_info info,
                  boost::asio::cancellation_slot cancellation, exception_reporter report_exception = {}) noexcept
    : m_socket{socket}
    , m_listener_id{listener_id}
    , m_info{std::move(info)}
    , m_cancellation{std::move(cancellation)}
    , m_report_exception{std::move(report_exception)}
  {}

  [[nodiscard]] socket_type& socket() const noexcept
  {
    return m_socket;
  }

  [[nodiscard]] boost::asio::any_io_executor get_executor() const
  {
    return m_socket.get_executor();
  }

  [[nodiscard]] std::uint64_t listener_id() const noexcept
  {
    return m_listener_id;
  }

  [[nodiscard]] const connection_info& info() const noexcept
  {
    return m_info;
  }

  [[nodiscard]] boost::asio::cancellation_slot cancellation_slot() const noexcept
  {
    return m_cancellation;
  }

  void report_exception(std::exception_ptr ep) const noexcept
  {
    detail::report_exception(m_report_exception, std::move(ep));
  }

private:
  socket_type& m_socket;
  std::uint64_t m_listener_id{};
  connection_info m_info;
  boost::asio::cancellation_slot m_cancellation;
  exception_reporter m_report_exception;
};

} // namespace serveza

#endif // SERVEZA_SESSION_CONTEXT_H
