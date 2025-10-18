/******************************************************************************
**
** Copyright (C) 2026 Ivan Pinezhaninov <ivan.pinezhaninov@gmail.com>
**
** This file is part of the serveza - which can be found at
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

#ifndef BASETEST_H
#define BASETEST_H

#include <thread>

#include <boost/asio.hpp>
#include <chrono>
#include <gtest/gtest.h>
#include <serveza/serveza.hpp>

namespace net = boost::asio;
namespace sys = boost::system;

class BaseTest : public testing::Test {
protected:
  BaseTest()
  {
    m_server.on_state_changed([this](serveza::lifecycle_state s) {
      if (s == serveza::lifecycle_state::started)
        m_cv.notify_one();
      else if (s == serveza::lifecycle_state::stopped)
        m_cv.notify_all();
    });
  }

  void SetUp() override
  {
    startServer();
  }

  void TearDown() override
  {
    stopServer();
  }

  template<typename Session, typename... Args>
  std::shared_ptr<serveza::listener> makeListener(const typename Session::protocol_type::endpoint& ep, Args&&... args)
  {
    return m_server.make_listener<Session>(ep, std::forward<Args>(args)...);
  }

  bool startListener(const std::shared_ptr<serveza::listener>& listener)
  {
    listener->start();
    return waitForState(listener, serveza::lifecycle_state::started);
  }

  bool stopListener(const std::shared_ptr<serveza::listener>& listener)
  {
    listener->stop();
    return waitForState(listener, serveza::lifecycle_state::stopped);
  }

  static bool waitForState(const std::shared_ptr<serveza::listener>& listener, serveza::lifecycle_state state,
                           std::chrono::milliseconds timeout = std::chrono::seconds(1))
  {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (listener->get_state() != state && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds{1});

    return listener->get_state() == state;
  }

  inline static constexpr auto serverPort = 54321;

private:
  void startServer()
  {
    m_thread = std::thread{[this] { m_server.start(); }};
    std::unique_lock lk{m_mutex};
    m_cv.wait(lk, [this] { return m_server.get_state() == serveza::lifecycle_state::started; });
  }

  void stopServer()
  {
    m_server.stop();
    std::unique_lock lk{m_mutex};
    m_cv.wait(lk, [this] { return m_server.get_state() == serveza::lifecycle_state::stopped; });
    if (m_thread.joinable()) m_thread.join();
  }

  net::io_context m_ioc;
  serveza::server m_server{m_ioc};
  std::mutex m_mutex;
  std::thread m_thread;
  std::condition_variable m_cv;
};

#endif // BASETEST_H
