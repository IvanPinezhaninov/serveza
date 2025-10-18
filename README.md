# Serveza 🍺
**The server you’d raise a glass to.**

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17.html)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](http://opensource.org/licenses/MIT)
[![Build](https://img.shields.io/github/actions/workflow/status/IvanPinezhaninov/serveza/build_and_test.yml)](https://github.com/IvanPinezhaninov/serveza/actions)

---

## Overview

**Serveza** is a lightweight asynchronous server framework built on Boost.Asio and written in modern C++.
It focuses on predictable behavior, minimal overhead, and code clarity - making it suitable for both embedded and high-performance network services.

---

### Usage

1. **Define Your Session Handler**

```cpp
#include <boost/asio.hpp>
#include <serveza/serveza.hpp>

namespace net = boost::asio;
namespace sys = boost::system;

class TcpEcho final : public serveza::session<net::ip::tcp> {
public:
  void operator()(net::any_io_executor ex, net::ip::tcp::socket socket,
                  net::yield_context yield) override
  {
    sys::error_code ec;
    net::streambuf buf;
    while (yield.cancelled() == net::cancellation_type::none) {
      std::size_t n = socket.async_read_some(buf.prepare(1024), yield[ec]);
      if (ec == net::error::operation_aborted) break;
      buf.commit(n);
      net::async_write(socket, buf.data(), yield[ec]);
      if (ec == net::error::operation_aborted) break;
      buf.consume(n);
    }
  }
};
```

2. **Set Up the Server**
```cpp
#include <boost/asio.hpp>
#include <serveza/serveza.hpp>

namespace net = boost::asio;

int main()
{
  serveza::server server;
  auto listener = server.make_listener<TcpEcho>({net::ip::tcp::v4(), 54321});
  listener->start();
  server.start();
}
```
---

## License

This code is distributed under the [MIT License](http://opensource.org/licenses/MIT)

## Author

[Ivan Pinezhaninov](mailto:ivan.pinezhaninov@gmail.com)
