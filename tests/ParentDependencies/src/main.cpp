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
** OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
** OR OTHER DEALINGS IN THE SOFTWARE.
**
******************************************************************************/

#include <cstdlib>
#include <utility>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <serveza/serveza.h>

namespace net = boost::asio;
using tcp = net::ip::tcp;

int main()
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>({net::ip::address_v4::loopback(), 0}, [] {
    return serveza::callback_session{
        [](serveza::session_context<tcp>&, auto handler) { handler(boost::system::error_code{}); }};
  });
  return listener->id() == 1 ? EXIT_SUCCESS : EXIT_FAILURE;
}
