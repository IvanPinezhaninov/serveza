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

include_guard(GLOBAL)

function(serveza_add_doxygen_target)
  find_package(Doxygen REQUIRED)

  set(DOXYGEN_PROJECT_NAME "Serveza")
  set(DOXYGEN_PROJECT_NUMBER "${PROJECT_VERSION}")
  set(DOXYGEN_EXTRACT_ALL NO)
  set(DOXYGEN_EXCLUDE_PATTERNS "*/detail/*")
  set(DOXYGEN_GENERATE_TREEVIEW YES)
  set(DOXYGEN_HTML_OUTPUT api)
  set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/docs")
  set(DOXYGEN_QUIET YES)
  set(DOXYGEN_WARN_AS_ERROR FAIL_ON_WARNINGS)
  set(DOXYGEN_WARN_IF_UNDOCUMENTED NO)

  doxygen_add_docs(ServezaDocs ALL "${CMAKE_CURRENT_SOURCE_DIR}/include/serveza"
                   COMMENT "Generating Serveza API documentation")
endfunction()
