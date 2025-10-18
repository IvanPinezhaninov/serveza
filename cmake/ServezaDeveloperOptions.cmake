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

include_guard(GLOBAL)

add_library(serveza_developer_options INTERFACE)

if(MSVC)
  target_compile_options(serveza_developer_options INTERFACE
    /W4
    /WX
    /bigobj
    /permissive-
    /wd4251
    /Zc:__cplusplus
  )
else()
  target_compile_options(serveza_developer_options INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Werror
    -pedantic-errors
  )
endif()

if(SERVEZA_ENABLE_COVERAGE)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT WIN32)
    target_compile_options(serveza_developer_options INTERFACE --coverage -O0 -g)
    target_link_options(serveza_developer_options INTERFACE --coverage)
  else()
    message(FATAL_ERROR "SERVEZA_ENABLE_COVERAGE currently requires GCC on a Unix-like platform")
  endif()
endif()

if(SERVEZA_ENABLE_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT WIN32)
    target_compile_options(serveza_developer_options INTERFACE
      -fsanitize=address,undefined
      -fno-omit-frame-pointer
      -DBOOST_USE_ASAN
    )
    target_link_options(serveza_developer_options INTERFACE -fsanitize=address,undefined)
  else()
    message(FATAL_ERROR "SERVEZA_ENABLE_SANITIZERS is unsupported by this compiler")
  endif()
endif()

if(SERVEZA_ENABLE_THREAD_SANITIZER)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT WIN32)
    target_compile_options(serveza_developer_options INTERFACE
      -fsanitize=thread
      -fno-omit-frame-pointer
    )
    target_link_options(serveza_developer_options INTERFACE -fsanitize=thread)
  else()
    message(FATAL_ERROR "SERVEZA_ENABLE_THREAD_SANITIZER is unsupported by this compiler")
  endif()
endif()

function(serveza_enable_developer_options target)
  set_property(TARGET ${target} PROPERTY CXX_EXTENSIONS OFF)
  target_link_libraries(${target} PRIVATE $<BUILD_INTERFACE:serveza_developer_options>)
endfunction()
