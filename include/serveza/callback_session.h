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

#ifndef SERVEZA_CALLBACK_SESSION_H
#define SERVEZA_CALLBACK_SESSION_H

#include <utility>

#include <boost/asio/async_result.hpp>
#include <boost/system/error_code.hpp>

namespace serveza {

/**
 * @brief Adapts `void(session_context<Protocol>&, CompletionHandler)` sessions.
 *
 * The adapted function owns the session protocol and must invoke its move-only
 * completion handler exactly once. Retaining the handler after forced shutdown
 * keeps the session storage alive, but the closed socket must no longer be used.
 */
template<typename Function>
class callback_session {
public:
  explicit callback_session(Function function)
    : m_function{std::move(function)}
  {}

  template<typename Context, typename CompletionToken>
  auto async_run(Context& ctx, CompletionToken&& token)
  {
    return boost::asio::async_initiate<CompletionToken, void(boost::system::error_code)>(
        [this, &ctx](auto handler) mutable { m_function(ctx, std::move(handler)); }, token);
  }

private:
  Function m_function;
};

template<typename Function>
callback_session(Function) -> callback_session<Function>;

} // namespace serveza

#endif // SERVEZA_CALLBACK_SESSION_H
