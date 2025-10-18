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

#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <boost/asio.hpp>

#include <gtest/gtest.h>

#include <serveza/serveza.h>
#include <serveza/yield_session.h>

namespace net = boost::asio;
namespace sys = boost::system;
using tcp = net::ip::tcp;

template<typename Handler>
class EchoState : public std::enable_shared_from_this<EchoState<Handler>> {
public:
  EchoState(tcp::socket& socket, Handler handler)
    : m_socket{socket}
    , m_handler{std::move(handler)}
  {}

  void start()
  {
    read();
  }

private:
  void read()
  {
    auto self = this->shared_from_this();
    m_socket.async_read_some(net::buffer(m_buffer), [self](sys::error_code error, std::size_t size) {
      if (error) return self->m_handler(sys::error_code{});
      net::async_write(self->m_socket, net::buffer(self->m_buffer.data(), size),
                       [self](sys::error_code writeError, std::size_t) {
                         if (writeError) return self->m_handler(sys::error_code{});
                         self->read();
                       });
    });
  }

  tcp::socket& m_socket;
  Handler m_handler;
  std::array<char, 1024> m_buffer{};
};

struct Echo {
  template<typename Context, typename Handler>
  void operator()(Context& context, Handler handler) const
  {
    std::make_shared<EchoState<std::decay_t<Handler>>>(context.socket(), std::move(handler))->start();
  }
};

template<typename SessionFactory>
bool runCase(const char* name, SessionFactory factory)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener =
      server.listen<tcp>({net::ip::address_v4::loopback(), 0}, std::move(factory),
                         serveza::listener_options{128, std::chrono::milliseconds{250}});

  bool started = false;
  bool stopped = false;
  bool echoed = false;
  std::thread client;

  server.async_wait([&](sys::error_code error) {
    if (error) std::cerr << "wait: " << error.message() << '\n';
    stopped = !error;
  });

  server.async_start([&](sys::error_code error) {
    if (error) {
      std::cerr << "start: " << error.message() << '\n';
      server.request_stop();
      return;
    }
    started = true;
    const auto text = listener->endpoint();
    const auto separator = text.rfind(':');
    const auto port = static_cast<unsigned short>(std::stoul(text.substr(separator + 1)));
    client = std::thread([&, port] {
      try {
        net::io_context clientIo;
        tcp::socket socket{clientIo};
        socket.connect({net::ip::address_v4::loopback(), port});
        const std::string input{"serveza"};
        net::write(socket, net::buffer(input));
        std::string output(input.size(), '\0');
        net::read(socket, net::buffer(output));
        echoed = output == input;
      } catch (const std::exception& exception) {
        std::cerr << "client: " << exception.what() << '\n';
      }
      server.request_stop();
    });
  });

  io.run();
  if (client.joinable()) client.join();

  if (!started || !stopped || !echoed) {
    std::cerr << name << ": started=" << started << " stopped=" << stopped << " echoed=" << echoed << '\n';
    return false;
  }
  return true;
}

TEST(AdapterTests, CallbackSessionEchoesData)
{
  EXPECT_TRUE(runCase("callback", [] { return serveza::callback_session{Echo{}}; }));
}

TEST(AdapterTests, YieldSessionEchoesData)
{
  EXPECT_TRUE(runCase("yield", [] {
    return serveza::yield_session{[](auto& context, net::yield_context yield) {
      std::array<char, 1024> buffer{};
      sys::error_code error;
      for (;;) {
        const auto size = context.socket().async_read_some(net::buffer(buffer), yield[error]);
        if (error) return;
        net::async_write(context.socket(), net::buffer(buffer.data(), size), yield[error]);
        if (error) return;
      }
    }};
  }));
}

TEST(AdapterTests, YieldTokenControlsServerLifecycle)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  server.listen<tcp>({net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{Echo{}}; });
  bool started = false;
  bool stopped = false;

  net::spawn(
      io,
      [&](net::yield_context yield) {
        sys::error_code ec;
        server.async_start(yield[ec]);
        ASSERT_FALSE(ec);
        started = !ec;
        server.request_stop();
        server.async_wait(yield[ec]);
        EXPECT_FALSE(ec);
        stopped = !ec;
      },
      net::detached);

  io.run();
  EXPECT_TRUE(started);
  EXPECT_TRUE(stopped);
}
