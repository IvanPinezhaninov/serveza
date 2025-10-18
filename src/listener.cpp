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

#include <serveza/listener.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <utility>

#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/bind_allocator.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/execution/outstanding_work.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/prefer.hpp>
#include <boost/asio/system_executor.hpp>

namespace serveza {

listener::~listener() = default;

namespace detail {

listener::completion_handler track_completion_work(boost::asio::any_io_executor fallback,
                                                   listener::completion_handler handler)
{
  auto executor = boost::asio::get_associated_executor(handler, fallback);
  auto allocator = boost::asio::get_associated_allocator(handler);
  auto cancellation = boost::asio::get_associated_cancellation_slot(handler);
  auto work = boost::asio::prefer(executor, boost::asio::execution::outstanding_work.tracked);
  auto completion = boost::asio::bind_executor(
      executor,
      boost::asio::bind_allocator(allocator, boost::asio::bind_cancellation_slot(
                                                 cancellation, [handler = std::move(handler), work = std::move(work)](
                                                                   boost::system::error_code ec) mutable {
                                                   static_cast<void>(work);
                                                   handler(ec);
                                                 })));
  return listener::completion_handler{std::move(completion)};
}

void complete_on(boost::asio::any_io_executor fallback, listener::completion_handler handler,
                 boost::system::error_code ec)
{
  auto executor = boost::asio::get_associated_executor(handler, fallback);
  auto allocator = boost::asio::get_associated_allocator(handler);
  auto completion =
      boost::asio::bind_allocator(allocator, [handler = std::move(handler), ec]() mutable { handler(ec); });
  boost::asio::post(std::move(fallback),
                    boost::asio::bind_allocator(
                        allocator, [executor = std::move(executor), completion = std::move(completion)]() mutable {
                          boost::asio::dispatch(executor, std::move(completion));
                        }));
}

void complete_on_associated(listener::completion_handler handler, boost::system::error_code ec)
{
  auto executor = boost::asio::prefer(boost::asio::get_associated_executor(handler, boost::asio::system_executor{}),
                                      boost::asio::execution::outstanding_work.tracked);
  auto allocator = boost::asio::get_associated_allocator(handler);
  auto completion =
      boost::asio::bind_allocator(allocator, [handler = std::move(handler), ec]() mutable { handler(ec); });
  boost::asio::post(boost::asio::system_executor{},
                    boost::asio::bind_allocator(
                        allocator, [executor = std::move(executor), completion = std::move(completion)]() mutable {
                          boost::asio::dispatch(executor, std::move(completion));
                        }));
}

void validate_acceptor_options(const listener_options& options)
{
  if (options.listen_backlog <= 0) throw std::invalid_argument{"listen_backlog must be greater than zero"};
  validate_socket_options(options);
  if (options.max_active_sessions == 0) {
    throw std::invalid_argument{"max_active_sessions must be greater than zero"};
  }
  if (options.accept_error_backoff < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument{"accept_error_backoff must not be negative"};
  }
  if (options.max_accept_error_backoff < options.accept_error_backoff) {
    throw std::invalid_argument{"max_accept_error_backoff must not be less than accept_error_backoff"};
  }
  if (options.max_consecutive_accept_errors == 0) {
    throw std::invalid_argument{"max_consecutive_accept_errors must be greater than zero"};
  }
}

void validate_socket_options(const listener_options& options)
{
  if (options.shutdown_grace_period < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument{"shutdown_grace_period must not be negative"};
  }
}

bool is_transient_accept_error(const boost::system::error_code& ec) noexcept
{
  return ec == boost::asio::error::connection_aborted || ec == boost::asio::error::interrupted ||
         ec == boost::asio::error::try_again || ec == boost::asio::error::would_block ||
         ec == boost::asio::error::no_descriptors || ec == boost::asio::error::no_buffer_space ||
         ec == boost::asio::error::no_memory;
}

std::chrono::milliseconds accept_retry_delay(const listener_options& options, std::size_t consecutive_errors) noexcept
{
  std::chrono::milliseconds delay = options.accept_error_backoff;
  for (std::size_t i = 1; i < consecutive_errors && delay < options.max_accept_error_backoff; ++i) {
    if (delay > options.max_accept_error_backoff / 2)
      delay = options.max_accept_error_backoff;
    else
      delay *= 2;
  }
  return std::min(delay, options.max_accept_error_backoff);
}

session_control::~session_control() = default;

} // namespace detail

} // namespace serveza
