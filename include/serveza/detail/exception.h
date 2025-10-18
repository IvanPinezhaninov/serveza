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

#ifndef SERVEZA_DETAIL_EXCEPTION_H
#define SERVEZA_DETAIL_EXCEPTION_H

#include <exception>
#include <functional>

#include <boost/system/error_code.hpp>

#include <serveza/export.h>

namespace serveza::detail {

[[nodiscard]] SERVEZA_API boost::system::error_code exception_to_error(std::exception_ptr ep) noexcept;

SERVEZA_API void report_exception(const std::function<void(std::exception_ptr)>& reporter,
                                  std::exception_ptr ep) noexcept;

} // namespace serveza::detail

#endif // SERVEZA_DETAIL_EXCEPTION_H
