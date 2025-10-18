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

foreach(serveza_required_variable
        SERVEZA_SOURCE_DIR
        SERVEZA_TEST_BUILD_DIR
        SERVEZA_TEST_CXX_COMPILER
        SERVEZA_TEST_GENERATOR)
  if(NOT DEFINED ${serveza_required_variable} OR "${${serveza_required_variable}}" STREQUAL "")
    message(FATAL_ERROR "${serveza_required_variable} is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${SERVEZA_TEST_BUILD_DIR}")
set(serveza_configure_command
  "${CMAKE_COMMAND}"
  -S "${SERVEZA_SOURCE_DIR}/tests/ParentDependencies"
  -B "${SERVEZA_TEST_BUILD_DIR}"
  -G "${SERVEZA_TEST_GENERATOR}"
  "-DCMAKE_CXX_COMPILER=${SERVEZA_TEST_CXX_COMPILER}"
  "-DSERVEZA_SOURCE_DIR=${SERVEZA_SOURCE_DIR}"
  "-DSERVEZA_TEST_BOOST_SOURCE_DIR=${SERVEZA_TEST_BOOST_SOURCE_DIR}"
  "-DSERVEZA_TEST_BOOST_DIR=${SERVEZA_TEST_BOOST_DIR}"
  "-DSERVEZA_TEST_GTEST_SOURCE_DIR=${SERVEZA_TEST_GTEST_SOURCE_DIR}"
  "-DSERVEZA_TEST_GTEST_DIR=${SERVEZA_TEST_GTEST_DIR}"
  "-DSERVEZA_TEST_ENABLE_HTTP=${SERVEZA_TEST_ENABLE_HTTP}"
  "-DSERVEZA_TEST_ENABLE_TLS=${SERVEZA_TEST_ENABLE_TLS}"
  "-DSERVEZA_TEST_ENABLE_CPP20=${SERVEZA_TEST_ENABLE_CPP20}"
  "-DSERVEZA_TEST_OPENSSL_INCLUDE_DIRS=${SERVEZA_TEST_OPENSSL_INCLUDE_DIRS}"
  "-DSERVEZA_TEST_OPENSSL_SSL_LIBRARY=${SERVEZA_TEST_OPENSSL_SSL_LIBRARY}"
  "-DSERVEZA_TEST_OPENSSL_CRYPTO_LIBRARY=${SERVEZA_TEST_OPENSSL_CRYPTO_LIBRARY}"
  "-DSERVEZA_TEST_OPENSSL_SYSTEM_LIBRARIES=${SERVEZA_TEST_OPENSSL_SYSTEM_LIBRARIES}"
)
if(DEFINED SERVEZA_TEST_C_COMPILER AND NOT SERVEZA_TEST_C_COMPILER STREQUAL "")
  list(APPEND serveza_configure_command "-DCMAKE_C_COMPILER=${SERVEZA_TEST_C_COMPILER}")
endif()
if(DEFINED SERVEZA_TEST_CXX_SCAN_FOR_MODULES AND NOT SERVEZA_TEST_CXX_SCAN_FOR_MODULES STREQUAL "")
  list(APPEND serveza_configure_command
    "-DCMAKE_CXX_SCAN_FOR_MODULES=${SERVEZA_TEST_CXX_SCAN_FOR_MODULES}"
  )
endif()
if(DEFINED SERVEZA_TEST_GENERATOR_PLATFORM AND NOT SERVEZA_TEST_GENERATOR_PLATFORM STREQUAL "")
  list(APPEND serveza_configure_command -A "${SERVEZA_TEST_GENERATOR_PLATFORM}")
endif()
if(DEFINED SERVEZA_TEST_GENERATOR_TOOLSET AND NOT SERVEZA_TEST_GENERATOR_TOOLSET STREQUAL "")
  list(APPEND serveza_configure_command -T "${SERVEZA_TEST_GENERATOR_TOOLSET}")
endif()
if(DEFINED SERVEZA_TEST_BUILD_TYPE AND NOT SERVEZA_TEST_BUILD_TYPE STREQUAL "")
  list(APPEND serveza_configure_command "-DCMAKE_BUILD_TYPE=${SERVEZA_TEST_BUILD_TYPE}")
endif()

execute_process(COMMAND ${serveza_configure_command} RESULT_VARIABLE serveza_configure_result)
if(NOT serveza_configure_result EQUAL 0)
  message(FATAL_ERROR "Parent dependency configure failed with ${serveza_configure_result}")
endif()

set(serveza_build_command
  "${CMAKE_COMMAND}"
  --build "${SERVEZA_TEST_BUILD_DIR}"
  --target ServezaParentConsumer ServezaAdapterTests ServezaHeaderTests
)
if(SERVEZA_TEST_ENABLE_CPP20)
  list(APPEND serveza_build_command ServezaCoroutineTests)
endif()
if(SERVEZA_TEST_ENABLE_HTTP)
  list(APPEND serveza_build_command ServezaHttpTests)
endif()
if(SERVEZA_TEST_ENABLE_TLS)
  list(APPEND serveza_build_command ServezaTlsTests)
endif()
if(DEFINED SERVEZA_TEST_CONFIG AND NOT SERVEZA_TEST_CONFIG STREQUAL "")
  list(APPEND serveza_build_command --config "${SERVEZA_TEST_CONFIG}")
endif()
execute_process(COMMAND ${serveza_build_command} RESULT_VARIABLE serveza_build_result)
if(NOT serveza_build_result EQUAL 0)
  message(FATAL_ERROR "Parent dependency consumer build failed with ${serveza_build_result}")
endif()

set(serveza_ctest_command
  "${CMAKE_CTEST_COMMAND}"
  --test-dir "${SERVEZA_TEST_BUILD_DIR}"
  --output-on-failure
  -R ServezaParentConsumerRuns
)
if(DEFINED SERVEZA_TEST_CONFIG AND NOT SERVEZA_TEST_CONFIG STREQUAL "")
  list(APPEND serveza_ctest_command -C "${SERVEZA_TEST_CONFIG}")
endif()
execute_process(COMMAND ${serveza_ctest_command} RESULT_VARIABLE serveza_ctest_result)
if(NOT serveza_ctest_result EQUAL 0)
  message(FATAL_ERROR "Parent dependency consumer test failed with ${serveza_ctest_result}")
endif()
