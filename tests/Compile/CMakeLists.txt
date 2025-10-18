#============================================================================
#
# Copyright (C) 2026 Ivan Pinezhaninov <ivan.pinezhaninov@gmail.com>
#
# This file is part of the serveza which can be found at
# https://github.com/IvanPinezhaninov/serveza/.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
# OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
# THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#
#============================================================================

add_library(ServezaHeaderTests OBJECT
  awaitable_session.cpp
  bound_socket_context.cpp
  callback_session.cpp
  connection_info.cpp
  export.cpp
  listener.cpp
  listener_options.cpp
  listener_status.cpp
  observer.cpp
  server.cpp
  server_status.cpp
  serveza.cpp
  session_context.cpp
)
target_link_libraries(ServezaHeaderTests PRIVATE serveza::serveza)
target_compile_features(ServezaHeaderTests PRIVATE cxx_std_17)
serveza_enable_developer_options(ServezaHeaderTests)

add_library(ServezaYieldHeaderTest OBJECT yield_session.cpp)
target_link_libraries(ServezaYieldHeaderTest PRIVATE serveza::serveza Boost::coroutine)
target_compile_features(ServezaYieldHeaderTest PRIVATE cxx_std_17)
serveza_enable_developer_options(ServezaYieldHeaderTest)
