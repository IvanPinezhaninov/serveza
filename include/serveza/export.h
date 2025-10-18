/******************************************************************************
**
** Copyright (C) 2026 Ivan Pinezhaninov <ivan.pinezhaninov@gmail.com>
**
** This file is part of the serveza which can be found at
** https://github.com/IvanPinezhaninov/serveza/.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
** IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
** DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
** OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
** THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**
******************************************************************************/

#ifndef SERVEZA_EXPORT_H
#define SERVEZA_EXPORT_H

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(SERVEZA_LIBRARY_BUILD)
#define SERVEZA_API __declspec(dllexport)
#elif defined(SERVEZA_USE_SHARED)
#define SERVEZA_API __declspec(dllimport)
#else
#define SERVEZA_API
#endif
#elif defined(SERVEZA_LIBRARY_BUILD)
#define SERVEZA_API __attribute__((visibility("default")))
#else
#define SERVEZA_API
#endif

#endif // SERVEZA_EXPORT_H
