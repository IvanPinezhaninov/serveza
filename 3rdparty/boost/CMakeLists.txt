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

set(serveza_boost_components asio ${SERVEZA_EXTRA_BOOST_COMPONENTS})
if(SERVEZA_BUILD_EXAMPLES OR SERVEZA_BUILD_TESTS)
  list(APPEND serveza_boost_components coroutine)
endif()
if((SERVEZA_BUILD_EXAMPLES AND SERVEZA_BUILD_HTTP_EXAMPLES) OR
   (SERVEZA_BUILD_TESTS AND SERVEZA_BUILD_HTTP_TESTS))
  list(APPEND serveza_boost_components beast)
endif()
list(REMOVE_DUPLICATES serveza_boost_components)

set(serveza_boost_known_targets headers boost ${serveza_boost_components})
set(serveza_boost_found_in_tree OFF)
foreach(serveza_boost_target IN LISTS serveza_boost_known_targets)
  if(TARGET Boost::${serveza_boost_target})
    set(serveza_boost_found_in_tree ON)
  endif()
endforeach()

if(serveza_boost_found_in_tree)
  message(STATUS "Using Boost targets provided by the parent project")
  if(Boost_VERSION)
    set(serveza_resolved_boost_version "${Boost_VERSION}")
  else()
    set(serveza_resolved_boost_version "${SERVEZA_BOOST_MIN_VERSION}")
  endif()
elseif(SERVEZA_USE_BUNDLED_BOOST)
  include(FetchContent)

  set(BOOST_INCLUDE_LIBRARIES ${serveza_boost_components})
  set(BOOST_SKIP_INSTALL_RULES ON)
  set(BOOST_INSTALL_LAYOUT system)
  set(BUILD_SHARED_LIBS OFF)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  if(SERVEZA_ENABLE_THREAD_SANITIZER)
    set(BOOST_CONTEXT_IMPLEMENTATION ucontext CACHE STRING "Boost.Context implementation" FORCE)
  endif()

  FetchContent_Declare(
    serveza_boost
    URL https://github.com/boostorg/boost/releases/download/boost-${SERVEZA_BUNDLED_BOOST_VERSION}/boost-${SERVEZA_BUNDLED_BOOST_VERSION}-cmake.tar.xz
    URL_HASH SHA256=9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    EXCLUDE_FROM_ALL
    SYSTEM
  )
  FetchContent_MakeAvailable(serveza_boost)
  message(STATUS "Using bundled Boost ${SERVEZA_BUNDLED_BOOST_VERSION}")
  set(serveza_resolved_boost_version "${SERVEZA_BUNDLED_BOOST_VERSION}")

  if(SERVEZA_ENABLE_THREAD_SANITIZER AND TARGET boost_context)
    target_compile_definitions(boost_context PUBLIC BOOST_USE_TSAN)
  endif()
else()
  set(serveza_boost_system_components ${serveza_boost_components})
  list(REMOVE_ITEM serveza_boost_system_components asio beast headers boost)
  if(serveza_boost_system_components)
    find_package(Boost ${SERVEZA_BOOST_MIN_VERSION} REQUIRED COMPONENTS ${serveza_boost_system_components})
  else()
    find_package(Boost ${SERVEZA_BOOST_MIN_VERSION} REQUIRED)
  endif()
  foreach(serveza_boost_target IN LISTS serveza_boost_known_targets)
    serveza_promote_imported_target(Boost::${serveza_boost_target})
  endforeach()
  message(STATUS "Using system Boost ${Boost_VERSION}")
  set(serveza_resolved_boost_version "${Boost_VERSION}")
endif()

set(SERVEZA_RESOLVED_BOOST_VERSION "${serveza_resolved_boost_version}" CACHE INTERNAL
    "Boost version used to build Serveza" FORCE)

foreach(serveza_boost_header_component asio beast)
  if(serveza_boost_header_component IN_LIST serveza_boost_components AND
     NOT TARGET Boost::${serveza_boost_header_component})
    if(TARGET Boost::headers)
      set(serveza_boost_headers_target Boost::headers)
    elseif(TARGET Boost::boost)
      set(serveza_boost_headers_target Boost::boost)
    else()
      message(FATAL_ERROR "The selected Boost dependency must provide Boost headers")
    endif()
    add_library(Boost::${serveza_boost_header_component} INTERFACE IMPORTED GLOBAL)
    set_property(TARGET Boost::${serveza_boost_header_component}
                 PROPERTY INTERFACE_LINK_LIBRARIES ${serveza_boost_headers_target})
  endif()
endforeach()

foreach(serveza_required_boost_component IN LISTS serveza_boost_components)
  if(NOT TARGET Boost::${serveza_required_boost_component})
    message(FATAL_ERROR "The selected Boost dependency must provide Boost::${serveza_required_boost_component}")
  endif()
endforeach()

if(WIN32 AND TARGET boost_mpl)
  target_compile_definitions(boost_mpl INTERFACE BOOST_MPL_CFG_NO_PREPROCESSED_HEADERS)
endif()
