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
# OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
# OR OTHER DEALINGS IN THE SOFTWARE.
#
#============================================================================

include_guard(GLOBAL)

if(BUILD_SHARED_LIBS)
  set(serveza_package_linkage shared)
else()
  set(serveza_package_linkage static)
endif()

string(TOLOWER "${CMAKE_SYSTEM_NAME}" serveza_package_system)
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" serveza_package_processor)

set(CPACK_PACKAGE_NAME serveza)
set(CPACK_PACKAGE_VENDOR "Ivan Pinezhaninov")
set(CPACK_PACKAGE_CONTACT "ivan.pinezhaninov@gmail.com")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "C++17 CompletionToken-based asynchronous server kernel")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/IvanPinezhaninov/serveza")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "serveza-${PROJECT_VERSION}-${serveza_package_system}-${serveza_package_processor}-${serveza_package_linkage}")
set(CPACK_PACKAGE_DIRECTORY "${PROJECT_BINARY_DIR}/packages")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${PROJECT_SOURCE_DIR}/README.md")
set(CPACK_GENERATOR TGZ ZIP)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND CPACK_GENERATOR DEB RPM)

  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Ivan Pinezhaninov <ivan.pinezhaninov@gmail.com>")
  set(CPACK_DEBIAN_PACKAGE_SECTION libdevel)
  set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "libboost-dev (>= ${SERVEZA_RESOLVED_BOOST_VERSION})")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.deb")

  set(CPACK_RPM_PACKAGE_LICENSE MIT)
  set(CPACK_RPM_PACKAGE_GROUP "Development/Libraries")
  set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}")
  set(CPACK_RPM_PACKAGE_REQUIRES "boost-devel >= ${SERVEZA_RESOLVED_BOOST_VERSION}")
  set(CPACK_RPM_FILE_NAME "${CPACK_PACKAGE_FILE_NAME}.rpm")
endif()

include(CPack)
