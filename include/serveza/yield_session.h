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

#ifndef SERVEZA_YIELD_SESSION_H
#define SERVEZA_YIELD_SESSION_H

#include <exception>
#include <utility>

#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/system/error_code.hpp>

#include <serveza/detail/exception.h>

namespace serveza {

/**
 * @brief Adapts a stackful `yield_context` session while keeping the listener callback based.
 *
 * Including this header requires the Boost.Coroutine and Boost.Context headers.
 * The consuming target must link Boost.Coroutine. Exceptions are converted to
 * the completion error and retained for observer diagnostics.
 */
template<typename Function>
class yield_session {
public:
  explicit yield_session(Function function)
    : m_function{std::move(function)}
  {}

  template<typename Context, typename CompletionToken>
  auto async_run(Context& ctx, CompletionToken&& token)
  {
    return boost::asio::async_initiate<CompletionToken, void(boost::system::error_code)>(
        [this, &ctx](auto handler) mutable {
          auto cancellation = boost::asio::get_associated_cancellation_slot(handler);
          boost::asio::spawn(
              ctx.get_executor(), [this, &ctx](boost::asio::yield_context yield) { m_function(ctx, yield); },
              boost::asio::bind_cancellation_slot(cancellation,
                                                  [&ctx, handler = std::move(handler)](std::exception_ptr ep) mutable {
                                                    const boost::system::error_code ec = detail::exception_to_error(ep);
                                                    ctx.report_exception(std::move(ep));
                                                    handler(ec);
                                                  }));
        },
        token);
  }

private:
  Function m_function;
};

template<typename Function>
yield_session(Function) -> yield_session<Function>;

} // namespace serveza

#endif // SERVEZA_YIELD_SESSION_H
