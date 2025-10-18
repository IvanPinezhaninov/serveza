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
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include <boost/asio.hpp>
#if defined(SERVEZA_TEST_HTTPS)
#include <boost/asio/ssl.hpp>
#endif
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#if defined(SERVEZA_TEST_HTTPS)
#include <boost/beast/ssl.hpp>
#endif

#include <gtest/gtest.h>

#include <serveza/serveza.h>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
#if defined(SERVEZA_TEST_HTTPS)
namespace ssl = net::ssl;
#endif
namespace sys = boost::system;
using tcp = net::ip::tcp;

namespace {

constexpr std::size_t headerLimit = 16 * 1024;
constexpr std::size_t bodyLimit = 1024 * 1024;

unsigned short listenerPort(const std::shared_ptr<serveza::listener>& listener)
{
  const std::string endpoint = listener->endpoint();
  return static_cast<unsigned short>(std::stoul(endpoint.substr(endpoint.rfind(':') + 1)));
}

struct PlainTransport {
  using stream_type = tcp::socket&;

  template<typename Handler>
  static void start(stream_type, Handler handler)
  {
    handler(sys::error_code{});
  }

  template<typename Handler>
  static void shutdown(stream_type, Handler handler)
  {
    handler(sys::error_code{});
  }

  static bool expected(const sys::error_code& ec)
  {
    return ec == http::error::end_of_stream || ec == net::error::eof || ec == net::error::connection_reset ||
           ec == net::error::operation_aborted;
  }
};

#if defined(SERVEZA_TEST_HTTPS)
struct TlsTransport {
  using stream_type = ssl::stream<tcp::socket&>;

  template<typename Handler>
  static void start(stream_type& stream, Handler handler)
  {
    stream.async_handshake(ssl::stream_base::server, std::move(handler));
  }

  template<typename Handler>
  static void shutdown(stream_type& stream, Handler handler)
  {
    stream.async_shutdown(std::move(handler));
  }

  static bool expected(const sys::error_code& ec)
  {
    return PlainTransport::expected(ec) || ec == ssl::error::stream_truncated;
  }
};
#endif

template<typename Transport, typename Handler>
class HttpEchoOperation final : public std::enable_shared_from_this<HttpEchoOperation<Transport, Handler>> {
public:
  using stream_type = typename Transport::stream_type;

  HttpEchoOperation(stream_type stream, Handler handler)
    : m_stream{std::forward<stream_type>(stream)}
    , m_handler{std::move(handler)}
  {}

  void start()
  {
    std::shared_ptr<HttpEchoOperation> self = this->shared_from_this();
    Transport::start(m_stream, [self](sys::error_code ec) {
      if (ec) {
        self->finish(ec);
        return;
      }
      self->read();
    });
  }

private:
  void read()
  {
    m_parser.emplace();
    m_parser->header_limit(headerLimit);
    m_parser->body_limit(bodyLimit);
    std::shared_ptr<HttpEchoOperation> self = this->shared_from_this();
    http::async_read(m_stream, m_buffer, *m_parser, [self](sys::error_code ec, std::size_t) {
      if (ec) {
        self->finish(ec);
        return;
      }
      self->m_request = self->m_parser->release();
      self->m_parser.reset();
      self->write();
    });
  }

  void write()
  {
    m_response.version(m_request.version());
    m_response.result(http::status::ok);
    m_response.set(http::field::server, "serveza-test");
    m_response.set(http::field::content_type, "text/plain");
    m_response.keep_alive(m_request.keep_alive());
    m_response.body() = m_request.body();
    m_response.prepare_payload();

    std::shared_ptr<HttpEchoOperation> self = this->shared_from_this();
    http::async_write(m_stream, m_response, [self](sys::error_code ec, std::size_t) {
      if (ec) {
        self->finish(ec);
        return;
      }
      if (!self->m_response.keep_alive()) {
        self->shutdown();
        return;
      }
      self->m_request = {};
      self->m_response = {};
      self->read();
    });
  }

  void shutdown()
  {
    std::shared_ptr<HttpEchoOperation> self = this->shared_from_this();
    Transport::shutdown(m_stream, [self](sys::error_code ec) { self->finish(ec); });
  }

  void finish(sys::error_code ec)
  {
    if (m_finished) return;
    m_finished = true;
    if (Transport::expected(ec)) ec.clear();
    Handler handler = std::move(m_handler);
    handler(ec);
  }

  stream_type m_stream;
  Handler m_handler;
  beast::flat_buffer m_buffer;
  std::optional<http::request_parser<http::string_body>> m_parser;
  http::request<http::string_body> m_request;
  http::response<http::string_body> m_response;
  bool m_finished{};
};

struct HttpEchoSession {
  template<typename Context, typename Handler>
  void operator()(Context& ctx, Handler handler) const
  {
    using Operation = HttpEchoOperation<PlainTransport, std::decay_t<Handler>>;
    std::make_shared<Operation>(ctx.socket(), std::move(handler))->start();
  }
};

#if defined(SERVEZA_TEST_HTTPS)
struct HttpsEchoSession {
  ssl::context& tlsCtx;

  template<typename Context, typename Handler>
  void operator()(Context& ctx, Handler handler) const
  {
    using Operation = HttpEchoOperation<TlsTransport, std::decay_t<Handler>>;
    typename TlsTransport::stream_type stream{ctx.socket(), tlsCtx};
    std::make_shared<Operation>(std::move(stream), std::move(handler))->start();
  }
};
#endif

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

TEST(HttpTests, EchoesTwoRequestsAcrossOneKeepAliveConnection)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{HttpEchoSession{}}; });
  tcp::socket client{io};
  beast::flat_buffer clientBuffer;
  http::request<http::string_body> first{http::verb::post, "/first", 11};
  first.keep_alive(true);
  first.body() = "first body";
  first.prepare_payload();
  http::request<http::string_body> second{http::verb::put, "/second", 11};
  second.keep_alive(false);
  second.body() = "second body";
  second.prepare_payload();
  http::response<http::string_body> firstResponse;
  http::response<http::string_body> secondResponse;
  bool completed = false;
  bool stopped = false;
  net::steady_timer deadline{io, std::chrono::seconds{3}};

  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
  });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client.async_connect({net::ip::address_v4::loopback(), listenerPort(listener)}, [&](sys::error_code connectEc) {
      ASSERT_FALSE(connectEc);
      http::async_write(client, first, [&](sys::error_code writeEc, std::size_t) {
        ASSERT_FALSE(writeEc);
        http::async_read(client, clientBuffer, firstResponse, [&](sys::error_code readEc, std::size_t) {
          ASSERT_FALSE(readEc);
          EXPECT_EQ(firstResponse.result(), http::status::ok);
          EXPECT_EQ(firstResponse.body(), first.body());
          EXPECT_TRUE(firstResponse.keep_alive());
          http::async_write(client, second, [&](sys::error_code secondWriteEc, std::size_t) {
            ASSERT_FALSE(secondWriteEc);
            http::async_read(client, clientBuffer, secondResponse, [&](sys::error_code secondReadEc, std::size_t) {
              EXPECT_FALSE(secondReadEc);
              EXPECT_EQ(secondResponse.result(), http::status::ok);
              EXPECT_EQ(secondResponse.body(), second.body());
              EXPECT_FALSE(secondResponse.keep_alive());
              completed = !secondReadEc;
              deadline.cancel();
              server.request_stop();
            });
          });
        });
      });
    });
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    ADD_FAILURE() << "HTTP keep-alive exchange timed out";
    server.request_stop();
  });

  io.run();

  EXPECT_TRUE(completed);
  EXPECT_TRUE(stopped);
}

TEST(HttpTests, MalformedRequestIsReportedWithoutFailingListener)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{HttpEchoSession{}}; });
  net::steady_timer deadline{io, std::chrono::seconds{3}};
  std::shared_ptr<SessionErrorObserver> observer = std::make_shared<SessionErrorObserver>(server, deadline);
  [[maybe_unused]] serveza::observer_subscription observerSubscription = server.observe(observer);
  tcp::socket client{io};
  const std::string malformed{"NOT HTTP\r\n\r\n"};
  bool stopped = false;

  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
  });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client.async_connect({net::ip::address_v4::loopback(), listenerPort(listener)}, [&](sys::error_code connectEc) {
      ASSERT_FALSE(connectEc);
      net::async_write(client, net::buffer(malformed),
                       [](sys::error_code writeEc, std::size_t) { EXPECT_FALSE(writeEc); });
    });
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    ADD_FAILURE() << "malformed HTTP request was not reported";
    server.request_stop();
  });

  io.run();

  EXPECT_TRUE(observer->observed);
  EXPECT_TRUE(observer->error);
  EXPECT_TRUE(stopped);
}

TEST(HttpTests, OversizedBodyIsRejectedBeforeItIsRead)
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>(
      {net::ip::address_v4::loopback(), 0}, [] { return serveza::callback_session{HttpEchoSession{}}; });
  net::steady_timer deadline{io, std::chrono::seconds{3}};
  std::shared_ptr<SessionErrorObserver> observer = std::make_shared<SessionErrorObserver>(server, deadline);
  [[maybe_unused]] serveza::observer_subscription observerSubscription = server.observe(observer);
  tcp::socket client{io};
  const std::string oversizedHeader{
      "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1048577\r\nConnection: close\r\n\r\n"};
  bool stopped = false;

  server.async_wait([&](sys::error_code ec) {
    EXPECT_FALSE(ec);
    stopped = !ec;
  });
  server.async_start([&](sys::error_code ec) {
    ASSERT_FALSE(ec);
    client.async_connect({net::ip::address_v4::loopback(), listenerPort(listener)}, [&](sys::error_code connectEc) {
      ASSERT_FALSE(connectEc);
      net::async_write(client, net::buffer(oversizedHeader),
                       [](sys::error_code writeEc, std::size_t) { EXPECT_FALSE(writeEc); });
    });
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    ADD_FAILURE() << "oversized HTTP body was not rejected from its header";
    server.request_stop();
  });

  io.run();

  EXPECT_TRUE(observer->observed);
  EXPECT_EQ(observer->error, http::error::body_limit);
  EXPECT_TRUE(stopped);
}

#if defined(SERVEZA_TEST_HTTPS)
void configureServerContext(ssl::context& ctx)
{
  ctx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3);
  ctx.use_certificate_chain_file(SERVEZA_TEST_TLS_CERTIFICATE_PATH);
  ctx.use_private_key_file(SERVEZA_TEST_TLS_PRIVATE_KEY_PATH, ssl::context::pem);
}

TEST(HttpsTests, HandshakeRequestResponseAndCloseNotifySucceed)
{
  net::io_context io;
  ssl::context serverTls{ssl::context::tls_server};
  configureServerContext(serverTls);
  ssl::context clientTls{ssl::context::tls_client};
  clientTls.set_verify_mode(ssl::verify_none);
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>({net::ip::address_v4::loopback(), 0}, [&serverTls] {
    return serveza::callback_session{HttpsEchoSession{serverTls}};
  });
  ssl::stream<tcp::socket> client{io, clientTls};
  beast::flat_buffer clientBuffer;
  http::request<http::string_body> request{http::verb::post, "/secure", 11};
  request.keep_alive(false);
  request.body() = "encrypted body";
  request.prepare_payload();
  http::response<http::string_body> response;
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
            http::async_write(client, request, [&](sys::error_code writeEc, std::size_t) {
              ASSERT_FALSE(writeEc);
              http::async_read(client, clientBuffer, response, [&](sys::error_code readEc, std::size_t) {
                ASSERT_FALSE(readEc);
                EXPECT_EQ(response.result(), http::status::ok);
                EXPECT_EQ(response.body(), request.body());
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
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    ADD_FAILURE() << "HTTPS exchange timed out";
    server.request_stop();
  });

  io.run();

  EXPECT_TRUE(completed);
  EXPECT_TRUE(stopped);
}

TEST(HttpsTests, MalformedEncryptedRequestIsReported)
{
  net::io_context io;
  ssl::context serverTls{ssl::context::tls_server};
  configureServerContext(serverTls);
  ssl::context clientTls{ssl::context::tls_client};
  clientTls.set_verify_mode(ssl::verify_none);
  serveza::server server{io.get_executor()};
  std::shared_ptr<serveza::listener> listener = server.listen<tcp>({net::ip::address_v4::loopback(), 0}, [&serverTls] {
    return serveza::callback_session{HttpsEchoSession{serverTls}};
  });
  net::steady_timer deadline{io, std::chrono::seconds{3}};
  std::shared_ptr<SessionErrorObserver> observer = std::make_shared<SessionErrorObserver>(server, deadline);
  [[maybe_unused]] serveza::observer_subscription observerSubscription = server.observe(observer);
  ssl::stream<tcp::socket> client{io, clientTls};
  const std::string malformed{"NOT HTTP\r\n\r\n"};
  bool stopped = false;

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
            net::async_write(client, net::buffer(malformed),
                             [](sys::error_code writeEc, std::size_t) { EXPECT_FALSE(writeEc); });
          });
        });
  });
  deadline.async_wait([&](sys::error_code ec) {
    if (ec) return;
    ADD_FAILURE() << "malformed HTTPS request was not reported";
    server.request_stop();
  });

  io.run();

  EXPECT_TRUE(observer->observed);
  EXPECT_TRUE(observer->error);
  EXPECT_TRUE(stopped);
}
#endif

} // namespace
