#pragma once

// Dreamcast: there is no GLEW and no OpenGL 3. The output headers name GL
// types in their declarations, so this provides those from GLdc. Nothing
// outside platform/dreamcast calls into GL on this platform.
#include <GL/gl.h>
#include <GL/glext.h>

#include <stddef.h>
#include <stdint.h>

#ifndef GL_VERSION_2_0
typedef char GLchar;
#endif
#ifndef GL_VERSION_3_2
typedef int64_t GLint64;
typedef uint64_t GLuint64;
typedef struct __GLsync *GLsync;
#endif
#ifndef GL_VERSION_1_5
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
#endif
