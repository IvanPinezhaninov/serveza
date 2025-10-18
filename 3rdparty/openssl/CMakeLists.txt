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

set(OPENSSL_VERSION 3.5.4)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
  OpenSSL
  URL https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  OVERRIDE_FIND_PACKAGE
)

FetchContent_MakeAvailable(openssl)

set(OPENSSL_INSTALL_DIR    "${openssl_BINARY_DIR}/install")
set(OPENSSL_INCLUDE_DIR    "${OPENSSL_INSTALL_DIR}/include")
set(OPENSSL_LIB_DIR        "${OPENSSL_INSTALL_DIR}/lib")
set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_LIB_DIR}/libcrypto${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(OPENSSL_SSL_LIBRARY    "${OPENSSL_LIB_DIR}/libssl${CMAKE_STATIC_LIBRARY_SUFFIX}")

file(MAKE_DIRECTORY "${OPENSSL_INCLUDE_DIR}")
file(MAKE_DIRECTORY "${OPENSSL_LIB_DIR}")

if(WIN32)
  if(MSVC)
    set(OPENSSL_PLATFORM "VC-WIN64A")
  else()
    set(OPENSSL_PLATFORM "mingw64")
  endif()
elseif(APPLE)
  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    set(OPENSSL_PLATFORM "darwin64-arm64-cc")
  else()
    set(OPENSSL_PLATFORM "darwin64-x86_64")
  endif()
else()
  set(OPENSSL_PLATFORM "linux-x86_64")
endif()

set(OPENSSL_CONFIGURE_OPTIONS
  no-shared
  no-pinshared
  no-apps
  no-tests
  no-docs
  no-legacy
  no-async
  no-comp
  no-ec2m
  no-idea
  no-md2
  no-md4
  no-rc2
  no-rc4
  no-rc5
  no-sctp
  no-sm2
  no-sm3
  no-sm4
  no-srp
  no-ssl3
  no-ssl3-method
  no-tls1
  no-tls1-method
  no-tls1_1
  no-tls1_1-method
  --libdir=lib
  --prefix=${OPENSSL_INSTALL_DIR}
  --openssldir=${OPENSSL_INSTALL_DIR}
)

if(NOT WIN32)
  list(APPEND OPENSSL_CONFIGURE_OPTIONS -fPIC)
endif()

set(CONFIGURE_CMD perl Configure
  ${OPENSSL_PLATFORM}
  ${OPENSSL_CONFIGURE_OPTIONS}
)

if(WIN32)
  set(BUILD_CMD nmake install_sw)
else()
  set(BUILD_CMD make install_sw)
endif()

add_custom_command(
  OUTPUT "${OPENSSL_SSL_LIBRARY}" "${OPENSSL_CRYPTO_LIBRARY}"
  COMMAND ${CONFIGURE_CMD}
  COMMAND ${BUILD_CMD}
  WORKING_DIRECTORY ${openssl_SOURCE_DIR}
  COMMENT "Building OpenSSL ${OPENSSL_VERSION}"
)

add_custom_target(build_openssl
  DEPENDS "${OPENSSL_SSL_LIBRARY}" "${OPENSSL_CRYPTO_LIBRARY}"
)

include(CMakePackageConfigHelpers)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/OpenSSLConfig.cmake.in"
  "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/openssl-config.cmake"
  INSTALL_DESTINATION "${OPENSSL_LIB_DIR}/cmake/OpenSSL"
)

write_basic_package_version_file(
  "${CMAKE_FIND_PACKAGE_REDIRECTS_DIR}/openssl-config-version.cmake"
  VERSION ${OPENSSL_VERSION}
  COMPATIBILITY AnyNewerVersion
)
