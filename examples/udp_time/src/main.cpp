/******************************************************************************
**
** Copyright (C) 2025 Ivan Pinezhaninov <ivan.pinezhaninov@gmail.com>
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

#include <chrono>
#include <ctime>
#include <iostream>

#include <boost/asio.hpp>
#include <serveza/serveza.hpp>

// To connect to the server use:
// socat STDIO UDP4:localhost:54321
// and send any command

namespace net = boost::asio;
namespace ip = net::ip;
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

class UdpTime final : public serveza::session<ip::udp> {
public:
  void operator()(net::any_io_executor ex, protocol_type::socket socket, net::yield_context yield,
                  const sys::error_code& ec) noexcept override
  {
    auto strand = net::make_strand(socket.get_executor());
    std::size_t sigsKeySeq = 0;
    std::unordered_map<std::size_t, boost::asio::cancellation_signal> sigs;

    while (yield.cancelled() == net::cancellation_type::none) {
      protocol_type::endpoint remote;
      std::array<char, 1> buf;

      socket.async_receive_from(net::buffer(buf), remote, yield);
      if (ec) {
        if (ec == net::error::operation_aborted) break;
        std::cerr << "Failed to receive: " << ec.message() << std::endl;
        continue;
      }

      std::cout << "New client: " << remote << std::endl;
      auto [key, sig] = emplace_signal(sigsKeySeq, sigs);
      net::spawn(
          strand,
          [this, ex, session_ex = yield.get_executor(), &socket, remote, &sigs,
           key = key](net::yield_context yield) mutable {
            timeBroadcast(ex, socket, remote, yield);
            net::defer(session_ex, [&sigs, key] { sigs.erase(key); });
            auto slot = yield.get_cancellation_slot();
            slot.clear();
          },
          net::bind_cancellation_slot(sig.slot(), net::detached));
    }

    for (auto& [_, sig] : sigs)
      sig.emit(net::cancellation_type::all);

    close(socket);
  }

private:
  [[nodiscard]]
  std::pair<std::size_t, boost::asio::cancellation_signal&>
  emplace_signal(std::size_t& sigsKeySeq, std::unordered_map<std::size_t, boost::asio::cancellation_signal>& sigs)
  {
    while (true) {
      if (sigsKeySeq == std::numeric_limits<std::size_t>::max()) sigsKeySeq = 0;
      auto [it, ok] = sigs.try_emplace(++sigsKeySeq);
      if (ok) return {it->first, it->second};
    }
  }

  static void timeBroadcast(net::any_io_executor ex, protocol_type::socket& socket, protocol_type::endpoint remote,
                            net::yield_context yield)
  {
    sys::error_code ec;
    net::steady_timer timer{ex};

    while (yield.cancelled() == net::cancellation_type::none) {
      using clock = std::chrono::system_clock;
      const std::time_t now = clock::to_time_t(clock::now());

      std::tm tm{};
#if defined(_WIN32)
      if (::localtime_s(&tm, &now) != 0) {
        std::cerr << "Failed to get local time" << std::endl;
      } else
#else
      if (::localtime_r(&now, &tm) == nullptr) {
        std::cerr << "Failed to get local time" << std::endl;
      } else
#endif
      {
        char out[32];
        std::size_t n = std::strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S\n", &tm);
        if (n > 0) {
          socket.async_send_to(net::buffer(out, n), remote, yield[ec]);
          if (ec) {
            if (ec == net::error::operation_aborted) break;
            std::cerr << "Broadcast failed to " << remote << ": " << ec.message() << std::endl;
          }
        }
      }

      timer.expires_after(std::chrono::seconds{1});
      timer.async_wait(yield[ec]);
      if (ec) {
        if (ec == net::error::operation_aborted) break;
        std::cerr << "Timer wait failed for " << remote << ": " << ec.message() << std::endl;
      }
    }
  }

  template<typename Socket>
  static void close(Socket& socket)
  {
    if (!socket.is_open()) return;
    sys::error_code ec;
    socket.close(ec);
  }
};

} // namespace

int main()
{
  serveza::server server{std::thread::hardware_concurrency()};

  ip::udp::endpoint ep{ip::udp::v4(), 54321};
  auto listener = server.make_listener<UdpTime>(ep);
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
