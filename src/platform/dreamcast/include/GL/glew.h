#pragma once

// Dreamcast: there is no GLEW, no OpenGL 3, and nothing draws through GL. The
// parts of the output module shared with the desktop still pass their data
// around as GL calls - mostly uniform buffers - so this declares the few they
// make. platform/dreamcast/gl_facade.c keeps the buffers in memory, where the
// Dreamcast renderer reads them; the rest of the calls set renderer state or
// do nothing.

#include <stddef.h>
#include <stdint.h>

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef int8_t GLbyte;
typedef uint8_t GLubyte;
typedef int16_t GLshort;
typedef uint16_t GLushort;
typedef int GLint;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef int64_t GLint64;
typedef uint64_t GLuint64;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NONE 0

#define GL_SCISSOR_TEST 0x0C11
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_CULL_FACE 0x0B44
#define GL_POLYGON_OFFSET_FILL 0x8037

#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_COLOR_BUFFER_BIT 0x00004000

#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8

#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601

void glGenBuffers(GLsizei n, GLuint *buffers);
void glDeleteBuffers(GLsizei n, const GLuint *buffers);
void glBindBuffer(GLenum target, GLuint buffer);
void glBindBufferBase(GLenum target, GLuint index, GLuint buffer);
void glBufferData(
    GLenum target, GLsizeiptr size, const void *data, GLenum usage);
void glBufferSubData(
    GLenum target, GLintptr offset, GLsizeiptr size, const void *data);

void glEnable(GLenum cap);
void glDisable(GLenum cap);
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void glPolygonOffset(GLfloat factor, GLfloat units);
void glClear(GLbitfield mask);
