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

project(serveza LANGUAGES CXX VERSION 0.1.0)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(SERVEZA_BOOST_MIN_VERSION 1.83.0)
set(SERVEZA_BUNDLED_BOOST_VERSION 1.92.0)

if(PROJECT_IS_TOP_LEVEL)
  option(BUILD_SHARED_LIBS "Build libraries as shared libraries" OFF)
endif()

if(PROJECT_IS_TOP_LEVEL AND WIN32 AND NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

option(SERVEZA_INSTALL "Generate install and package configuration rules" ${PROJECT_IS_TOP_LEVEL})

option(SERVEZA_BUILD_EXAMPLES "Build examples" OFF)
option(SERVEZA_BUILD_HTTP_EXAMPLES "Build examples that require Boost.Beast" OFF)
option(SERVEZA_BUILD_TLS_EXAMPLES "Build examples that require OpenSSL" OFF)

option(SERVEZA_BUILD_TESTS "Build tests" OFF)
option(SERVEZA_BUILD_HTTP_TESTS "Build integration tests that require Boost.Beast" OFF)
option(SERVEZA_BUILD_TLS_TESTS "Build integration tests that require OpenSSL" OFF)

option(SERVEZA_BUILD_CPP20 "Build optional C++20 coroutine tests and examples" OFF)
option(SERVEZA_BUILD_DOCS "Build the Doxygen API reference" OFF)

option(SERVEZA_USE_BUNDLED_BOOST "Build Serveza's pinned Boost when no parent targets exist" ON)
option(SERVEZA_USE_BUNDLED_GTEST "Build Serveza's pinned GoogleTest when no parent targets exist" ON)
option(SERVEZA_USE_BUNDLED_OPENSSL "Build Serveza's pinned OpenSSL when no parent targets exist" ON)

option(SERVEZA_ENABLE_COVERAGE "Enable GCC coverage instrumentation" OFF)
option(SERVEZA_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SERVEZA_ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)

set(SERVEZA_EXTRA_BOOST_COMPONENTS "" CACHE STRING "Additional Boost components requested by a parent project")
mark_as_advanced(SERVEZA_EXTRA_BOOST_COMPONENTS)

if(SERVEZA_ENABLE_SANITIZERS AND SERVEZA_ENABLE_THREAD_SANITIZER)
  message(FATAL_ERROR "Address/undefined sanitizers and ThreadSanitizer are mutually exclusive")
endif()
if(SERVEZA_ENABLE_COVERAGE AND (SERVEZA_ENABLE_SANITIZERS OR SERVEZA_ENABLE_THREAD_SANITIZER))
  message(FATAL_ERROR "Coverage and sanitizer instrumentation are mutually exclusive")
endif()
if(SERVEZA_ENABLE_COVERAGE AND NOT SERVEZA_BUILD_TESTS)
  message(FATAL_ERROR "SERVEZA_ENABLE_COVERAGE requires SERVEZA_BUILD_TESTS")
endif()
if(SERVEZA_BUILD_CPP20 AND NOT cxx_std_20 IN_LIST CMAKE_CXX_COMPILE_FEATURES)
  message(FATAL_ERROR "SERVEZA_BUILD_CPP20 requires a compiler with C++20 support")
endif()

include(ServezaDeveloperOptions)
include(ServezaDependencies)
add_subdirectory(3rdparty)

add_library(serveza
  src/exception.cpp
  src/listener.cpp
  src/observer.cpp
  src/observer_registry.cpp
  src/server.cpp
)
add_library(serveza::serveza ALIAS serveza)

if(TARGET Boost::asio_core)
  set(serveza_boost_core_target Boost::asio_core)
else()
  set(serveza_boost_core_target Boost::asio)
endif()

target_sources(serveza
  PUBLIC
    FILE_SET public_headers TYPE HEADERS
    BASE_DIRS include
    FILES
      include/serveza/awaitable_session.h
      include/serveza/bound_socket_context.h
      include/serveza/callback_session.h
      include/serveza/connection_info.h
      include/serveza/detail/basic_socket_listener.h
      include/serveza/detail/exception.h
      include/serveza/detail/observer_registry.h
      include/serveza/export.h
      include/serveza/listener.h
      include/serveza/listener_options.h
      include/serveza/listener_status.h
      include/serveza/observer.h
      include/serveza/server.h
      include/serveza/server_status.h
      include/serveza/serveza.h
      include/serveza/session_context.h
      include/serveza/yield_session.h
)

target_compile_features(serveza PUBLIC cxx_std_17)
# Keep Asio's ABI-affecting mutex backend consistent between the C++17 core and C++20 consumers.
target_compile_definitions(serveza PUBLIC
  BOOST_ASIO_DISABLE_STD_ATOMIC_WAIT
  BOOST_ASIO_NO_DEPRECATED
)
target_include_directories(serveza
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(serveza
  PRIVATE
    $<BUILD_LOCAL_INTERFACE:${serveza_boost_core_target}>
  INTERFACE
    $<BUILD_LOCAL_INTERFACE:${serveza_boost_core_target}>
    $<INSTALL_INTERFACE:serveza_dependencies::boost_asio>
    Threads::Threads
)

get_target_property(SERVEZA_LIBRARY_TYPE serveza TYPE)
if(SERVEZA_LIBRARY_TYPE STREQUAL "SHARED_LIBRARY")
  target_compile_definitions(serveza
    PRIVATE SERVEZA_LIBRARY_BUILD
    INTERFACE SERVEZA_USE_SHARED
  )
endif()

if(WIN32)
  target_compile_definitions(serveza PUBLIC _WIN32_WINNT=0x0A00 BOOST_ALL_NO_LIB)
endif()

set_target_properties(serveza PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  EXPORT_NAME serveza
  POSITION_INDEPENDENT_CODE ON
  VERSION "${PROJECT_VERSION}"
  VISIBILITY_INLINES_HIDDEN ON
)
if(PROJECT_VERSION_MAJOR EQUAL 0)
  set_property(TARGET serveza PROPERTY SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
else()
  set_property(TARGET serveza PROPERTY SOVERSION "${PROJECT_VERSION_MAJOR}")
endif()
serveza_enable_developer_options(serveza)

if(SERVEZA_INSTALL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)

  set(serveza_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/serveza")
  configure_package_config_file(
    cmake/servezaConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/servezaConfig.cmake"
    INSTALL_DESTINATION "${serveza_package_dir}"
  )
  if(PROJECT_VERSION_MAJOR EQUAL 0)
    set(serveza_version_compatibility SameMinorVersion)
  else()
    set(serveza_version_compatibility SameMajorVersion)
  endif()
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/servezaConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY "${serveza_version_compatibility}"
  )

  install(
    TARGETS serveza
    EXPORT servezaTargets
    FILE_SET public_headers DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  )
  install(
    EXPORT servezaTargets
    FILE servezaTargets.cmake
    NAMESPACE serveza::
    DESTINATION "${serveza_package_dir}"
  )
  install(
    FILES
      "${CMAKE_CURRENT_BINARY_DIR}/servezaConfig.cmake"
      "${CMAKE_CURRENT_BINARY_DIR}/servezaConfigVersion.cmake"
    DESTINATION "${serveza_package_dir}"
  )
  install(
    FILES
      LICENSE
      README.md
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
  )
endif()

if(PROJECT_IS_TOP_LEVEL AND SERVEZA_INSTALL)
  include(ServezaPackaging)
endif()

if(SERVEZA_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(SERVEZA_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)

  if(SERVEZA_ENABLE_COVERAGE)
    include(ServezaCoverage)
    serveza_add_coverage_target()
  endif()
endif()

if(SERVEZA_BUILD_DOCS)
  include(ServezaDoxygen)
  serveza_add_doxygen_target()
endif()
