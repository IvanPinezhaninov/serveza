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

project(ServezaParentDependencies LANGUAGES CXX)

if(NOT DEFINED SERVEZA_SOURCE_DIR OR SERVEZA_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "SERVEZA_SOURCE_DIR is required")
endif()
if(DEFINED SERVEZA_TEST_BOOST_SOURCE_DIR AND NOT SERVEZA_TEST_BOOST_SOURCE_DIR STREQUAL "")
  set(BOOST_INCLUDE_LIBRARIES asio coroutine)
  if(SERVEZA_TEST_ENABLE_HTTP)
    list(APPEND BOOST_INCLUDE_LIBRARIES beast)
  endif()
  set(BOOST_SKIP_INSTALL_RULES ON)
  set(BUILD_SHARED_LIBS OFF)
  add_subdirectory(
    "${SERVEZA_TEST_BOOST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/parent-boost"
    EXCLUDE_FROM_ALL SYSTEM
  )
else()
  if(DEFINED SERVEZA_TEST_BOOST_DIR AND NOT SERVEZA_TEST_BOOST_DIR STREQUAL "")
    set(Boost_DIR "${SERVEZA_TEST_BOOST_DIR}")
  endif()
  set(serveza_parent_boost_components coroutine)
  if(SERVEZA_TEST_ENABLE_HTTP)
    set(serveza_parent_needs_beast ON)
  endif()
  find_package(Boost 1.83 REQUIRED COMPONENTS ${serveza_parent_boost_components})
  foreach(serveza_parent_header_component asio beast)
    if((serveza_parent_header_component STREQUAL "asio" OR serveza_parent_needs_beast) AND
       NOT TARGET Boost::${serveza_parent_header_component})
      if(TARGET Boost::headers)
        set(serveza_parent_boost_headers_target Boost::headers)
      else()
        set(serveza_parent_boost_headers_target Boost::boost)
      endif()
      add_library(Boost::${serveza_parent_header_component} INTERFACE IMPORTED GLOBAL)
      set_property(TARGET Boost::${serveza_parent_header_component}
                   PROPERTY INTERFACE_LINK_LIBRARIES ${serveza_parent_boost_headers_target})
    endif()
  endforeach()
endif()

if(DEFINED SERVEZA_TEST_GTEST_SOURCE_DIR AND NOT SERVEZA_TEST_GTEST_SOURCE_DIR STREQUAL "")
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  add_subdirectory(
    "${SERVEZA_TEST_GTEST_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/parent-gtest"
    EXCLUDE_FROM_ALL SYSTEM
  )
else()
  if(DEFINED SERVEZA_TEST_GTEST_DIR AND NOT SERVEZA_TEST_GTEST_DIR STREQUAL "")
    set(GTest_DIR "${SERVEZA_TEST_GTEST_DIR}")
  endif()
  find_package(GTest REQUIRED)
endif()

if(SERVEZA_TEST_ENABLE_TLS)
  if(NOT SERVEZA_TEST_OPENSSL_SSL_LIBRARY OR NOT SERVEZA_TEST_OPENSSL_CRYPTO_LIBRARY)
    message(FATAL_ERROR "Parent OpenSSL SSL and Crypto libraries are required")
  endif()
  add_library(OpenSSL::SSL UNKNOWN IMPORTED GLOBAL)
  set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${SERVEZA_TEST_OPENSSL_SSL_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SERVEZA_TEST_OPENSSL_INCLUDE_DIRS}"
  )
  add_library(OpenSSL::Crypto UNKNOWN IMPORTED GLOBAL)
  set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${SERVEZA_TEST_OPENSSL_CRYPTO_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SERVEZA_TEST_OPENSSL_INCLUDE_DIRS}"
  )
  if(SERVEZA_TEST_OPENSSL_SYSTEM_LIBRARIES)
    string(REPLACE "|" ";" serveza_openssl_system_libraries "${SERVEZA_TEST_OPENSSL_SYSTEM_LIBRARIES}")
    target_link_libraries(OpenSSL::Crypto INTERFACE ${serveza_openssl_system_libraries})
  endif()
  target_link_libraries(OpenSSL::SSL INTERFACE OpenSSL::Crypto)
endif()

# Bundled mode remains enabled and its sources are deliberately invalid. The
# existing parent targets must win before Serveza attempts any download.
set(SERVEZA_USE_BUNDLED_BOOST ON CACHE BOOL "" FORCE)
set(SERVEZA_USE_BUNDLED_GTEST ON CACHE BOOL "" FORCE)
set(SERVEZA_USE_BUNDLED_OPENSSL ON CACHE BOOL "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_SERVEZA_BOOST
    "${CMAKE_CURRENT_BINARY_DIR}/must-not-use-bundled-boost" CACHE PATH "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_SERVEZA_GOOGLETEST
    "${CMAKE_CURRENT_BINARY_DIR}/must-not-use-bundled-googletest" CACHE PATH "" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_Perl TRUE)

set(SERVEZA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SERVEZA_BUILD_TLS_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SERVEZA_BUILD_TESTS ON CACHE BOOL "" FORCE)
set(SERVEZA_BUILD_CPP20 ${SERVEZA_TEST_ENABLE_CPP20} CACHE BOOL "" FORCE)
set(SERVEZA_BUILD_HTTP_TESTS ${SERVEZA_TEST_ENABLE_HTTP} CACHE BOOL "" FORCE)
set(SERVEZA_BUILD_TLS_TESTS ${SERVEZA_TEST_ENABLE_TLS} CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(serveza_under_test SOURCE_DIR "${SERVEZA_SOURCE_DIR}")
FetchContent_MakeAvailable(serveza_under_test)

set(serveza_required_targets serveza ServezaAdapterTests ServezaHeaderTests)
if(SERVEZA_TEST_ENABLE_CPP20)
  list(APPEND serveza_required_targets ServezaCoroutineTests)
endif()
if(SERVEZA_TEST_ENABLE_HTTP)
  list(APPEND serveza_required_targets ServezaHttpTests)
endif()
if(SERVEZA_TEST_ENABLE_TLS)
  list(APPEND serveza_required_targets ServezaTlsTests)
endif()
foreach(serveza_required_target IN LISTS serveza_required_targets)
  if(NOT TARGET ${serveza_required_target})
    message(FATAL_ERROR "Embedded Serveza did not create ${serveza_required_target}")
  endif()
endforeach()

add_executable(ServezaParentConsumer src/main.cpp)
target_compile_features(ServezaParentConsumer PRIVATE cxx_std_17)
target_link_libraries(ServezaParentConsumer PRIVATE serveza::serveza)
set_target_properties(ServezaParentConsumer PROPERTIES CXX_EXTENSIONS OFF)

enable_testing()
add_test(NAME ServezaParentConsumerRuns COMMAND ServezaParentConsumer)
