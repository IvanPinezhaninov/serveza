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

cmake_minimum_required(VERSION 3.5)

project(serveza_tests LANGUAGES CXX VERSION 1.0.0)

include(FetchContent)

set(GTEST_VERSION 1.15.2)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
  GTest
  URL https://github.com/google/googletest/releases/download/v${GTEST_VERSION}/googletest-${GTEST_VERSION}.tar.gz
  USES_TERMINAL_DOWNLOAD ON
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)

FetchContent_MakeAvailable(GTest)

set(HEADERS
  src/BaseTest.h
)

set(SOURCES
  src/main.cpp
  src/test_Listener.cpp
  src/test_TcpSession.cpp
  src/test_UdpSession.cpp
)

if(NOT WIN32)
  list(APPEND SOURCES
    src/test_LocalDatagramSession.cpp
    src/test_LocalStreamSession.cpp
  )
endif()

add_executable(${PROJECT_NAME}
  ${HEADERS}
  ${SOURCES}
)

set_target_properties(${PROJECT_NAME}
  PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(${PROJECT_NAME}
  PRIVATE
    GTest::gtest
    serveza::serveza
)

include(GoogleTest)
gtest_discover_tests(${PROJECT_NAME})
