#============================================================================
#
# Copyright (C) 2025 Ivan Pinezhaninov <ivan.pinezhaninov@gmail.com>
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

cmake_minimum_required(VERSION 3.24)

include(FetchContent)

set(BOOST_VERSION 1.90.0)
set(BOOST_INCLUDE_LIBRARIES any asio beast coroutine json system)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
  Boost
  URL https://github.com/boostorg/boost/releases/download/boost-${BOOST_VERSION}/boost-${BOOST_VERSION}-cmake.tar.xz
  USES_TERMINAL_DOWNLOAD ON
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  OVERRIDE_FIND_PACKAGE
)

FetchContent_MakeAvailable(Boost)

set(TARGET BOOST_TARGET)

add_library(${TARGET} INTERFACE)

target_link_libraries(${TARGET} INTERFACE
  Boost::any
  Boost::asio
  Boost::beast
  Boost::system
)

if(WIN32)
  target_compile_definitions(boost_mpl INTERFACE
    BOOST_MPL_CFG_NO_PREPROCESSED_HEADERS
  )
endif()

add_library(Boost::boost ALIAS ${TARGET})
