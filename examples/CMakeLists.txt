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

add_subdirectory(CallbackEcho)
add_subdirectory(YieldEcho)
add_subdirectory(UdpEcho)

if(SERVEZA_BUILD_CPP20)
  add_subdirectory(CoroutineEcho)
endif()

if(UNIX)
  add_subdirectory(LocalEcho)
endif()

if(SERVEZA_BUILD_HTTP_EXAMPLES)
  add_subdirectory(HttpEcho)
endif()

if(SERVEZA_BUILD_TLS_EXAMPLES)
  add_subdirectory(TlsEcho)
endif()

if(SERVEZA_BUILD_HTTP_EXAMPLES AND SERVEZA_BUILD_TLS_EXAMPLES)
  add_subdirectory(HttpsEcho)
endif()
