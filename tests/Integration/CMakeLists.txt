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

add_executable(ServezaAdapterTests
  test_Adapters.cpp
  test_Lifecycle.cpp
  test_LifecycleEdges.cpp
  test_Listener.cpp
  test_Local.cpp
  test_Multithread.cpp
  test_Server.cpp
  test_Udp.cpp
)
target_compile_features(ServezaAdapterTests PRIVATE cxx_std_17)
target_link_libraries(ServezaAdapterTests PRIVATE serveza::serveza Boost::coroutine GTest::gtest_main)
serveza_enable_developer_options(ServezaAdapterTests)

gtest_discover_tests(ServezaAdapterTests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS integration TIMEOUT 15
)

if(SERVEZA_BUILD_CPP20)
  add_executable(ServezaCoroutineTests test_Coroutines.cpp)
  target_compile_features(ServezaCoroutineTests PRIVATE cxx_std_20)
  target_link_libraries(ServezaCoroutineTests PRIVATE serveza::serveza GTest::gtest_main)
  set_target_properties(ServezaCoroutineTests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  serveza_enable_developer_options(ServezaCoroutineTests)

  gtest_discover_tests(ServezaCoroutineTests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS integration TIMEOUT 15
  )
endif()

if(SERVEZA_BUILD_HTTP_TESTS)
  add_executable(ServezaHttpTests test_Http.cpp)
  target_compile_features(ServezaHttpTests PRIVATE cxx_std_17)
  target_link_libraries(ServezaHttpTests PRIVATE serveza::serveza Boost::beast GTest::gtest_main)
  if(SERVEZA_BUILD_TLS_TESTS)
    target_link_libraries(ServezaHttpTests PRIVATE OpenSSL::SSL)
    target_compile_definitions(ServezaHttpTests PRIVATE
      SERVEZA_TEST_HTTPS
      SERVEZA_TEST_TLS_CERTIFICATE_PATH="${PROJECT_SOURCE_DIR}/examples/TlsEcho/cert/server.crt"
      SERVEZA_TEST_TLS_PRIVATE_KEY_PATH="${PROJECT_SOURCE_DIR}/examples/TlsEcho/cert/server.key"
    )
  endif()
  serveza_enable_developer_options(ServezaHttpTests)

  gtest_discover_tests(ServezaHttpTests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS http TIMEOUT 15
  )
endif()

if(SERVEZA_BUILD_TLS_TESTS)
  add_executable(ServezaTlsTests test_Tls.cpp)
  target_compile_features(ServezaTlsTests PRIVATE cxx_std_17)
  target_link_libraries(ServezaTlsTests PRIVATE serveza::serveza OpenSSL::SSL GTest::gtest_main)
  target_compile_definitions(ServezaTlsTests PRIVATE
    SERVEZA_TEST_TLS_CERTIFICATE_PATH="${PROJECT_SOURCE_DIR}/examples/TlsEcho/cert/server.crt"
    SERVEZA_TEST_TLS_PRIVATE_KEY_PATH="${PROJECT_SOURCE_DIR}/examples/TlsEcho/cert/server.key"
  )
  serveza_enable_developer_options(ServezaTlsTests)

  gtest_discover_tests(ServezaTlsTests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS tls TIMEOUT 15
  )
endif()
