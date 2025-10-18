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

#include <cstddef>
#include <cstdlib>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/system/error_code.hpp>

#include <serveza/serveza.h>

namespace net = boost::asio;
using tcp = net::ip::tcp;
using udp = net::ip::udp;

namespace {

struct ProbeSession {
  template<typename Context, typename Handler>
  void operator()(Context&, Handler handler) const
  {
    handler(boost::system::error_code{});
  }
};

} // namespace

int main()
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  server.listen<tcp>({net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ProbeSession{}}; });
  server.bind<udp>({net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{ProbeSession{}}; });
  return server.listeners().size() == std::size_t{2} ? EXIT_SUCCESS : EXIT_FAILURE;
}
