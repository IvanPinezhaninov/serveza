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

cmake_minimum_required(VERSION 3.5)

project(serveza_support LANGUAGES CXX VERSION 1.0.0)

find_package(OpenSSL REQUIRED)

set(HEADERS
  include/serveza/tls_cert_utils.h
)

set(SOURCES
  src/tls_cert_utils.cpp
)

add_library(${PROJECT_NAME}
  STATIC
    ${HEADERS}
    ${SOURCES}
)

add_library("serveza::support" ALIAS ${PROJECT_NAME})

set_target_properties(${PROJECT_NAME}
  PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(${PROJECT_NAME}
  PUBLIC
    include
  PRIVATE
    src
)

target_link_libraries(${PROJECT_NAME}
  PUBLIC
    OpenSSL::SSL
)

if (MSVC)
  target_compile_options(${PROJECT_NAME}
    PRIVATE
      /bigobj
  )
endif()
