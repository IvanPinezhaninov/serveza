# Serveza 🍺

**The server you’d raise a glass to.**

[![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Build](https://img.shields.io/github/actions/workflow/status/IvanPinezhaninov/serveza/ci.yml?label=Build)](https://github.com/IvanPinezhaninov/serveza/actions/workflows/ci.yml)
[![Coverage](https://img.shields.io/endpoint?url=https%3A%2F%2Fivanpinezhaninov.github.io%2Fserveza%2Fcoverage.json)](https://ivanpinezhaninov.github.io/serveza/)
[![Raw coverage](https://img.shields.io/endpoint?url=https%3A%2F%2Fivanpinezhaninov.github.io%2Fserveza%2Fraw-coverage.json)](https://ivanpinezhaninov.github.io/serveza/raw/)

Serveza is a small C++17 server library built on Boost.Asio. It supports TCP,
UDP and Unix domain sockets. HTTP, TLS and other protocols live in session code.

Sessions can be written with callbacks, `boost::asio::yield_context`, or C++20
coroutines.

## Yield echo server

This TCP echo server uses `yield_context`. It greets each client and sends back
everything it receives. A single `io_context` keeps the example short. Use a
`thread_pool` executor when the server needs several CPU cores.

```cpp
#include <array>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include <boost/asio.hpp>

#include <serveza/serveza.h>
#include <serveza/yield_session.h>

namespace net = boost::asio;
namespace sys = boost::system;
using tcp = net::ip::tcp;

namespace {

constexpr unsigned short port = 54321;
constexpr std::string_view greeting = "Hello! I'm an echo server.\n";

class Echo final {
public:
  void operator()(serveza::session_context<tcp>& ctx, net::yield_context yield) const
  {
    std::array<char, 4096> buffer{};
    sys::error_code ec;

    net::async_write(ctx.socket(), net::buffer(greeting.data(), greeting.size()), yield[ec]);
    while (!ec) {
      const std::size_t size = ctx.socket().async_read_some(net::buffer(buffer), yield[ec]);
      if (!ec) net::async_write(ctx.socket(), net::buffer(buffer.data(), size), yield[ec]);
    }
  }
};

} // namespace

int main()
{
  net::io_context io;
  serveza::server server{io.get_executor()};
  const tcp::endpoint endpoint{net::ip::address_v4::loopback(), port};
  server.listen<tcp>(endpoint, [] {
    return serveza::yield_session{Echo{}};
  });

  net::signal_set signals{io, SIGINT, SIGTERM};
  signals.async_wait([&server](sys::error_code ec, int) {
    if (!ec) server.request_stop();
  });

  int result = EXIT_SUCCESS;
  net::spawn(
      io,
      [&](net::yield_context yield) {
        sys::error_code ec;
        server.async_start(yield[ec]);
        if (ec) {
          std::cerr << "Start failed: " << ec.message() << std::endl;
          result = EXIT_FAILURE;
          signals.cancel();
          return;
        }

        std::cout << "Listening on 127.0.0.1:" << port << std::endl;
        server.async_wait(yield[ec]);
        if (ec) {
          std::cerr << "Server failed: " << ec.message() << std::endl;
          result = EXIT_FAILURE;
        }
        signals.cancel();
      },
      net::detached);

  io.run();
  return result;
}
```

Applications using yield sessions include `<serveza/yield_session.h>` and link
with `serveza::serveza` and `Boost::coroutine`. Callback sessions do not include
or link Boost.Coroutine. For C++20 coroutines, use `serveza::awaitable_session`. If Serveza downloads Boost in a `FetchContent`
project, set `SERVEZA_EXTRA_BOOST_COMPONENTS=coroutine` before adding Serveza.
More complete programs live in [examples](examples/).

## Build

From a source checkout:

```sh
cmake -S . -B build -DSERVEZA_BUILD_EXAMPLES=ON -DSERVEZA_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Add `-DSERVEZA_BUILD_CPP20=ON` for coroutine examples and tests. Add
`-DSERVEZA_BUILD_DOCS=ON` to build the Doxygen reference.

Serveza requires CMake 3.28 and Boost 1.83 or newer. If the parent project
already provides Boost, GoogleTest or OpenSSL targets, Serveza uses them.
Otherwise it downloads pinned copies by default. Set
`SERVEZA_USE_BUNDLED_BOOST`, `SERVEZA_USE_BUNDLED_GTEST`, or
`SERVEZA_USE_BUNDLED_OPENSSL` to `OFF` to use installed packages instead.
GoogleTest is only needed for tests. OpenSSL is only needed for TLS.

Whether Serveza is installed or added with `FetchContent`, link the same target:

```cmake
target_link_libraries(MyServer PRIVATE serveza::serveza)
```

## License

Serveza is distributed under the [MIT License](LICENSE).

## Author

[Ivan Pinezhaninov](mailto:ivan.pinezhaninov@gmail.com)
