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

#include <filesystem>
#include <iostream>

#include <boost/asio.hpp>
#include <serveza/serveza.hpp>

// To connect to the server use:
// socat STDIO UNIX-CONNECT:/tmp/local_echo

namespace fs = std::filesystem;
namespace net = boost::asio;
namespace local = net::local;
namespace sys = boost::system;

namespace {

void logError(std::exception_ptr err, std::string_view where)
{
  try {
    if (err) std::rethrow_exception(err);
  } catch (const sys::system_error& e) {
    std::cout << "Error in the " << where << ": " << e.code().message() << std::endl;
  } catch (const std::exception& e) {
    std::cout << "Error in the " << where << ": " << e.what() << std::endl;
  }
}

class LocalEcho final : public serveza::session<local::stream_protocol> {
public:
  explicit LocalEcho(std::string greetings)
    : m_greetings{std::move(greetings)}
  {}

  void operator()(protocol_type::socket socket, net::yield_context yield) override
  {
    std::cout << "Client connected" << std::endl;

    try {
      net::streambuf buffer;
      std::ostream os{&buffer};
      os << m_greetings << '\n';
      net::async_write(socket, buffer, yield);

      while (yield.cancelled() == net::cancellation_type::none) {
        net::async_read_until(socket, buffer, "\n", yield);
        net::async_write(socket, buffer, yield);
        buffer.consume(buffer.size());
      }
    } catch (...) {
      close(socket);
      throw;
    }

    close(socket);
  }

private:
  template<typename Socket>
  static void close(Socket& socket)
  {
    if (!socket.is_open()) return;
    sys::error_code ec;
    socket.close(ec);
  }

  std::string m_greetings;
};

fs::path socketPath()
{
  auto path = fs::temp_directory_path() / "local_echo";
  std::error_code ec;
  fs::remove(path, ec);
  return path;
}

} // namespace

int main()
{
  auto path = socketPath();
  std::cout << "Socket path: " << path << std::endl;

  serveza::server server{std::thread::hardware_concurrency()};

  local::stream_protocol::endpoint ep{path.generic_string()};
  auto listener = server.make_listener<LocalEcho>(ep, "Hello! I'm an echo server.");
  listener->on_state_changed([](serveza::lifecycle_state state) {
    using state_type = serveza::lifecycle_state;
    if (state == state_type::started || state == state_type::stopped)
      std::cout << "Listener " << (state == state_type::started ? "started" : "stopped") << std::endl;
  });
  listener->on_error([](std::exception_ptr err, std::string_view where) { logError(err, where); });
  listener->start();

  net::signal_set signals{server.get_executor(), SIGINT, SIGTERM};
  signals.async_wait([&server, listener](const sys::error_code& ec, int) {
    if (ec) return;
    server.stop();
  });

  server.on_state_changed([](serveza::lifecycle_state state) {
    using state_type = serveza::lifecycle_state;
    if (state == state_type::started || state == state_type::stopped)
      std::cout << "Server " << (state == state_type::started ? "started" : "stopped") << std::endl;
  });

  try {
    server.start();
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    logError(std::current_exception(), "server::start()");
    return EXIT_FAILURE;
  }
}
