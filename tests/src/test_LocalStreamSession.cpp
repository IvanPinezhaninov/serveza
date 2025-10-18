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

#include <filesystem>

#include "BaseTest.h"

namespace fs = std::filesystem;
namespace local = net::local;

namespace {

class Session final : public serveza::session<local::stream_protocol> {
public:
  void operator()(net::any_io_executor ex, protocol_type::socket socket, net::yield_context yield,
                  const sys::error_code& ec) noexcept override
  {
    net::streambuf buffer;

    std::size_t received = socket.async_read_some(buffer.prepare(32), yield);
    if (!ec && received > 0) {
      buffer.commit(received);
      net::async_write(socket, buffer.data(), yield);
      buffer.consume(received);
    }

    if (socket.is_open()) {
      sys::error_code ec;
      socket.close(ec);
    }
  }
};

class LocalStreamSessionTest : public BaseTest {
protected:
  void TearDown() override
  {
    BaseTest::TearDown();
    fs::remove(socket_path.c_str());
  }

  sys::error_code ec;
  net::io_context ioc;
  Session::protocol_type::socket socket{ioc};
  std::string socket_path = "/tmp/test_socket_" + std::to_string(::getpid());
  Session::protocol_type::endpoint ep{socket_path};
  std::string req = "Hello World!";
};

TEST_F(LocalStreamSessionTest, ConnectAndDisconnect)
{
  auto listener = makeListener<Session>(ep);
  ASSERT_TRUE(startListener(listener));

  socket.connect(ep, ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(socket.is_open());

  socket.close(ec);
  ASSERT_FALSE(ec);
  ASSERT_FALSE(socket.is_open());
}

TEST_F(LocalStreamSessionTest, SendAndReceive)
{
  auto listener = makeListener<Session>(ep);
  ASSERT_TRUE(startListener(listener));

  socket.connect(ep);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(socket.is_open());

  auto sent = net::write(socket, net::buffer(req), ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(sent, req.size());

  std::string res(req.size(), ' ');

  auto received = net::read(socket, net::buffer(res), ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(received, req.size());
  ASSERT_EQ(res, req);

  socket.close(ec);
  ASSERT_FALSE(ec);
  ASSERT_FALSE(socket.is_open());
}

TEST_F(LocalStreamSessionTest, SendEmptyPayload)
{
  auto listener = makeListener<Session>(ep);
  ASSERT_TRUE(startListener(listener));

  socket.connect(ep, ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(socket.is_open());

  auto written = net::write(socket, net::buffer("", 0), ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(written, 0u);

  socket.close(ec);
  ASSERT_FALSE(ec);
  ASSERT_FALSE(socket.is_open());
}

} // namespace
