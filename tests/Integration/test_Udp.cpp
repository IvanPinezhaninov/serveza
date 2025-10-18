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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio.hpp>

#include <gtest/gtest.h>

#include <serveza/serveza.h>

namespace net = boost::asio;
namespace sys = boost::system;
using udp = net::ip::udp;

namespace {

unsigned short listenerPort(const std::shared_ptr<serveza::listener>& listener)
{
  const std::string endpoint = listener->endpoint();
  return static_cast<unsigned short>(std::stoul(endpoint.substr(endpoint.rfind(':') + 1)));
}

struct UdpSessionState {
  std::size_t invocations{};
  std::uint64_t listenerId{};
  std::string localEndpoint;
};

template<typename Handler>
class UdpEchoOperation final : public std::enable_shared_from_this<UdpEchoOperation<Handler>> {
public:
  UdpEchoOperation(udp::socket& socket, Handler handler)
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
    std::shared_ptr<UdpEchoOperation> self = this->shared_from_this();
    m_socket.async_receive_from(net::buffer(m_data), m_remote, [self](sys::error_code ec, std::size_t size) {
      if (ec == net::error::operation_aborted) return self->finish({});
      if (ec) return self->finish(ec);
      self->send(size);
    });
  }

  void send(std::size_t size)
  {
    std::shared_ptr<UdpEchoOperation> self = this->shared_from_this();
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

  udp::socket& m_socket;
  Handler m_handler;
  std::array<char, 1024> m_data{};
  udp::endpoint m_remote;
};

struct UdpEchoSession {
  std::shared_ptr<UdpSessionState> state;

  template<typename Handler>
  void operator()(serveza::bound_socket_context<udp>& ctx, Handler handler) const
  {
    ++state->invocations;
    state->listenerId = ctx.listener_id();
    state->localEndpoint = ctx.local_endpoint();
    using Operation = UdpEchoOperation<std::decay_t<Handler>>;
    std::make_shared<Operation>(ctx.socket(), std::move(handler))->start();
  }
};

class UdpClientOperation final : public std::enable_shared_from_this<UdpClientOperation> {
public:
  using completion = std::function<void(bool)>;

  UdpClientOperation(net::io_context& io, udp::endpoint server, std::vector<std::string> requests, completion done)
    : m_socket{io, server.protocol()}
    , m_server{std::move(server)}
    , m_requests{std::move(requests)}
    , m_done{std::move(done)}
  {}

  void start()
  {
    send();
  }

private:
  void send()
  {
    std::shared_ptr<UdpClientOperation> self = shared_from_this();
    m_socket.async_send_to(net::buffer(m_requests[m_i]), m_server, [self](sys::error_code ec, std::size_t size) {
      if (ec || size != self->m_requests[self->m_i].size()) return self->finish(false);
      self->receive();
    });
  }

  void receive()
  {
    std::shared_ptr<UdpClientOperation> self = shared_from_this();
    m_socket.async_receive_from(net::buffer(m_response), m_sender, [self](sys::error_code ec, std::size_t size) {
      if (ec || std::string{self->m_response.data(), size} != self->m_requests[self->m_i]) {
        return self->finish(false);
      }
      ++self->m_i;
      if (self->m_i == self->m_requests.size()) return self->finish(true);
      self->send();
    });
  }

  void finish(bool success)
  {
    if (!m_done) return;
    completion done = std::move(m_done);
    done(success);
  }

  udp::socket m_socket;
  udp::endpoint m_server;
  udp::endpoint m_sender;
  std::vector<std::string> m_requests;
  completion m_done;
  std::array<char, 1024> m_response{};
  std::size_t m_i{};
};

struct ImmediateBoundSession {
  template<typename Context, typename Handler>
  void operator()(Context&, Handler handler) const
  {
    handler(sys::error_code{});
  }
};

struct FailingBoundSession {
  template<typename Context, typename Handler>
  void operator()(Context&, Handler handler) const
  {
    handler(make_error_code(sys::errc::io_error));
  }
};

struct HangingBoundState {
  std::function<void()> started;
  std::vector<serveza::listener::completion_handler> completions;
};

struct HangingBoundSession {
  std::shared_ptr<HangingBoundState> state;

  template<typename Context, typename Handler>
  void operator()(Context&, Handler handler) const
  {
    state->completions.emplace_back(std::move(handler));
    state->started();
  }
};

TEST(UdpTests, OneBoundSessionEchoesSeveralDatagrams)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<UdpSessionState> state = std::make_shared<UdpSessionState>();
  std::shared_ptr<serveza::listener> listener = server.bind<udp>(
      {net::ip::address_v4::loopback(), 0}, [state] { return serveza::callback_session{UdpEchoSession{state}}; });

  bool echoed = false;
  bool stopped = false;
  std::shared_ptr<UdpClientOperation> client;
  net::steady_timer deadline{io, std::chrono::seconds{2}};
  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
  });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client =
        std::make_shared<UdpClientOperation>(io, udp::endpoint{net::ip::address_v4::loopback(), listenerPort(listener)},
                                             std::vector<std::string>{"first", "", "third"}, [&](bool success) {
                                               echoed = success;
                                               deadline.cancel();
                                               server.request_stop();
                                             });
    client->start();
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    ADD_FAILURE() << "UDP echo timed out";
    server.request_stop();
  });

  io.run();

  EXPECT_TRUE(echoed);
  EXPECT_TRUE(stopped);
  EXPECT_EQ(state->invocations, 1u);
  EXPECT_EQ(state->listenerId, listener->id());
  EXPECT_EQ(state->localEndpoint, listener->endpoint());
  EXPECT_EQ(listener->active_sessions(), 0u);
}

TEST(UdpTests, EchoesIpv6LoopbackDatagram)
{
  net::io_context io;
  sys::error_code probeEc;
  udp::socket probe{io};
  probe.open(udp::v6(), probeEc);
  if (!probeEc) probe.bind({net::ip::address_v6::loopback(), 0}, probeEc);
  sys::error_code closeEc;
  probe.close(closeEc);
  if (probeEc) GTEST_SKIP() << "IPv6 UDP loopback is unavailable: " << probeEc.message();

  serveza::server server{io.get_executor()};
  std::shared_ptr<UdpSessionState> state = std::make_shared<UdpSessionState>();
  std::shared_ptr<serveza::listener> listener = server.bind<udp>(
      {net::ip::address_v6::loopback(), 0}, [state] { return serveza::callback_session{UdpEchoSession{state}}; });

  bool echoed = false;
  std::shared_ptr<UdpClientOperation> client;
  server.async_wait([](sys::error_code ec) { EXPECT_FALSE(ec); });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client =
        std::make_shared<UdpClientOperation>(io, udp::endpoint{net::ip::address_v6::loopback(), listenerPort(listener)},
                                             std::vector<std::string>{"ipv6"}, [&](bool success) {
                                               echoed = success;
                                               server.request_stop();
                                             });
    client->start();
  });
  io.run();

  EXPECT_TRUE(echoed);
}

TEST(UdpTests, DualStackIpv6SocketAcceptsIpv4Datagram)
{
  net::io_context io;
  sys::error_code probeEc;
  udp::socket probe{io};
  probe.open(udp::v6(), probeEc);
  if (!probeEc) probe.set_option(net::ip::v6_only{false}, probeEc);
  if (!probeEc) probe.bind({net::ip::address_v6::any(), 0}, probeEc);
  sys::error_code closeEc;
  probe.close(closeEc);
  if (probeEc) GTEST_SKIP() << "dual-stack UDP is unavailable: " << probeEc.message();

  serveza::server server{io.get_executor()};
  serveza::listener_options options;
  options.v6_only = false;
  std::shared_ptr<UdpSessionState> state = std::make_shared<UdpSessionState>();
  std::shared_ptr<serveza::listener> listener = server.bind<udp>(
      {net::ip::address_v6::any(), 0}, [state] { return serveza::callback_session{UdpEchoSession{state}}; }, options);

  bool echoed = false;
  std::shared_ptr<UdpClientOperation> client;
  server.async_wait([](sys::error_code ec) { EXPECT_FALSE(ec); });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client =
        std::make_shared<UdpClientOperation>(io, udp::endpoint{net::ip::address_v4::loopback(), listenerPort(listener)},
                                             std::vector<std::string>{"dual"}, [&](bool success) {
                                               echoed = success;
                                               server.request_stop();
                                             });
    client->start();
  });
  io.run();

  EXPECT_TRUE(echoed);
}

TEST(UdpTests, RestartCreatesANewBoundSession)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<std::size_t> created = std::make_shared<std::size_t>();
  std::shared_ptr<serveza::listener> listener = server.bind<udp>({net::ip::address_v4::loopback(), 0}, [created] {
    ++*created;
    return serveza::callback_session{ImmediateBoundSession{}};
  });

  std::size_t starts = 0;
  std::size_t stops = 0;
  listener->async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    ++stops;
    listener->async_start([&](sys::error_code restartEc) {
      EXPECT_FALSE(restartEc);
      ++starts;
      listener->async_wait([&](sys::error_code waitEc) {
        EXPECT_FALSE(waitEc);
        ++stops;
      });
    });
  });
  listener->async_start([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    ++starts;
  });

  io.run();

  EXPECT_EQ(starts, 2u);
  EXPECT_EQ(stops, 2u);
  EXPECT_EQ(*created, 2u);
  EXPECT_EQ(listener->status().generation, 2u);
  EXPECT_EQ(listener->state(), serveza::listener_state::stopped);
  EXPECT_EQ(listener->status().stop_reason, serveza::listener_stop_reason::completed);
}

TEST(UdpTests, SessionFailureIsReportedAsRuntimeFailure)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.bind<udp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{FailingBoundSession{}}; });

  sys::error_code waitEc;
  listener->async_wait([&](sys::error_code ec) { waitEc = ec; });
  listener->async_start([](sys::error_code ec) { EXPECT_FALSE(ec); });
  io.run();

  EXPECT_EQ(waitEc, make_error_code(sys::errc::io_error));
  EXPECT_EQ(listener->status().stop_reason, serveza::listener_stop_reason::runtime_failure);
}

TEST(UdpTests, BindFailureCompletesStartAndWaitWithError)
{
  net::io_context io;
  udp::socket blocker{io, {net::ip::address_v4::loopback(), 0}};
  const udp::endpoint occupied = blocker.local_endpoint();
  serveza::server server{io.get_executor()};
  serveza::listener_options options;
  options.reuse_address = false;
  std::shared_ptr<serveza::listener> listener =
      server.bind<udp>(occupied, [] { return serveza::callback_session{ImmediateBoundSession{}}; }, options);

  sys::error_code startEc;
  sys::error_code waitEc;
  listener->async_wait([&](sys::error_code ec) { waitEc = ec; });
  listener->async_start([&](sys::error_code ec) { startEc = ec; });

  io.run();

  EXPECT_TRUE(startEc);
  EXPECT_EQ(waitEc, startEc);
  EXPECT_EQ(listener->state(), serveza::listener_state::stopped);
  EXPECT_EQ(listener->status().last_error, startEc);
  EXPECT_EQ(listener->status().stop_reason, serveza::listener_stop_reason::startup_failure);
}

TEST(UdpTests, FactoryExceptionIsPreservedAsStartupFailure)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener =
      server.bind<udp>({net::ip::address_v4::loopback(), 0}, []() -> serveza::callback_session<ImmediateBoundSession> {
        throw std::runtime_error{"UDP factory failure"};
      });

  sys::error_code startEc;
  sys::error_code waitEc;
  listener->async_wait([&](sys::error_code ec) { waitEc = ec; });
  listener->async_start([&](sys::error_code ec) { startEc = ec; });

  io.run();

  EXPECT_TRUE(startEc);
  EXPECT_EQ(waitEc, startEc);
  const serveza::listener_status status = listener->status();
  EXPECT_EQ(status.state, serveza::listener_state::stopped);
  EXPECT_EQ(status.stop_reason, serveza::listener_stop_reason::startup_failure);
  ASSERT_TRUE(status.last_exception);
  try {
    std::rethrow_exception(status.last_exception);
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "UDP factory failure");
  } catch (...) {
    FAIL() << "UDP factory exception changed type";
  }
}

TEST(UdpTests, GraceDeadlineDrainsUncooperativeBoundSession)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<HangingBoundState> state = std::make_shared<HangingBoundState>();
  serveza::listener_options options;
  options.shutdown_grace_period = std::chrono::milliseconds{10};
  std::shared_ptr<serveza::listener> listener;
  bool joinedRunningGeneration = false;
  bool rejectedWhileStopping = false;
  state->started = [&] {
    listener->async_start([&](sys::error_code joinEc) {
      EXPECT_FALSE(joinEc);
      joinedRunningGeneration = !joinEc;
      listener->request_stop();
      listener->async_start([&](sys::error_code stoppingEc) {
        EXPECT_EQ(stoppingEc, net::error::operation_aborted);
        rejectedWhileStopping = true;
      });
    });
  };
  listener = server.bind<udp>(
      {net::ip::address_v4::loopback(), 0}, [state] { return serveza::callback_session{HangingBoundSession{state}}; },
      options);

  bool stopped = false;
  listener->async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
  });
  listener->async_start([](sys::error_code ec) { EXPECT_FALSE(ec); });

  io.run();

  EXPECT_TRUE(stopped);
  EXPECT_TRUE(joinedRunningGeneration);
  EXPECT_TRUE(rejectedWhileStopping);
  EXPECT_EQ(listener->active_sessions(), 0u);
  EXPECT_EQ(listener->status().last_error, make_error_code(sys::errc::timed_out));
  EXPECT_EQ(listener->status().stop_reason, serveza::listener_stop_reason::requested);
  ASSERT_EQ(state->completions.size(), 1u);
  std::move(state->completions.front())(sys::error_code{});
}

} // namespace
