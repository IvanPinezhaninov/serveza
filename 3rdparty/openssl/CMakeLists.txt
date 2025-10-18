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

if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
  message(STATUS "Using OpenSSL targets provided by the parent project")
  return()
endif()
if(TARGET OpenSSL::SSL OR TARGET OpenSSL::Crypto)
  message(FATAL_ERROR "The parent project must provide both OpenSSL::SSL and OpenSSL::Crypto")
endif()

if(NOT SERVEZA_USE_BUNDLED_OPENSSL)
  find_package(OpenSSL 3.0 REQUIRED GLOBAL)
  message(STATUS "Using system OpenSSL ${OPENSSL_VERSION}")
  return()
endif()

include(ExternalProject)
find_package(Perl REQUIRED)

set(serveza_openssl_version 3.5.7)
set(serveza_openssl_prefix "${CMAKE_BINARY_DIR}/_deps/serveza_openssl")
set(serveza_openssl_install_dir "${serveza_openssl_prefix}/install")
set(serveza_openssl_include_dir "${serveza_openssl_install_dir}/include")
set(serveza_openssl_library_dir "${serveza_openssl_install_dir}/lib")

if(WIN32)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(serveza_openssl_platform VC-WIN64A)
  else()
    set(serveza_openssl_platform VC-WIN32)
  endif()
  get_filename_component(serveza_msvc_tool_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  find_program(serveza_openssl_make_program NAMES nmake HINTS "${serveza_msvc_tool_dir}" REQUIRED)
  set(serveza_openssl_crypto_library "${serveza_openssl_library_dir}/libcrypto.lib")
  set(serveza_openssl_ssl_library "${serveza_openssl_library_dir}/libssl.lib")
elseif(APPLE)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(serveza_openssl_platform darwin64-arm64-cc)
  else()
    set(serveza_openssl_platform darwin64-x86_64-cc)
  endif()
  find_program(serveza_openssl_make_program NAMES gmake make REQUIRED)
  set(serveza_openssl_crypto_library "${serveza_openssl_library_dir}/libcrypto.a")
  set(serveza_openssl_ssl_library "${serveza_openssl_library_dir}/libssl.a")
else()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(serveza_openssl_platform linux-aarch64)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    set(serveza_openssl_platform linux-x86_64)
  else()
    message(FATAL_ERROR "Unsupported architecture for bundled OpenSSL: ${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  find_program(serveza_openssl_make_program NAMES gmake make REQUIRED)
  set(serveza_openssl_crypto_library "${serveza_openssl_library_dir}/libcrypto.a")
  set(serveza_openssl_ssl_library "${serveza_openssl_library_dir}/libssl.a")
endif()

file(MAKE_DIRECTORY "${serveza_openssl_include_dir}" "${serveza_openssl_library_dir}")

set(serveza_openssl_configure_options
  no-shared
  no-tests
  no-docs
  no-apps
  no-legacy
  --libdir=lib
  --prefix=${serveza_openssl_install_dir}
  --openssldir=${serveza_openssl_install_dir}
)
if(NOT WIN32)
  list(APPEND serveza_openssl_configure_options -fPIC)
endif()

ExternalProject_Add(serveza_bundled_openssl
  URL https://github.com/openssl/openssl/releases/download/openssl-${serveza_openssl_version}/openssl-${serveza_openssl_version}.tar.gz
  URL_HASH SHA256=a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  PREFIX "${serveza_openssl_prefix}"
  BUILD_IN_SOURCE ON
  CONFIGURE_COMMAND
    "${PERL_EXECUTABLE}" <SOURCE_DIR>/Configure ${serveza_openssl_platform} ${serveza_openssl_configure_options}
  BUILD_COMMAND "${serveza_openssl_make_program}"
  INSTALL_COMMAND "${serveza_openssl_make_program}" install_sw
  BUILD_BYPRODUCTS "${serveza_openssl_crypto_library}" "${serveza_openssl_ssl_library}"
  EXCLUDE_FROM_ALL TRUE
)

add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
  IMPORTED_LOCATION "${serveza_openssl_crypto_library}"
  INTERFACE_INCLUDE_DIRECTORIES "${serveza_openssl_include_dir}"
)
add_dependencies(OpenSSL::Crypto serveza_bundled_openssl)

if(WIN32)
  target_link_libraries(OpenSSL::Crypto INTERFACE advapi32 bcrypt crypt32 user32 ws2_32)
else()
  target_link_libraries(OpenSSL::Crypto INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
endif()

add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::SSL PROPERTIES
  IMPORTED_LOCATION "${serveza_openssl_ssl_library}"
  INTERFACE_INCLUDE_DIRECTORIES "${serveza_openssl_include_dir}"
)
target_link_libraries(OpenSSL::SSL INTERFACE OpenSSL::Crypto)
add_dependencies(OpenSSL::SSL serveza_bundled_openssl)

set(OPENSSL_FOUND TRUE)
set(OPENSSL_VERSION "${serveza_openssl_version}")
message(STATUS "Using bundled OpenSSL ${serveza_openssl_version}")
