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

#include <serveza/detail/observer_registry.h>

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace serveza::detail {

class observer_registry_implementation final {
public:
  struct entry {
    std::uint64_t id{};
    std::shared_ptr<observer> value;
  };

  template<typename Function>
  void notify_each(Function function) noexcept
  {
    try {
      std::vector<std::shared_ptr<observer>> observers;
      {
        std::lock_guard lock{m_mutex};
        observers.reserve(m_values.size());
        for (const entry& item : m_values)
          observers.push_back(item.value);
      }
      if (observers.empty()) return;
      std::lock_guard callback_lock{m_callback_mutex};
      for (const std::shared_ptr<observer>& value : observers) {
        try {
          function(*value);
        } catch (...) {}
      }
    } catch (...) {}
  }

  std::mutex m_mutex;
  std::recursive_mutex m_callback_mutex;
  std::vector<entry> m_values;
  std::uint64_t m_next_id{};
};

observer_registry::observer_registry()
  : m_impl{std::make_unique<observer_registry_implementation>()}
{}

observer_registry::~observer_registry() = default;

std::uint64_t observer_registry::add(std::shared_ptr<observer> value)
{
  std::lock_guard lock{m_impl->m_mutex};
  const std::uint64_t id = ++m_impl->m_next_id;
  m_impl->m_values.push_back(observer_registry_implementation::entry{id, std::move(value)});
  return id;
}

void observer_registry::remove(std::uint64_t id) noexcept
{
  try {
    std::lock_guard lock{m_impl->m_mutex};
    m_impl->m_values.erase(
        std::remove_if(m_impl->m_values.begin(), m_impl->m_values.end(),
                       [id](const observer_registry_implementation::entry& item) { return item.id == id; }),
        m_impl->m_values.end());
  } catch (...) {}
}

void observer_registry::notify(const listener_event& event) noexcept
{
  m_impl->notify_each([&event](observer& value) { value.on_event(event); });
}

void observer_registry::notify(const server_event& event) noexcept
{
  m_impl->notify_each([&event](observer& value) { value.on_server_event(event); });
}

} // namespace serveza::detail
