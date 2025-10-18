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

add_executable(ServezaHttpsEcho src/main.cpp)
target_link_libraries(ServezaHttpsEcho PRIVATE serveza::serveza Boost::beast OpenSSL::SSL)
target_compile_definitions(ServezaHttpsEcho PRIVATE
  SERVEZA_HTTPS_CERTIFICATE_PATH="${PROJECT_SOURCE_DIR}/examples/TlsEcho/cert/server.crt"
  SERVEZA_HTTPS_PRIVATE_KEY_PATH="${PROJECT_SOURCE_DIR}/examples/TlsEcho/cert/server.key"
)
serveza_enable_developer_options(ServezaHttpsEcho)
