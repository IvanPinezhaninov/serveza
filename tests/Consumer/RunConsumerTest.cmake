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

if(NOT DEFINED SERVEZA_BUILD_DIR OR SERVEZA_BUILD_DIR STREQUAL "")
  message(FATAL_ERROR "SERVEZA_BUILD_DIR is required")
endif()
if(NOT DEFINED SERVEZA_SOURCE_DIR OR SERVEZA_SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "SERVEZA_SOURCE_DIR is required")
endif()
if(NOT DEFINED SERVEZA_TEST_BOOST_INCLUDE_DIRS_ENCODED OR SERVEZA_TEST_BOOST_INCLUDE_DIRS_ENCODED STREQUAL "")
  message(FATAL_ERROR "SERVEZA_TEST_BOOST_INCLUDE_DIRS_ENCODED is required")
endif()
if(NOT DEFINED SERVEZA_TEST_BOOST_VERSION OR SERVEZA_TEST_BOOST_VERSION STREQUAL "")
  message(FATAL_ERROR "SERVEZA_TEST_BOOST_VERSION is required")
endif()
string(REPLACE "|" ";" SERVEZA_TEST_BOOST_INCLUDE_DIRS "${SERVEZA_TEST_BOOST_INCLUDE_DIRS_ENCODED}")

if(NOT DEFINED SERVEZA_TEST_VARIANT OR SERVEZA_TEST_VARIANT STREQUAL "")
  set(SERVEZA_TEST_VARIANT preprovided)
endif()
set(serveza_consumer_root "${SERVEZA_BUILD_DIR}/tests/Consumer-${SERVEZA_TEST_VARIANT}")
set(serveza_install_dir "${serveza_consumer_root}/install")
set(serveza_consumer_build_dir "${serveza_consumer_root}/build")
set(serveza_boost_package_dir "${serveza_consumer_root}/boost-package")
file(REMOVE_RECURSE "${serveza_install_dir}" "${serveza_consumer_build_dir}" "${serveza_boost_package_dir}")

set(serveza_install_command
  "${CMAKE_COMMAND}" --install "${SERVEZA_BUILD_DIR}" --prefix "${serveza_install_dir}"
)
if(DEFINED SERVEZA_TEST_CONFIG AND NOT SERVEZA_TEST_CONFIG STREQUAL "")
  list(APPEND serveza_install_command --config "${SERVEZA_TEST_CONFIG}")
endif()
execute_process(COMMAND ${serveza_install_command} RESULT_VARIABLE serveza_install_result)
if(NOT serveza_install_result EQUAL 0)
  message(FATAL_ERROR "Serveza installation failed with ${serveza_install_result}")
endif()

set(serveza_configure_command
  "${CMAKE_COMMAND}"
  -S "${SERVEZA_SOURCE_DIR}/tests/Consumer"
  -B "${serveza_consumer_build_dir}"
  -G "${SERVEZA_TEST_GENERATOR}"
  "-DCMAKE_PREFIX_PATH=${serveza_install_dir}"
  "-DSERVEZA_TEST_BOOST_INCLUDE_DIRS_ENCODED=${SERVEZA_TEST_BOOST_INCLUDE_DIRS_ENCODED}"
  "-DSERVEZA_TEST_PREDEFINE_BOOST=${SERVEZA_TEST_PREDEFINE_BOOST}"
  "-DSERVEZA_TEST_ENABLE_COVERAGE=${SERVEZA_TEST_ENABLE_COVERAGE}"
  "-DSERVEZA_TEST_ENABLE_SANITIZERS=${SERVEZA_TEST_ENABLE_SANITIZERS}"
  "-DSERVEZA_TEST_ENABLE_THREAD_SANITIZER=${SERVEZA_TEST_ENABLE_THREAD_SANITIZER}"
)
if(SERVEZA_TEST_DISCOVER_BOOST)
  set(serveza_boost_config_dir "${serveza_boost_package_dir}/lib/cmake/Boost-${SERVEZA_TEST_BOOST_VERSION}")
  file(MAKE_DIRECTORY "${serveza_boost_config_dir}")
  configure_file(
    "${SERVEZA_SOURCE_DIR}/tests/Consumer/BoostConfig.cmake.in"
    "${serveza_boost_config_dir}/BoostConfig.cmake"
    @ONLY
  )
  configure_file(
    "${SERVEZA_SOURCE_DIR}/tests/Consumer/BoostConfigVersion.cmake.in"
    "${serveza_boost_config_dir}/BoostConfigVersion.cmake"
    @ONLY
  )
  list(APPEND serveza_configure_command "-DBoost_DIR=${serveza_boost_config_dir}")
endif()
if(DEFINED SERVEZA_TEST_CXX_COMPILER AND NOT SERVEZA_TEST_CXX_COMPILER STREQUAL "")
  list(APPEND serveza_configure_command "-DCMAKE_CXX_COMPILER=${SERVEZA_TEST_CXX_COMPILER}")
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
  message(FATAL_ERROR "Installed consumer configure failed with ${serveza_configure_result}")
endif()

set(serveza_build_command "${CMAKE_COMMAND}" --build "${serveza_consumer_build_dir}")
if(DEFINED SERVEZA_TEST_CONFIG AND NOT SERVEZA_TEST_CONFIG STREQUAL "")
  list(APPEND serveza_build_command --config "${SERVEZA_TEST_CONFIG}")
endif()
execute_process(COMMAND ${serveza_build_command} RESULT_VARIABLE serveza_build_result)
if(NOT serveza_build_result EQUAL 0)
  message(FATAL_ERROR "Installed consumer build failed with ${serveza_build_result}")
endif()

set(serveza_ctest_command "${CMAKE_CTEST_COMMAND}" --test-dir "${serveza_consumer_build_dir}" --output-on-failure)
if(DEFINED SERVEZA_TEST_CONFIG AND NOT SERVEZA_TEST_CONFIG STREQUAL "")
  list(APPEND serveza_ctest_command -C "${SERVEZA_TEST_CONFIG}")
endif()
execute_process(COMMAND ${serveza_ctest_command} RESULT_VARIABLE serveza_ctest_result)
if(NOT serveza_ctest_result EQUAL 0)
  message(FATAL_ERROR "Installed consumer test failed with ${serveza_ctest_result}")
endif()
