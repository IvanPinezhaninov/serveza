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
namespace local = boost::asio::local;

namespace {

class Session final : public serveza::session<local::datagram_protocol> {
public:
  void operator()(protocol_type::socket socket, net::yield_context yield) override
  {
    std::array<char, 32> data;
    protocol_type::endpoint ep;
    sys::error_code ec;

    std::size_t received = socket.async_receive_from(net::buffer(data), ep, yield[ec]);

    if (!ec) socket.async_send_to(net::buffer(data, received), ep, yield[ec]);

    if (socket.is_open()) socket.close(ec);
  }
};

class LocalDatagramSessionTest : public BaseTest {
protected:
  void TearDown() override
  {
    BaseTest::TearDown();
    fs::remove(server_socket_path.c_str());
    fs::remove(client_socket_path.c_str());
  }

  sys::error_code ec;
  net::io_context ioc;
  Session::protocol_type::socket socket{ioc};
  std::string server_socket_path = "/tmp/test_socket_" + std::to_string(::getpid());
  std::string client_socket_path = server_socket_path + "_client";
  Session::protocol_type::endpoint ep{server_socket_path};
  std::string req = "Hello World!";
};

TEST_F(LocalDatagramSessionTest, SendAndReceive)
{
  auto listener = makeListener<Session>(ep);
  ASSERT_TRUE(startListener(listener));

  socket.open();
  ASSERT_TRUE(socket.is_open());

  socket.bind(Session::protocol_type::endpoint(client_socket_path), ec);
  ASSERT_FALSE(ec);

  auto sent = socket.send_to(net::buffer(req), ep, 0, ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(sent, req.size());

  std::string res(req.size(), ' ');
  Session::protocol_type::endpoint sender;
  auto received = socket.receive_from(net::buffer(res), sender, 0, ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(received, req.size());
  ASSERT_EQ(res, req);

  socket.close(ec);
  ASSERT_FALSE(ec);
  ASSERT_FALSE(socket.is_open());
}

TEST_F(LocalDatagramSessionTest, SendEmptyPacket)
{
  auto listener = makeListener<Session>(ep);
  ASSERT_TRUE(startListener(listener));

  socket.open();
  ASSERT_TRUE(socket.is_open());

  socket.bind(Session::protocol_type::endpoint(client_socket_path));
  ASSERT_FALSE(ec);

  auto sent = socket.send_to(net::buffer("", 0), ep, 0, ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(sent, 0u);

  std::array<char, 1> res{};
  Session::protocol_type::endpoint sender;
  auto received = socket.receive_from(net::buffer(res), sender, 0, ec);
  ASSERT_FALSE(ec);
  ASSERT_EQ(received, 0u);

  socket.close(ec);
  ASSERT_FALSE(ec);
  ASSERT_FALSE(socket.is_open());
}

} // namespace
