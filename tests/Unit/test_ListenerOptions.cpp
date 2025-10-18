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

#include <chrono>
#include <stdexcept>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>

#include <boost/system/error_code.hpp>

#include <gtest/gtest.h>

#include <serveza/serveza.h>

namespace net = boost::asio;
using tcp = net::ip::tcp;
using udp = net::ip::udp;

namespace {

struct NoopSession {
  template<typename Context, typename Handler>
  void operator()(Context&, Handler handler) const
  {
    handler(boost::system::error_code{});
  }
};

template<typename Configure>
void expectInvalidOptions(Configure configure)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  serveza::listener_options options;
  configure(options);
  EXPECT_THROW(
      server.listen<tcp>(
          {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{NoopSession{}}; }, options),
      std::invalid_argument);
}

TEST(ListenerOptionsTests, RejectsInvalidOperationalLimits)
{
  expectInvalidOptions([](serveza::listener_options& options) { options.listen_backlog = 0; });
  expectInvalidOptions(
      [](serveza::listener_options& options) { options.shutdown_grace_period = std::chrono::milliseconds{-1}; });
  expectInvalidOptions([](serveza::listener_options& options) { options.max_active_sessions = 0; });
  expectInvalidOptions(
      [](serveza::listener_options& options) { options.accept_error_backoff = std::chrono::milliseconds{-1}; });
  expectInvalidOptions([](serveza::listener_options& options) {
    options.accept_error_backoff = std::chrono::milliseconds{2};
    options.max_accept_error_backoff = std::chrono::milliseconds{1};
  });
  expectInvalidOptions([](serveza::listener_options& options) { options.max_consecutive_accept_errors = 0; });

  net::io_context io;
  serveza::server server{io.get_executor()};
  serveza::listener_options options;
  options.shutdown_grace_period = std::chrono::milliseconds{-1};
  EXPECT_THROW(
      server.bind<udp>(
          {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{NoopSession{}}; }, options),
      std::invalid_argument);
}

TEST(ListenerOptionsTests, ComputesBoundedExponentialAcceptRetryDelay)
{
  serveza::listener_options options;
  options.accept_error_backoff = std::chrono::milliseconds{5};
  options.max_accept_error_backoff = std::chrono::milliseconds{12};

  EXPECT_EQ(serveza::detail::accept_retry_delay(options, 1), std::chrono::milliseconds{5});
  EXPECT_EQ(serveza::detail::accept_retry_delay(options, 2), std::chrono::milliseconds{10});
  EXPECT_EQ(serveza::detail::accept_retry_delay(options, 3), std::chrono::milliseconds{12});
  EXPECT_EQ(serveza::detail::accept_retry_delay(options, 100), std::chrono::milliseconds{12});
}

} // namespace
