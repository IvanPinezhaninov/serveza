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

cmake_minimum_required(VERSION 3.28)

project(ServezaInstalledConsumer LANGUAGES CXX)

if(SERVEZA_TEST_PREDEFINE_BOOST)
  if(NOT DEFINED SERVEZA_TEST_BOOST_INCLUDE_DIRS_ENCODED OR SERVEZA_TEST_BOOST_INCLUDE_DIRS_ENCODED STREQUAL "")
    message(FATAL_ERROR "SERVEZA_TEST_BOOST_INCLUDE_DIRS_ENCODED is required when predefining Boost::asio")
  endif()
  string(REPLACE "|" ";" serveza_test_boost_include_dirs "${SERVEZA_TEST_BOOST_INCLUDE_DIRS_ENCODED}")
  add_library(Boost::asio INTERFACE IMPORTED GLOBAL)
  target_include_directories(Boost::asio INTERFACE ${serveza_test_boost_include_dirs})
endif()

find_package(serveza 0.1 CONFIG REQUIRED)

add_executable(ServezaInstalledConsumer src/main.cpp)
target_compile_features(ServezaInstalledConsumer PRIVATE cxx_std_17)
target_link_libraries(ServezaInstalledConsumer PRIVATE serveza::serveza)
set_target_properties(ServezaInstalledConsumer PROPERTIES CXX_EXTENSIONS OFF)

get_target_property(serveza_consumer_library_type serveza::serveza TYPE)
if(WIN32 AND serveza_consumer_library_type STREQUAL "SHARED_LIBRARY")
  add_custom_command(
    TARGET ServezaInstalledConsumer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:serveza::serveza>
      $<TARGET_FILE_DIR:ServezaInstalledConsumer>
    VERBATIM
  )
endif()

if(SERVEZA_TEST_ENABLE_SANITIZERS)
  target_compile_options(ServezaInstalledConsumer PRIVATE
    -fsanitize=address,undefined
    -fno-omit-frame-pointer
    -DBOOST_USE_ASAN
  )
  target_link_options(ServezaInstalledConsumer PRIVATE -fsanitize=address,undefined)
endif()
if(SERVEZA_TEST_ENABLE_THREAD_SANITIZER)
  target_compile_options(ServezaInstalledConsumer PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(ServezaInstalledConsumer PRIVATE -fsanitize=thread)
endif()
if(SERVEZA_TEST_ENABLE_COVERAGE)
  target_compile_options(ServezaInstalledConsumer PRIVATE --coverage -O0 -g)
  target_link_options(ServezaInstalledConsumer PRIVATE --coverage)
endif()

enable_testing()
add_test(NAME ServezaInstalledConsumerRuns COMMAND ServezaInstalledConsumer)
