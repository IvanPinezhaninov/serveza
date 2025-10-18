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

#include "BaseTest.h"

namespace ip = boost::asio::ip;

namespace {

class Session final : public serveza::session<ip::udp> {
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

class UdpSessionTest : public BaseTest {
protected:
  sys::error_code ec;
  net::io_context ioc;
  Session::protocol_type::socket socket{ioc};
  Session::protocol_type::endpoint ep{ip::address_v4::loopback(), serverPort};
  std::string req = "Hello World!";
};

TEST_F(UdpSessionTest, SendAndReceive)
{
  auto listener = makeListener<Session>(ep);
  ASSERT_TRUE(startListener(listener));

  socket.open(Session::protocol_type::v4(), ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(socket.is_open());

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

TEST_F(UdpSessionTest, SendEmptyPacket)
{
  auto listener = makeListener<Session>(ep);
  ASSERT_TRUE(startListener(listener));

  socket.open(Session::protocol_type::v4(), ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(socket.is_open());

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
