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

if(NOT TARGET GTest::gtest_main)
  message(FATAL_ERROR "The third-party dependency layer did not provide GTest::gtest_main")
endif()

include(GoogleTest)

set(serveza_test_boost_source_dir "${Boost_SOURCE_DIR}")
if(serveza_test_boost_source_dir STREQUAL "")
  include(FetchContent)
  FetchContent_GetProperties(serveza_boost SOURCE_DIR serveza_test_boost_source_dir)
endif()
if(NOT serveza_test_boost_source_dir STREQUAL "")
  file(GLOB serveza_test_boost_include_dirs LIST_DIRECTORIES TRUE
    "${serveza_test_boost_source_dir}/libs/*/include"
  )
elseif(Boost_INCLUDE_DIRS)
  set(serveza_test_boost_include_dirs "${Boost_INCLUDE_DIRS}")
else()
  get_target_property(serveza_test_boost_include_dirs Boost::asio INTERFACE_INCLUDE_DIRECTORIES)
  if(NOT serveza_test_boost_include_dirs OR serveza_test_boost_include_dirs MATCHES "-NOTFOUND$")
    foreach(serveza_test_boost_header_target Boost::headers Boost::boost)
      if(TARGET ${serveza_test_boost_header_target})
        get_target_property(serveza_test_boost_include_dirs
          ${serveza_test_boost_header_target} INTERFACE_INCLUDE_DIRECTORIES)
        if(serveza_test_boost_include_dirs AND NOT serveza_test_boost_include_dirs MATCHES "-NOTFOUND$")
          break()
        endif()
      endif()
    endforeach()
  endif()
endif()
if(NOT serveza_test_boost_include_dirs OR serveza_test_boost_include_dirs MATCHES "-NOTFOUND$")
  message(FATAL_ERROR "Unable to determine Boost include directories for consumer tests")
endif()
string(REPLACE ";" "|" serveza_test_boost_include_dirs_encoded "${serveza_test_boost_include_dirs}")

include(FetchContent)
FetchContent_GetProperties(serveza_googletest SOURCE_DIR serveza_test_gtest_source_dir)

set(serveza_parent_dependency_arguments
  -DSERVEZA_TEST_BOOST_SOURCE_DIR=${serveza_test_boost_source_dir}
  -DSERVEZA_TEST_BOOST_DIR=${Boost_DIR}
  -DSERVEZA_TEST_GTEST_SOURCE_DIR=${serveza_test_gtest_source_dir}
  -DSERVEZA_TEST_GTEST_DIR=${GTest_DIR}
  -DSERVEZA_TEST_ENABLE_HTTP=${SERVEZA_BUILD_HTTP_TESTS}
  -DSERVEZA_TEST_ENABLE_TLS=${SERVEZA_BUILD_TLS_TESTS}
  -DSERVEZA_TEST_ENABLE_CPP20=${SERVEZA_BUILD_CPP20}
)
if(SERVEZA_BUILD_TLS_TESTS)
  get_target_property(serveza_test_openssl_include_dirs OpenSSL::SSL INTERFACE_INCLUDE_DIRECTORIES)
  if(WIN32)
    set(serveza_test_openssl_system_libraries "advapi32|bcrypt|crypt32|user32|ws2_32")
  endif()
  list(APPEND serveza_parent_dependency_arguments
    -DSERVEZA_TEST_OPENSSL_INCLUDE_DIRS=${serveza_test_openssl_include_dirs}
    -DSERVEZA_TEST_OPENSSL_SSL_LIBRARY=$<TARGET_FILE:OpenSSL::SSL>
    -DSERVEZA_TEST_OPENSSL_CRYPTO_LIBRARY=$<TARGET_FILE:OpenSSL::Crypto>
    -DSERVEZA_TEST_OPENSSL_SYSTEM_LIBRARIES=${serveza_test_openssl_system_libraries}
  )
endif()

add_subdirectory(Compile)
add_subdirectory(Unit)
add_subdirectory(Integration)

add_test(
  NAME ParentDependenciesTest
  COMMAND ${CMAKE_COMMAND}
    -DSERVEZA_SOURCE_DIR=${PROJECT_SOURCE_DIR}
    -DSERVEZA_TEST_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/ParentDependencies
    -DSERVEZA_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DSERVEZA_TEST_C_COMPILER=${CMAKE_C_COMPILER}
    -DSERVEZA_TEST_CXX_SCAN_FOR_MODULES=${CMAKE_CXX_SCAN_FOR_MODULES}
    -DSERVEZA_TEST_GENERATOR=${CMAKE_GENERATOR}
    -DSERVEZA_TEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
    -DSERVEZA_TEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
    ${serveza_parent_dependency_arguments}
    -DSERVEZA_TEST_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DSERVEZA_TEST_CONFIG=$<CONFIG>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/ParentDependencies/RunParentDependenciesTest.cmake
)
set_tests_properties(ParentDependenciesTest PROPERTIES LABELS cmake TIMEOUT 300)

if(SERVEZA_INSTALL)
  add_test(
    NAME InstalledConsumerTest
    COMMAND ${CMAKE_COMMAND}
      -DSERVEZA_BUILD_DIR=${CMAKE_BINARY_DIR}
      -DSERVEZA_SOURCE_DIR=${PROJECT_SOURCE_DIR}
      -DSERVEZA_TEST_BOOST_INCLUDE_DIRS_ENCODED=${serveza_test_boost_include_dirs_encoded}
      -DSERVEZA_TEST_BOOST_VERSION=${SERVEZA_RESOLVED_BOOST_VERSION}
      -DSERVEZA_TEST_PREDEFINE_BOOST=ON
      -DSERVEZA_TEST_DISCOVER_BOOST=OFF
      -DSERVEZA_TEST_VARIANT=preprovided
      -DSERVEZA_TEST_GENERATOR=${CMAKE_GENERATOR}
      -DSERVEZA_TEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
      -DSERVEZA_TEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
      -DSERVEZA_TEST_BUILD_TYPE=${CMAKE_BUILD_TYPE}
      -DSERVEZA_TEST_CONFIG=$<CONFIG>
      -DSERVEZA_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DSERVEZA_TEST_ENABLE_COVERAGE=${SERVEZA_ENABLE_COVERAGE}
      -DSERVEZA_TEST_ENABLE_SANITIZERS=${SERVEZA_ENABLE_SANITIZERS}
      -DSERVEZA_TEST_ENABLE_THREAD_SANITIZER=${SERVEZA_ENABLE_THREAD_SANITIZER}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/Consumer/RunConsumerTest.cmake
  )
  set_tests_properties(InstalledConsumerTest PROPERTIES LABELS cmake TIMEOUT 60)

  add_test(
    NAME InstalledConsumerDependencyDiscoveryTest
    COMMAND ${CMAKE_COMMAND}
      -DSERVEZA_BUILD_DIR=${CMAKE_BINARY_DIR}
      -DSERVEZA_SOURCE_DIR=${PROJECT_SOURCE_DIR}
      -DSERVEZA_TEST_BOOST_INCLUDE_DIRS_ENCODED=${serveza_test_boost_include_dirs_encoded}
      -DSERVEZA_TEST_BOOST_VERSION=${SERVEZA_RESOLVED_BOOST_VERSION}
      -DSERVEZA_TEST_PREDEFINE_BOOST=OFF
      -DSERVEZA_TEST_DISCOVER_BOOST=ON
      -DSERVEZA_TEST_VARIANT=discovery
      -DSERVEZA_TEST_GENERATOR=${CMAKE_GENERATOR}
      -DSERVEZA_TEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
      -DSERVEZA_TEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
      -DSERVEZA_TEST_BUILD_TYPE=${CMAKE_BUILD_TYPE}
      -DSERVEZA_TEST_CONFIG=$<CONFIG>
      -DSERVEZA_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DSERVEZA_TEST_ENABLE_COVERAGE=${SERVEZA_ENABLE_COVERAGE}
      -DSERVEZA_TEST_ENABLE_SANITIZERS=${SERVEZA_ENABLE_SANITIZERS}
      -DSERVEZA_TEST_ENABLE_THREAD_SANITIZER=${SERVEZA_ENABLE_THREAD_SANITIZER}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/Consumer/RunConsumerTest.cmake
  )
  set_tests_properties(InstalledConsumerDependencyDiscoveryTest PROPERTIES LABELS cmake TIMEOUT 60)
endif()

set(serveza_check_targets ServezaAdapterTests ServezaHeaderTests ServezaUnitTests)
if(TARGET ServezaCoroutineTests)
  list(APPEND serveza_check_targets ServezaCoroutineTests)
endif()
add_custom_target(
  ServezaCheck
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -C $<CONFIG>
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  DEPENDS ${serveza_check_targets}
  USES_TERMINAL
)
