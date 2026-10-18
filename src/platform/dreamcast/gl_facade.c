// Dreamcast platform layer: the GL calls the shared output code makes (see
// include/GL/glew.h). Buffers live in main memory; uniform buffers bound to a
// binding point are what the Dreamcast renderer reads its frame state from.

#include "gl_facade.h"

#include <GL/glew.h>
#include <trx/core/memory.h>
#include <trx/gl/utils.h>

#include <string.h>

#define M_MAX_BUFFERS 64

typedef struct {
    bool used;
    void *data;
    size_t size;
} M_BUFFER;

static M_BUFFER m_Buffers[M_MAX_BUFFERS + 1] = {};
static GLuint m_Bound[4] = {}; // array, element, uniform, other
static GLuint m_UniformBindings[DC_GL_MAX_UNIFORM_BINDINGS] = {};
static DC_GL_SCISSOR m_Scissor = {};
static float m_PolygonOffsetUnits = 0.0f;

TRX_GL_METRICS g_TRX_GL_Metrics = {};

static int32_t M_TargetSlot(const GLenum target)
{
    switch (target) {
    case GL_ARRAY_BUFFER:
        return 0;
    case GL_ELEMENT_ARRAY_BUFFER:
        return 1;
    case GL_UNIFORM_BUFFER:
        return 2;
    default:
        return 3;
    }
}

static M_BUFFER *M_GetBuffer(const GLuint name)
{
    if (name == 0 || name > M_MAX_BUFFERS || !m_Buffers[name].used) {
        return nullptr;
    }
    return &m_Buffers[name];
}

void glGenBuffers(const GLsizei n, GLuint *const buffers)
{
    for (GLsizei i = 0; i < n; i++) {
        buffers[i] = 0;
        for (GLuint name = 1; name <= M_MAX_BUFFERS; name++) {
            if (!m_Buffers[name].used) {
                m_Buffers[name] = (M_BUFFER) { .used = true };
                buffers[i] = name;
                break;
            }
        }
    }
}

void glDeleteBuffers(const GLsizei n, const GLuint *const buffers)
{
    for (GLsizei i = 0; i < n; i++) {
        M_BUFFER *const buffer = M_GetBuffer(buffers[i]);
        if (buffer != nullptr) {
            Memory_FreePointer(&buffer->data);
            *buffer = (M_BUFFER) {};
        }
    }
}

void glBindBuffer(const GLenum target, const GLuint buffer)
{
    m_Bound[M_TargetSlot(target)] = buffer;
}

void glBindBufferBase(
    const GLenum target, const GLuint index, const GLuint buffer)
{
    if (target == GL_UNIFORM_BUFFER && index < DC_GL_MAX_UNIFORM_BINDINGS) {
        m_UniformBindings[index] = buffer;
    }
    glBindBuffer(target, buffer);
}

void glBufferData(
    const GLenum target, const GLsizeiptr size, const void *const data,
    const GLenum usage)
{
    M_BUFFER *const buffer = M_GetBuffer(m_Bound[M_TargetSlot(target)]);
    if (buffer == nullptr) {
        return;
    }
    if (buffer->size != (size_t)size) {
        Memory_FreePointer(&buffer->data);
        buffer->data = size > 0 ? Memory_Alloc(size) : nullptr;
        buffer->size = size;
    }
    if (data != nullptr && size > 0) {
        memcpy(buffer->data, data, size);
    }
}

void glBufferSubData(
    const GLenum target, const GLintptr offset, const GLsizeiptr size,
    const void *const data)
{
    M_BUFFER *const buffer = M_GetBuffer(m_Bound[M_TargetSlot(target)]);
    if (buffer == nullptr || data == nullptr || offset < 0
        || (size_t)(offset + size) > buffer->size) {
        return;
    }
    memcpy((char *)buffer->data + offset, data, size);
}

void glEnable(const GLenum cap)
{
    if (cap == GL_SCISSOR_TEST) {
        m_Scissor.enabled = true;
    }
}

void glDisable(const GLenum cap)
{
    if (cap == GL_SCISSOR_TEST) {
        m_Scissor.enabled = false;
    }
}

void glScissor(
    const GLint x, const GLint y, const GLsizei width, const GLsizei height)
{
    m_Scissor.x = x;
    m_Scissor.y = y;
    m_Scissor.w = width;
    m_Scissor.h = height;
}

void glPolygonOffset(const GLfloat factor, const GLfloat units)
{
    m_PolygonOffsetUnits = units;
}

void glClear(const GLbitfield mask)
{
}

const void *DC_GL_GetUniformBlock(const int32_t binding, size_t *const size)
{
    if (binding < 0 || binding >= DC_GL_MAX_UNIFORM_BINDINGS) {
        return nullptr;
    }
    const M_BUFFER *const buffer = M_GetBuffer(m_UniformBindings[binding]);
    if (buffer == nullptr) {
        return nullptr;
    }
    if (size != nullptr) {
        *size = buffer->size;
    }
    return buffer->data;
}

DC_GL_SCISSOR DC_GL_GetScissor(void)
{
    return m_Scissor;
}

float DC_GL_GetPolygonOffsetUnits(void)
{
    return m_PolygonOffsetUnits;
}

void TRX_GL_CheckErrorAt(
    const char *const file, const int line, const char *const func)
{
}

void TRX_GL_Track_Reset(void)
{
    g_TRX_GL_Metrics = (TRX_GL_METRICS) {};
}

TRX_GL_METRICS TRX_GL_Track_GetMetrics(void)
{
    return g_TRX_GL_Metrics;
}
