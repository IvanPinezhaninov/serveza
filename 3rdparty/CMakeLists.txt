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

function(serveza_promote_imported_target target_name)
  if(TARGET ${target_name})
    get_target_property(serveza_imported_target ${target_name} IMPORTED)
    if(serveza_imported_target)
      set_property(TARGET ${target_name} PROPERTY IMPORTED_GLOBAL ON)
    endif()
  endif()
endfunction()

add_subdirectory(boost)

if((SERVEZA_BUILD_EXAMPLES AND SERVEZA_BUILD_TLS_EXAMPLES) OR
   (SERVEZA_BUILD_TESTS AND SERVEZA_BUILD_TLS_TESTS))
  add_subdirectory(openssl)
endif()

if(SERVEZA_BUILD_TESTS)
  add_subdirectory(gtest)
endif()
