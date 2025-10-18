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
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <gtest/gtest.h>

#include <serveza/serveza.h>

namespace net = boost::asio;
namespace ssl = net::ssl;
namespace sys = boost::system;
using tcp = net::ip::tcp;

namespace {

constexpr std::string_view greeting = "Hello! I'm a TLS echo server.\n";

unsigned short listenerPort(const std::shared_ptr<serveza::listener>& listener)
{
  const std::string endpoint = listener->endpoint();
  return static_cast<unsigned short>(std::stoul(endpoint.substr(endpoint.rfind(':') + 1)));
}

void configureServerContext(ssl::context& ctx)
{
  ctx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3);
  ctx.use_certificate_chain_file(SERVEZA_TEST_TLS_CERTIFICATE_PATH);
  ctx.use_private_key_file(SERVEZA_TEST_TLS_PRIVATE_KEY_PATH, ssl::context::pem);
}

template<typename Handler>
class TlsEchoOperation final : public std::enable_shared_from_this<TlsEchoOperation<Handler>> {
public:
  TlsEchoOperation(tcp::socket& socket, ssl::context& tlsCtx, Handler handler)
    : m_stream{socket, tlsCtx}
    , m_handler{std::move(handler)}
  {}

  void start()
  {
    std::shared_ptr<TlsEchoOperation> self = this->shared_from_this();
    m_stream.async_handshake(ssl::stream_base::server, [self](sys::error_code ec) {
      if (ec) return self->finish(ec);
      self->writeGreeting();
    });
  }

private:
  void writeGreeting()
  {
    std::shared_ptr<TlsEchoOperation> self = this->shared_from_this();
    net::async_write(m_stream, net::buffer(greeting.data(), greeting.size()), [self](sys::error_code ec, std::size_t) {
      if (ec) return self->finish(ec);
      self->read();
    });
  }

  void read()
  {
    std::shared_ptr<TlsEchoOperation> self = this->shared_from_this();
    m_stream.async_read_some(net::buffer(m_data), [self](sys::error_code ec, std::size_t size) {
      if (ec == net::error::eof) return self->shutdown();
      if (ec) return self->finish(ec);
      self->write(size);
    });
  }

  void write(std::size_t size)
  {
    std::shared_ptr<TlsEchoOperation> self = this->shared_from_this();
    net::async_write(m_stream, net::buffer(m_data.data(), size), [self](sys::error_code ec, std::size_t) {
      if (ec) return self->finish(ec);
      self->read();
    });
  }

  void shutdown()
  {
    std::shared_ptr<TlsEchoOperation> self = this->shared_from_this();
    m_stream.async_shutdown([self](sys::error_code ec) {
      if (ec == ssl::error::stream_truncated) ec.clear();
      self->finish(ec);
    });
  }

  void finish(sys::error_code ec)
  {
    if (m_finished) return;
    m_finished = true;
    if (ec == net::error::operation_aborted) ec.clear();
    Handler handler = std::move(m_handler);
    handler(ec);
  }

  ssl::stream<tcp::socket&> m_stream;
  Handler m_handler;
  std::array<char, 1024> m_data{};
  bool m_finished{};
};

struct TlsEchoSession {
  ssl::context& tlsCtx;

  template<typename Context, typename Handler>
  void operator()(Context& ctx, Handler handler) const
  {
    using Operation = TlsEchoOperation<std::decay_t<Handler>>;
    std::make_shared<Operation>(ctx.socket(), tlsCtx, std::move(handler))->start();
  }
};

class SessionErrorObserver final : public serveza::observer {
public:
  SessionErrorObserver(serveza::server& server, net::steady_timer& deadline)
    : m_server{server}
    , m_deadline{deadline}
  {}

  void on_event(const serveza::listener_event& event) override
  {
    if (event.type != serveza::listener_event_type::session_stopped || !event.error) return;
    observed = true;
    error = event.error;
    m_deadline.cancel();
    m_server.request_stop();
  }

  bool observed{};
  sys::error_code error;

private:
  serveza::server& m_server;
  net::steady_timer& m_deadline;
};

TEST(TlsTests, HandshakeGreetingEchoAndShutdownSucceed)
{
  net::io_context io;
  ssl::context serverTls{ssl::context::tls_server};
  configureServerContext(serverTls);
  ssl::context clientTls{ssl::context::tls_client};
  clientTls.set_verify_mode(ssl::verify_none);
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>({net::ip::address_v4::loopback(), 0}, [&serverTls] {
    return serveza::callback_session{TlsEchoSession{serverTls}};
  });

  ssl::stream<tcp::socket> client{io, clientTls};
  std::string receivedGreeting(greeting.size(), '\0');
  const std::string request{"encrypted echo"};
  std::string response(request.size(), '\0');
  bool completed = false;
  bool stopped = false;
  net::steady_timer deadline{io, std::chrono::seconds{3}};
  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
  });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client.lowest_layer().async_connect(
        {net::ip::address_v4::loopback(), listenerPort(listener)}, [&](sys::error_code connectEc) {
          ASSERT_FALSE(connectEc);
          client.async_handshake(ssl::stream_base::client, [&](sys::error_code handshakeEc) {
            ASSERT_FALSE(handshakeEc);
            net::async_read(client, net::buffer(receivedGreeting), [&](sys::error_code greetingEc, std::size_t) {
              ASSERT_FALSE(greetingEc);
              net::async_write(client, net::buffer(request), [&](sys::error_code writeEc, std::size_t) {
                ASSERT_FALSE(writeEc);
                net::async_read(client, net::buffer(response), [&](sys::error_code readEc, std::size_t) {
                  ASSERT_FALSE(readEc);
                  client.async_shutdown([&](sys::error_code shutdownEc) {
                    EXPECT_FALSE(shutdownEc);
                    completed = !shutdownEc;
                    deadline.cancel();
                    server.request_stop();
                  });
                });
              });
            });
          });
        });
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    ADD_FAILURE() << "TLS exchange timed out";
    server.request_stop();
  });

  io.run();

  EXPECT_TRUE(completed);
  EXPECT_TRUE(stopped);
  EXPECT_EQ(receivedGreeting, greeting);
  EXPECT_EQ(response, request);
}

TEST(TlsTests, HandshakeFailureIsObservedWithoutFailingListener)
{
  net::io_context io;
  ssl::context serverTls{ssl::context::tls_server};
  configureServerContext(serverTls);
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>({net::ip::address_v4::loopback(), 0}, [&serverTls] {
    return serveza::callback_session{TlsEchoSession{serverTls}};
  });
  net::steady_timer deadline{io, std::chrono::seconds{3}};
  std::shared_ptr<SessionErrorObserver> observer = std::make_shared<SessionErrorObserver>(server, deadline);
  [[maybe_unused]] serveza::observer_subscription observerSubscription = server.observe(observer);

  const std::string plainText{"GET / HTTP/1.0\r\n\r\n"};
  tcp::socket client{io};
  bool stopped = false;
  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
  });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client.async_connect({net::ip::address_v4::loopback(), listenerPort(listener)}, [&](sys::error_code connectEc) {
      ASSERT_FALSE(connectEc);
      net::async_write(client, net::buffer(plainText),
                       [](sys::error_code writeEc, std::size_t) { EXPECT_FALSE(writeEc); });
    });
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    ADD_FAILURE() << "TLS handshake failure was not observed";
    server.request_stop();
  });

  io.run();

  EXPECT_TRUE(observer->observed);
  EXPECT_TRUE(observer->error);
  EXPECT_TRUE(stopped);
}

} // namespace
