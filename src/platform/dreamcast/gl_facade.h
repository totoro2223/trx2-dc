#pragma once

// What the Dreamcast renderer reads back from the GL facade (gl_facade.c).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Binding points the shared uniforms.c and lighting code bind their uniform
// buffers to.
#define DC_GL_BINDING_GENERAL 0
#define DC_GL_BINDING_MATRICES 1
#define DC_GL_BINDING_LIGHTS 2
#define DC_GL_BINDING_LIGHT_SOURCE 3
#define DC_GL_BINDING_FOG_BULBS 4
#define DC_GL_MAX_UNIFORM_BINDINGS 8

typedef struct {
    bool enabled;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} DC_GL_SCISSOR;

// The contents of the uniform buffer bound to a binding point, or nullptr.
const void *DC_GL_GetUniformBlock(int32_t binding, size_t *size);
DC_GL_SCISSOR DC_GL_GetScissor(void);
float DC_GL_GetPolygonOffsetUnits(void);
