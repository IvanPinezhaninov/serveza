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

#ifndef SERVEZA_SERVER_H
#define SERVEZA_SERVER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/strand.hpp>

#include <serveza/detail/basic_socket_listener.h>
#include <serveza/export.h>
#include <serveza/listener.h>
#include <serveza/observer.h>
#include <serveza/server_status.h>

namespace serveza {

namespace detail {
class server_implementation;
}

/** @brief Listener registry running on a caller-owned executor. */
class SERVEZA_API server {
public:
  /**
   * @brief Creates a listener registry while preserving the caller-owned executor's concrete type.
   *
   * Each listener and accepted connection receives a distinct strand over this executor. The strand is type-erased
   * only after construction so composed operations can retain the executor's allocation-free property adaptations.
   */
  template<typename Executor, std::enable_if_t<!std::is_same_v<std::decay_t<Executor>, boost::asio::any_io_executor> &&
                                                   std::is_constructible_v<boost::asio::any_io_executor, Executor>,
                                               int> = 0>
  explicit server(Executor executor)
    : server{boost::asio::any_io_executor{executor},
             [executor] { return boost::asio::any_io_executor{boost::asio::strand<Executor>{executor}}; }}
  {}

  /** @brief Creates a listener registry from an already type-erased caller-owned executor. */
  explicit server(boost::asio::any_io_executor executor);

  /**
   * @brief Requests shutdown of all registered listeners and releases the registry.
   *
   * Destruction does not run or wait for the caller-owned executor. The executor
   * must remain valid and be allowed to drain the posted shutdown operations.
   */
  ~server();

  server(const server&) = delete;
  server& operator=(const server&) = delete;
  server(server&&) = delete;
  server& operator=(server&&) = delete;

  /**
   * @brief Registers an observer shared by all current and future listeners.
   *
   * The returned move-only subscription owns the registration. Destroying or
   * resetting it unregisters the observer, including from inside a callback.
   * A null observer is rejected with `std::invalid_argument`.
   */
  [[nodiscard]] observer_subscription observe(std::shared_ptr<observer> value);

  /**
   * @brief Creates a listener whose factory constructs one session per accepted connection.
   *
   * The factory is invoked without arguments on the serialized listener executor and must
   * return a session object by value. The factory itself is retained for the listener's
   * lifetime and may be move-only.
   */
  template<typename Protocol, typename SessionFactory>
  std::shared_ptr<listener> listen(typename Protocol::endpoint endpoint, SessionFactory&& factory,
                                   listener_options options = {})
  {
    using factory_type = std::decay_t<SessionFactory>;
    static_assert(detail::has_acceptor<Protocol>::value,
                  "server::listen requires a connection-oriented protocol with an acceptor");
    static_assert(std::is_invocable_v<factory_type&>, "The session factory must be invocable without arguments");
    static_assert(!std::is_reference_v<std::invoke_result_t<factory_type&>>,
                  "The session factory must return a session by value");
    using listener_type = basic_acceptor_listener<Protocol, factory_type>;
    std::shared_ptr<listener_type> value =
        std::make_shared<listener_type>(strand_factory(), next_listener_id(), observer_registry(), std::move(endpoint),
                                        std::forward<SessionFactory>(factory), options);
    add_listener(value);
    return value;
  }

  /**
   * @brief Creates a listener running one session over one bound connectionless socket.
   *
   * A fresh session is constructed for each listener generation. The bound
   * socket is shared by all datagrams handled during that generation.
   */
  template<typename Protocol, typename SessionFactory>
  std::shared_ptr<listener> bind(typename Protocol::endpoint endpoint, SessionFactory&& factory,
                                 listener_options options = {})
  {
    using factory_type = std::decay_t<SessionFactory>;
    static_assert(!detail::has_acceptor<Protocol>::value,
                  "server::bind requires a connectionless protocol without an acceptor");
    static_assert(std::is_invocable_v<factory_type&>, "The session factory must be invocable without arguments");
    static_assert(!std::is_reference_v<std::invoke_result_t<factory_type&>>,
                  "The session factory must return a session by value");
    using listener_type = detail::basic_socket_listener<Protocol, factory_type>;
    std::shared_ptr<listener_type> value =
        std::make_shared<listener_type>(strand_factory(), next_listener_id(), observer_registry(), std::move(endpoint),
                                        std::forward<SessionFactory>(factory), options);
    add_listener(value);
    return value;
  }

  /**
   * @brief Starts the current listener snapshot as one coordinated operation.
   *
   * The completion signature is `void(boost::system::error_code)`. If one
   * listener fails to start, stop is requested for every listener in the
   * snapshot and the first observed error is reported. Cancellation is sent
   * to every pending listener start. A concurrent `request_stop()` takes
   * precedence, drains every submitted child start and completes this operation
   * with `boost::asio::error::operation_aborted`. After successful initiation,
   * completion is scheduled exactly once and never inline. Allocation or
   * initial executor submission may throw before initiation succeeds.
   */
  template<typename CompletionToken>
  auto async_start(CompletionToken&& token)
  {
    return boost::asio::async_initiate<CompletionToken, void(boost::system::error_code)>(
        [this](auto handler) { do_async_start(listener::completion_handler{std::move(handler)}); }, token);
  }

  /**
   * @brief Waits until every listener in the current snapshot has stopped.
   *
   * The completion signature is `void(boost::system::error_code)`. Cancelling
   * this wait cancels only its child wait operations; it does not stop any
   * listener. Allocation or initial executor submission may throw before
   * initiation succeeds.
   */
  template<typename CompletionToken>
  auto async_wait(CompletionToken&& token)
  {
    return boost::asio::async_initiate<CompletionToken, void(boost::system::error_code)>(
        [this](auto handler) { do_async_wait(listener::completion_handler{std::move(handler)}); }, token);
  }

  /**
   * @brief Requests idempotent shutdown of every currently registered listener.
   *
   * The caller-owned executor must remain valid and able to accept work.
   */
  void request_stop() noexcept;
  /** @brief Returns the caller-owned executor supplied at construction. */
  [[nodiscard]] boost::asio::any_io_executor get_executor() const;
  /** @brief Returns an aggregate snapshot of the server and all listeners. */
  [[nodiscard]] server_status status() const;
  /** @brief Reports whether the server and all registered listeners are running. */
  [[nodiscard]] bool is_ready() const;
  /** @brief Returns the aggregate active-session count. */
  [[nodiscard]] std::size_t active_sessions() const;
  /** @brief Returns a stable snapshot of all registered listeners. */
  [[nodiscard]] std::vector<std::shared_ptr<listener>> listeners() const;

private:
  using strand_factory_type = std::function<boost::asio::any_io_executor()>;

  server(boost::asio::any_io_executor executor, strand_factory_type strand_factory);
  [[nodiscard]] strand_factory_type strand_factory() const;
  [[nodiscard]] std::uint64_t next_listener_id() noexcept;
  [[nodiscard]] std::shared_ptr<detail::observer_registry> observer_registry() const;
  void add_listener(std::shared_ptr<listener> value);
  void do_async_start(listener::completion_handler handler);
  void do_async_wait(listener::completion_handler handler);

  std::unique_ptr<detail::server_implementation> m_impl;
};

} // namespace serveza

#endif // SERVEZA_SERVER_H
