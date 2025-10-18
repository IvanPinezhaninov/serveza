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

class Session final : public serveza::session<ip::tcp> {
public:
  explicit Session(std::atomic_bool& running)
    : m_running{running}
  {}

  void operator()(protocol_type::socket socket, net::yield_context yield) override
  {
    m_running.store(true, std::memory_order_release);

    sys::error_code ec;
    net::steady_timer timer{yield.get_executor()};
    timer.expires_after(std::chrono::seconds{5});
    timer.async_wait(yield[ec]);
    timer.cancel();
    if (socket.is_open()) socket.close(ec);
    m_running.store(false, std::memory_order_release);
  }

private:
  std::atomic_bool& m_running;
};

class ListenerTest : public BaseTest {
protected:
  sys::error_code ec;
  net::io_context ioc;
  Session::protocol_type::socket socket{ioc};
  Session::protocol_type::endpoint ep{ip::address_v4::loopback(), serverPort};
  std::atomic_bool running{false};
};

TEST_F(ListenerTest, Restart)
{
  running.store(false, std::memory_order_release);

  auto listener = makeListener<Session>(ep, running);
  ASSERT_FALSE(running.load(std::memory_order_acquire));

  // First start
  ASSERT_TRUE(startListener(listener));
  ASSERT_EQ(listener->get_state(), serveza::lifecycle_state::started);
  socket.connect(ep, ec);
  ASSERT_FALSE(ec);

  for (auto i = 0; i < 500 && !running.load(std::memory_order_acquire); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  ASSERT_TRUE(running.load(std::memory_order_acquire));

  // Stop
  ASSERT_TRUE(stopListener(listener));
  ASSERT_EQ(listener->get_state(), serveza::lifecycle_state::stopped);
  for (auto i = 0; i < 500 && running.load(std::memory_order_acquire); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  ASSERT_FALSE(running.load(std::memory_order_acquire));

  // Second start
  ASSERT_TRUE(startListener(listener));
  ASSERT_EQ(listener->get_state(), serveza::lifecycle_state::started);
  socket = Session::protocol_type::socket{ioc};
  socket.connect(ep, ec);
  ASSERT_FALSE(ec);

  for (auto i = 0; i < 500 && !running.load(std::memory_order_acquire); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  ASSERT_TRUE(running.load(std::memory_order_acquire));
}

TEST_F(ListenerTest, SeveralStopCalling)
{
  running.store(false, std::memory_order_release);

  auto listener = makeListener<Session>(ep, running);
  ASSERT_FALSE(running.load(std::memory_order_acquire));

  ASSERT_TRUE(startListener(listener));
  socket.connect(ep, ec);
  for (auto i = 0; i < 500 && !running.load(std::memory_order_acquire); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  ASSERT_TRUE(running.load(std::memory_order_acquire));

  for (auto i = 0; i < 500; ++i)
    listener->stop();

  for (auto i = 0; i < 500 && running.load(std::memory_order_acquire); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  ASSERT_FALSE(running.load(std::memory_order_acquire));
}

} // namespace
