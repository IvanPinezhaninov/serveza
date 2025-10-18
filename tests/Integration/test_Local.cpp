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
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include <boost/asio.hpp>

#include <gtest/gtest.h>

#include <serveza/serveza.h>

#if defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)

namespace net = boost::asio;
namespace local = net::local;
namespace sys = boost::system;

namespace {

class LocalSocketPath final {
public:
  explicit LocalSocketPath(const char* label)
  {
    static std::atomic_uint64_t sequence{};
    const std::uint64_t value = sequence.fetch_add(1, std::memory_order_relaxed);
    const std::int64_t timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    m_path =
        std::filesystem::temp_directory_path() /
        (std::string{"serveza-"} + label + '-' + std::to_string(timestamp) + '-' + std::to_string(value) + ".sock");
    std::error_code ignored;
    std::filesystem::remove(m_path, ignored);
  }

  ~LocalSocketPath()
  {
    std::error_code ignored;
    std::filesystem::remove(m_path, ignored);
  }

  LocalSocketPath(const LocalSocketPath&) = delete;
  LocalSocketPath& operator=(const LocalSocketPath&) = delete;

  [[nodiscard]] std::string string() const
  {
    return m_path.string();
  }

private:
  std::filesystem::path m_path;
};

struct LocalStreamEcho {
  template<typename Context, typename Handler>
  void operator()(Context& ctx, Handler handler) const
  {
    std::shared_ptr<std::array<char, 1>> data = std::make_shared<std::array<char, 1>>();
    typename Context::socket_type* socket = &ctx.socket();
    socket->async_read_some(
        net::buffer(*data), [socket, data, handler = std::move(handler)](sys::error_code ec, std::size_t size) mutable {
          if (ec) {
            handler(ec);
            return;
          }
          net::async_write(
              *socket, net::buffer(data->data(), size),
              [data, handler = std::move(handler)](sys::error_code writeEc, std::size_t) mutable { handler(writeEc); });
        });
  }
};

template<typename Handler>
class LocalDatagramEchoOperation final : public std::enable_shared_from_this<LocalDatagramEchoOperation<Handler>> {
public:
  LocalDatagramEchoOperation(local::datagram_protocol::socket& socket, Handler handler)
    : m_socket{socket}
    , m_handler{std::move(handler)}
  {}

  void start()
  {
    receive();
  }

private:
  void receive()
  {
    std::shared_ptr<LocalDatagramEchoOperation> self = this->shared_from_this();
    m_socket.async_receive_from(net::buffer(m_data), m_remote, [self](sys::error_code ec, std::size_t size) {
      if (ec == net::error::operation_aborted) return self->finish({});
      if (ec) return self->finish(ec);
      self->send(size);
    });
  }

  void send(std::size_t size)
  {
    std::shared_ptr<LocalDatagramEchoOperation> self = this->shared_from_this();
    m_socket.async_send_to(net::buffer(m_data.data(), size), m_remote, [self](sys::error_code ec, std::size_t) {
      if (ec == net::error::operation_aborted) return self->finish({});
      if (ec) return self->finish(ec);
      self->receive();
    });
  }

  void finish(sys::error_code ec)
  {
    Handler handler = std::move(m_handler);
    handler(ec);
  }

  local::datagram_protocol::socket& m_socket;
  Handler m_handler;
  std::array<char, 64> m_data{};
  local::datagram_protocol::endpoint m_remote;
};

struct LocalDatagramEcho {
  template<typename Handler>
  void operator()(serveza::bound_socket_context<local::datagram_protocol>& ctx, Handler handler) const
  {
    using Operation = LocalDatagramEchoOperation<std::decay_t<Handler>>;
    std::make_shared<Operation>(ctx.socket(), std::move(handler))->start();
  }
};

TEST(LocalSocketTests, StreamSessionEchoesData)
{
  LocalSocketPath serverPath{"stream"};
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener =
      server.listen<local::stream_protocol>(local::stream_protocol::endpoint{serverPath.string()},
                                            [] { return serveza::callback_session{LocalStreamEcho{}}; });

  bool echoed = false;
  bool stopped = false;
  char request = 's';
  char response{};
  local::stream_protocol::socket client{io};
  net::steady_timer deadline{io, std::chrono::seconds{2}};
  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
  });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client.async_connect(local::stream_protocol::endpoint{serverPath.string()}, [&](sys::error_code connectEc) {
      ASSERT_FALSE(connectEc);
      net::async_write(client, net::buffer(&request, 1), [&](sys::error_code writeEc, std::size_t) {
        ASSERT_FALSE(writeEc);
        net::async_read(client, net::buffer(&response, 1), [&](sys::error_code readEc, std::size_t) {
          echoed = !readEc && response == request;
          deadline.cancel();
          server.request_stop();
        });
      });
    });
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    ADD_FAILURE() << "local stream echo timed out";
    server.request_stop();
  });

  io.run();

  EXPECT_TRUE(echoed);
  EXPECT_TRUE(stopped);
  EXPECT_EQ(listener->active_sessions(), 0u);
}

TEST(LocalSocketTests, DatagramSessionEchoesData)
{
#if defined(_WIN32)
  GTEST_SKIP() << "Windows does not reliably support local datagram socket endpoints";
#else

  LocalSocketPath serverPath{"datagram-server"};
  LocalSocketPath clientPath{"datagram-client"};
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener =
      server.bind<local::datagram_protocol>(local::datagram_protocol::endpoint{serverPath.string()},
                                            [] { return serveza::callback_session{LocalDatagramEcho{}}; });

  local::datagram_protocol::socket client{io};
  sys::error_code ec;
  client.open(local::datagram_protocol{}, ec);
  ASSERT_FALSE(ec);
  client.bind(local::datagram_protocol::endpoint{clientPath.string()}, ec);
  ASSERT_FALSE(ec);

  const std::string request{"local datagram"};
  std::array<char, 64> response{};
  local::datagram_protocol::endpoint sender;
  bool echoed = false;
  bool stopped = false;
  net::steady_timer deadline{io, std::chrono::seconds{2}};
  server.async_wait([&](sys::error_code waitEc) {
    EXPECT_FALSE(waitEc);
    stopped = !waitEc;
  });
  server.async_start([&](sys::error_code startEc) {
    ASSERT_FALSE(startEc);
    client.async_send_to(net::buffer(request), local::datagram_protocol::endpoint{serverPath.string()},
                         [&](sys::error_code sendEc, std::size_t size) {
                           ASSERT_FALSE(sendEc);
                           ASSERT_EQ(size, request.size());
                           client.async_receive_from(
                               net::buffer(response), sender, [&](sys::error_code receiveEc, std::size_t received) {
                                 echoed = !receiveEc && std::string{response.data(), received} == request;
                                 deadline.cancel();
                                 server.request_stop();
                               });
                         });
  });
  deadline.async_wait([&](sys::error_code timerEc) {
    if (timerEc) return;
    ADD_FAILURE() << "local datagram echo timed out";
    server.request_stop();
  });

  io.run();

  EXPECT_TRUE(echoed);
  EXPECT_TRUE(stopped);
  EXPECT_EQ(listener->active_sessions(), 0u);
#endif // defined(_WIN32)
}

} // namespace

#endif
