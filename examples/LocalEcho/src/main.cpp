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
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <utility>

#include <boost/asio.hpp>

#include <serveza/serveza.h>

namespace fs = std::filesystem;
namespace net = boost::asio;
namespace local = net::local;
namespace sys = boost::system;
using datagram = local::datagram_protocol;
using stream = local::stream_protocol;

namespace {

constexpr std::size_t workerCount = 4;
constexpr std::string_view greeting = "Hello! I'm a local echo server.\n";
std::mutex logMutex;

template<typename Handler>
class StreamEchoOperation final : public std::enable_shared_from_this<StreamEchoOperation<Handler>> {
public:
  StreamEchoOperation(stream::socket& socket, serveza::connection_info info, Handler handler)
    : m_socket{socket}
    , m_info{std::move(info)}
    , m_handler{std::move(handler)}
  {}

  void run()
  {
    {
      std::lock_guard<std::mutex> lock{logMutex};
      std::cout << "Local stream client " << m_info.id << " connected" << std::endl;
    }
    std::shared_ptr<StreamEchoOperation> self = this->shared_from_this();
    net::async_write(m_socket, net::buffer(greeting), [self](sys::error_code ec, std::size_t) {
      if (ec)
        self->finish(ec);
      else
        self->read();
    });
  }

private:
  void read()
  {
    std::shared_ptr<StreamEchoOperation> self = this->shared_from_this();
    m_socket.async_read_some(net::buffer(m_data), [self](sys::error_code ec, std::size_t size) {
      if (ec) return self->finish(ec);
      self->write(size);
    });
  }

  void write(std::size_t size)
  {
    std::shared_ptr<StreamEchoOperation> self = this->shared_from_this();
    net::async_write(m_socket, net::buffer(m_data.data(), size), [self](sys::error_code ec, std::size_t) {
      if (ec) return self->finish(ec);
      self->read();
    });
  }

  void finish(const sys::error_code& ec)
  {
    {
      std::lock_guard<std::mutex> lock{logMutex};
      std::cout << "Local stream client " << m_info.id << " disconnected";
      if (ec != net::error::eof && ec != net::error::operation_aborted) std::cout << ": " << ec.message();
      std::cout << std::endl;
    }
    Handler handler = std::move(m_handler);
    handler(sys::error_code{});
  }

  stream::socket& m_socket;
  serveza::connection_info m_info;
  Handler m_handler;
  std::array<char, 4096> m_data{};
};

struct StreamEcho {
  template<typename Handler>
  void operator()(serveza::session_context<stream>& ctx, Handler handler) const
  {
    using Operation = StreamEchoOperation<std::decay_t<Handler>>;
    std::shared_ptr<Operation> operation = std::make_shared<Operation>(ctx.socket(), ctx.info(), std::move(handler));
    operation->run();
  }
};

template<typename Handler>
class DatagramEchoOperation final : public std::enable_shared_from_this<DatagramEchoOperation<Handler>> {
public:
  DatagramEchoOperation(datagram::socket& socket, Handler handler)
    : m_socket{socket}
    , m_handler{std::move(handler)}
  {}

  void run()
  {
    receive();
  }

private:
  void receive()
  {
    std::shared_ptr<DatagramEchoOperation> self = this->shared_from_this();
    m_socket.async_receive_from(net::buffer(m_data), m_remote, [self](sys::error_code ec, std::size_t size) {
      if (ec == net::error::operation_aborted) return self->finish();
      if (ec) {
        std::lock_guard<std::mutex> lock{logMutex};
        std::cerr << "Local datagram receive failed: " << ec.message() << std::endl;
        self->receive();
        return;
      }
      self->send(size);
    });
  }

  void send(std::size_t size)
  {
    std::shared_ptr<DatagramEchoOperation> self = this->shared_from_this();
    m_socket.async_send_to(net::buffer(m_data.data(), size), m_remote, [self](sys::error_code ec, std::size_t) {
      if (ec == net::error::operation_aborted) return self->finish();
      if (ec) {
        std::lock_guard<std::mutex> lock{logMutex};
        std::cerr << "Local datagram send failed: " << ec.message() << std::endl;
      }
      self->receive();
    });
  }

  void finish()
  {
    Handler handler = std::move(m_handler);
    handler(sys::error_code{});
  }

  datagram::socket& m_socket;
  Handler m_handler;
  std::array<char, 4096> m_data{};
  datagram::endpoint m_remote;
};

struct DatagramEcho {
  template<typename Handler>
  void operator()(serveza::bound_socket_context<datagram>& ctx, Handler handler) const
  {
    using Operation = DatagramEchoOperation<std::decay_t<Handler>>;
    std::shared_ptr<Operation> operation = std::make_shared<Operation>(ctx.socket(), std::move(handler));
    operation->run();
  }
};

} // namespace

int main(int argc, char* argv[])
{
  const fs::path streamPath = argc > 1 ? fs::path{argv[1]} : fs::temp_directory_path() / "serveza-stream.sock";
  const fs::path datagramPath = argc > 2 ? fs::path{argv[2]} : fs::temp_directory_path() / "serveza-datagram.sock";
  std::error_code fsEc;
  fs::remove(streamPath, fsEc);
  fs::remove(datagramPath, fsEc);

  net::thread_pool pool{workerCount};
  serveza::server server{pool.get_executor()};
  std::shared_ptr<serveza::listener> streamListener = server.listen<stream>(
      stream::endpoint{streamPath.string()}, [] { return serveza::callback_session{StreamEcho{}}; });
  std::shared_ptr<serveza::listener> datagramListener = server.bind<datagram>(
      datagram::endpoint{datagramPath.string()}, [] { return serveza::callback_session{DatagramEcho{}}; });

  net::strand<net::thread_pool::executor_type> control = net::make_strand(pool);
  net::signal_set signals{control, SIGINT, SIGTERM};
  signals.async_wait([&server](sys::error_code ec, int) {
    if (ec) return;
    std::lock_guard<std::mutex> lock{logMutex};
    std::cout << "Stopping Serveza local echo example." << std::endl;
    server.request_stop();
  });

  int result = EXIT_SUCCESS;
  server.async_wait(net::bind_executor(control, [&](sys::error_code ec) {
    if (ec) {
      std::lock_guard<std::mutex> lock{logMutex};
      std::cerr << "Local listeners stopped with error: " << ec.message() << std::endl;
      result = EXIT_FAILURE;
    }
    signals.cancel();
  }));
  server.async_start(net::bind_executor(control, [&](sys::error_code ec) {
    if (ec) {
      {
        std::lock_guard<std::mutex> lock{logMutex};
        std::cerr << "Local listener start failed: " << ec.message() << std::endl;
      }
      result = EXIT_FAILURE;
      server.request_stop();
      return;
    }
    std::lock_guard<std::mutex> lock{logMutex};
    std::cout << "Local stream endpoint: " << streamListener->endpoint() << std::endl;
    std::cout << "Local datagram endpoint: " << datagramListener->endpoint() << std::endl;
    std::cout << "Try stream: socat STDIO UNIX-CONNECT:" << streamPath << std::endl;
    std::cout << "Try datagram: echo hello | socat - UNIX-SENDTO:" << datagramPath << std::endl;
    std::cout << "I/O worker threads: " << workerCount << std::endl;
  }));

  pool.join();
  fs::remove(streamPath, fsEc);
  fs::remove(datagramPath, fsEc);
  return result;
}
