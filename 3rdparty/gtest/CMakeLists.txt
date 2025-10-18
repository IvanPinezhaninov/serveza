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

if(TARGET GTest::gtest)
  message(STATUS "Using GoogleTest targets provided by the parent project")
elseif(SERVEZA_USE_BUNDLED_GTEST)
  include(FetchContent)

  set(SERVEZA_GTEST_VERSION 1.17.0)
  set(INSTALL_GTEST OFF CACHE BOOL "Disable GoogleTest installation" FORCE)
  if(MSVC)
    set(gtest_force_shared_crt ON CACHE BOOL "Use the shared MSVC runtime" FORCE)
  endif()

  FetchContent_Declare(
    serveza_googletest
    URL https://github.com/google/googletest/releases/download/v${SERVEZA_GTEST_VERSION}/googletest-${SERVEZA_GTEST_VERSION}.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SYSTEM
  )
  FetchContent_MakeAvailable(serveza_googletest)
  message(STATUS "Using bundled GoogleTest ${SERVEZA_GTEST_VERSION}")
else()
  find_package(GTest REQUIRED)
  foreach(serveza_gtest_target gtest gtest_main gmock gmock_main)
    serveza_promote_imported_target(GTest::${serveza_gtest_target})
  endforeach()
  message(STATUS "Using system GoogleTest")
endif()

if(NOT TARGET GTest::gtest_main)
  message(FATAL_ERROR "The selected GoogleTest dependency must provide GTest::gtest_main")
endif()
