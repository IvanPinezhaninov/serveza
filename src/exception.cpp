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

#include <serveza/detail/exception.h>

#include <utility>

#include <boost/system/system_error.hpp>

namespace serveza::detail {

boost::system::error_code exception_to_error(std::exception_ptr ep) noexcept
{
  if (!ep) return {};
  try {
    std::rethrow_exception(std::move(ep));
  } catch (const boost::system::system_error& e) {
    return e.code();
  } catch (...) {
    return make_error_code(boost::system::errc::io_error);
  }
}

void report_exception(const std::function<void(std::exception_ptr)>& reporter, std::exception_ptr ep) noexcept
{
  if (!reporter) return;
  try {
    reporter(std::move(ep));
  } catch (...) {}
}

} // namespace serveza::detail
