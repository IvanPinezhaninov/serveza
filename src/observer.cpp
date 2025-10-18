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

#include <serveza/observer.h>

#include <utility>

#include <serveza/detail/observer_registry.h>

namespace serveza {

observer::~observer() = default;

void observer::on_server_event(const server_event&) {}

observer_subscription::observer_subscription(std::weak_ptr<detail::observer_registry> registry,
                                             std::uint64_t id) noexcept
  : m_registry{std::move(registry)}
  , m_id{id}
{}

observer_subscription::~observer_subscription()
{
  reset();
}

observer_subscription::observer_subscription(observer_subscription&& other) noexcept
  : m_registry{std::move(other.m_registry)}
  , m_id{std::exchange(other.m_id, 0)}
{}

observer_subscription& observer_subscription::operator=(observer_subscription&& other) noexcept
{
  if (this == &other) return *this;
  reset();
  m_registry = std::move(other.m_registry);
  m_id = std::exchange(other.m_id, 0);
  return *this;
}

void observer_subscription::reset() noexcept
{
  const std::uint64_t id = std::exchange(m_id, 0);
  if (id == 0) return;
  if (std::shared_ptr<detail::observer_registry> registry = m_registry.lock()) registry->remove(id);
  m_registry.reset();
}

observer_subscription::operator bool() const noexcept
{
  return m_id != 0;
}

} // namespace serveza
