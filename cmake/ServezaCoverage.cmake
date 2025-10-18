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

function(serveza_add_coverage_target)
  find_program(GCOVR_EXECUTABLE NAMES gcovr REQUIRED)

  set(serveza_coverage_directory "${PROJECT_BINARY_DIR}/coverage")
  set(serveza_coverage_inputs
    "${PROJECT_BINARY_DIR}/CMakeFiles/serveza.dir"
    "${PROJECT_BINARY_DIR}/tests/Unit/CMakeFiles/ServezaUnitTests.dir"
    "${PROJECT_BINARY_DIR}/tests/Integration/CMakeFiles/ServezaAdapterTests.dir"
  )
  set(serveza_coverage_targets ServezaHeaderTests ServezaUnitTests ServezaAdapterTests)

  if(TARGET ServezaCoroutineTests)
    list(APPEND serveza_coverage_inputs
      "${PROJECT_BINARY_DIR}/tests/Integration/CMakeFiles/ServezaCoroutineTests.dir"
    )
    list(APPEND serveza_coverage_targets ServezaCoroutineTests)
  endif()
  if(TARGET ServezaHttpTests)
    list(APPEND serveza_coverage_inputs
      "${PROJECT_BINARY_DIR}/tests/Integration/CMakeFiles/ServezaHttpTests.dir"
    )
    list(APPEND serveza_coverage_targets ServezaHttpTests)
  endif()
  if(TARGET ServezaTlsTests)
    list(APPEND serveza_coverage_inputs
      "${PROJECT_BINARY_DIR}/tests/Integration/CMakeFiles/ServezaTlsTests.dir"
    )
    list(APPEND serveza_coverage_targets ServezaTlsTests)
  endif()

  add_custom_target(
    ServezaCoverage
    COMMAND ${CMAKE_COMMAND}
            -DCOVERAGE_BUILD_DIRECTORY="${PROJECT_BINARY_DIR}"
            -P "${PROJECT_SOURCE_DIR}/cmake/ClearCoverageData.cmake"
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${PROJECT_BINARY_DIR}" --output-on-failure -C $<CONFIG> -j1
    COMMAND ${CMAKE_COMMAND} -E make_directory "${serveza_coverage_directory}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${serveza_coverage_directory}/raw"
    COMMAND ${GCOVR_EXECUTABLE}
            --root "${PROJECT_SOURCE_DIR}"
            ${serveza_coverage_inputs}
            --filter "^src/"
            --filter "^include/serveza/"
            --merge-lines
            --print-summary
            --fail-under-branch 55
            --fail-under-function 60
            --fail-under-line 98
            --txt "${serveza_coverage_directory}/summary.txt"
            --xml "${serveza_coverage_directory}/coverage.xml"
            --html-details "${serveza_coverage_directory}/index.html"
    COMMAND ${GCOVR_EXECUTABLE}
            --root "${PROJECT_SOURCE_DIR}"
            ${serveza_coverage_inputs}
            --filter "^src/"
            --filter "^include/serveza/"
            --merge-lines
            --no-markers
            --print-summary
            --fail-under-branch 53
            --fail-under-function 60
            --fail-under-line 92
            --txt "${serveza_coverage_directory}/raw/summary.txt"
            --xml "${serveza_coverage_directory}/raw/coverage.xml"
            --html-details "${serveza_coverage_directory}/raw/index.html"
    DEPENDS ${serveza_coverage_targets}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Generating source-aware and raw Serveza coverage reports"
    USES_TERMINAL
  )
endfunction()
