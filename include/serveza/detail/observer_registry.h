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

#ifndef SERVEZA_DETAIL_OBSERVER_REGISTRY_H
#define SERVEZA_DETAIL_OBSERVER_REGISTRY_H

#include <cstdint>
#include <memory>

#include <serveza/export.h>
#include <serveza/observer.h>

namespace serveza::detail {

class observer_registry_implementation;

class SERVEZA_API observer_registry final {
public:
  observer_registry();
  ~observer_registry();

  observer_registry(const observer_registry&) = delete;
  observer_registry& operator=(const observer_registry&) = delete;
  observer_registry(observer_registry&&) = delete;
  observer_registry& operator=(observer_registry&&) = delete;

  [[nodiscard]] std::uint64_t add(std::shared_ptr<observer> value);
  void remove(std::uint64_t id) noexcept;
  void notify(const listener_event& event) noexcept;
  void notify(const server_event& event) noexcept;

private:
  std::unique_ptr<observer_registry_implementation> m_impl;
};

} // namespace serveza::detail

#endif // SERVEZA_DETAIL_OBSERVER_REGISTRY_H
