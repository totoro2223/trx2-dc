// Dreamcast UI source: trx/game/output/sources/ui.c drawn through the
// PowerVR. Sprites (text, icons), flat and gradient quads and rings are kept
// as screen-space primitives through the frame and drawn in the UI pass, in
// the translucent list, at the depth their z gives them. Models (the
// inventory) go through the objects source, over the flat interface.

#include "cel.h"
#include "pvr.h"

#include <platform/dreamcast/stub.h>
#include <trx/config.h>
#include <trx/core/math/const.h>
#include <trx/core/math/func.h>
#include <trx/core/memory.h>
#include <trx/core/utils.h>
#include <trx/core/vector.h>
#include <trx/game/matrix.h>
#include <trx/game/objects/common.h>
#include <trx/game/output.h>
#include <trx/game/output/const.h>
#include <trx/game/output/func.h>
#include <trx/game/output/scene_compositor.h>
#include <trx/game/output/sources/objects.h>
#include <trx/game/output/sources/ui.h>
#include <trx/game/output/textures.h>
#include <trx/game/output/uniforms.h>
#include <trx/game/viewport.h>
#include <trx/version.h>

#include <math.h>

#define M_RING_SEGMENTS 32

typedef struct {
    // Strip order: top left, top right, bottom left, bottom right; a
    // triangle uses the first three.
    DC_PVR_VERTEX v[4];
    int16_t page;
    bool is_triangle;
    // Drawn smaller than its texels: from the page at half size.
    bool half;
} M_PRIM;

typedef struct {
    SCENE_SOURCE source;
    const SCENE_SOURCE *objects_source;
    VECTOR *prims; // M_PRIM
    VECTOR *meshes; // OUTPUT_UI_MESH
    bool binocular_mask;
} M_PRIV;

static M_PRIV m_Priv = {};

// What a model is lit by where the script gives no light.
static const OUTPUT_UI_MESH_LIGHT m_PlainLight = {
    .ambient = { 1.0f, 1.0f, 1.0f },
};

// ----------------------------------------------------------------------------
// Models (the inventory ring, the passport, held pickups): posed and lit as
// trx/game/output/sources/ui.c does, then drawn by the objects source with
// the UI's projection, orthographic on the canvas or the model's own camera's
// in a world of its own.

static float M_GetMeshScale(
    const VIEWPORT_RECT rect, const ANIM_FRAME *const frame)
{
    const XYZ_F obj_size = {
        .x = MAX(1, frame->bounds.max.x - frame->bounds.min.x),
        .y = MAX(1, frame->bounds.max.y - frame->bounds.min.y),
        .z = MAX(1, frame->bounds.max.z - frame->bounds.min.z),
    };
    const float ref_scale = rect.w / 200.0f;
    const float perfect_fit_scale = MIN3(
        rect.w / obj_size.x, rect.h / obj_size.y, rect.w / obj_size.z);
    return (ref_scale + perfect_fit_scale) / 2.0f;
}

static int32_t M_GetGrey(const RGB_F color)
{
    return lroundf((color.r + color.g + color.b) / 3.0f * 255.0f);
}

static XYZ_32 M_Normalize(const XYZ_32 dir)
{
    const float length = sqrtf(
        (float)dir.x * dir.x + (float)dir.y * dir.y + (float)dir.z * dir.z);
    if (length == 0.0f) {
        return dir;
    }
    const float scale = (1 << W2V_SHIFT) / length;
    return (XYZ_32) {
        .x = (int32_t)lroundf(dir.x * scale),
        .y = (int32_t)lroundf(dir.y * scale),
        .z = (int32_t)lroundf(dir.z * scale),
    };
}

static XYZ_32 M_RotateToView(const XYZ_32 dir)
{
    const MATRIX *const m = &g_ViewMatrix;
    return (XYZ_32) {
        .x = (int32_t)((m->_00 * dir.x + m->_01 * dir.y + m->_02 * dir.z)
                       >> W2V_SHIFT),
        .y = (int32_t)((m->_10 * dir.x + m->_11 * dir.y + m->_12 * dir.z)
                       >> W2V_SHIFT),
        .z = (int32_t)((m->_20 * dir.x + m->_21 * dir.y + m->_22 * dir.z)
                       >> W2V_SHIFT),
    };
}

static void M_ApplyLight(
    const OUTPUT_UI_MESH_LIGHT *const light, const RGB_F *const mesh_color)
{
    if (g_TRVersion == 4) {
        Output_CalculateStaticLightRGB_F(light->ambient);
    } else if (g_TRVersion == 3) {
        RGB_F colors[3] = {};
        XYZ_32 dirs[3] = {};
        for (int32_t i = 0; i < light->light_count; i++) {
            colors[i] =
                mesh_color != nullptr ? *mesh_color : light->lights[i].color;
            dirs[i] = light->lights[i].dir;
        }
        Output_SetTR3Light(light->ambient, colors, dirs);
    } else {
        Output_SetLightAdder(
            SHADE_NEUTRAL * 2 - M_GetGrey(light->ambient) * 32);
        const int32_t grey =
            light->light_count > 0 ? M_GetGrey(light->lights[0].color) : 0;
        if (grey > 0) {
            Output_SetLightDivider((1 << W2V_SHIFT) * 256 / grey);
            Output_SetLightVectorView(M_Normalize(light->lights[0].dir));
        } else {
            Output_SetLightVectorView((XYZ_32) {});
        }
    }
}

static void M_DrawColoredMeshes(
    const OUTPUT_UI_MESH *const mesh, const ANIM_FRAME *const frame,
    const OUTPUT_UI_MESH_LIGHT *const light)
{
    const OBJECT *const obj = mesh->object;
    const XYZ_16 *const rots = Anim_GetFrameRots(frame);
    Matrix_Push();
    for (int32_t i = 0; i < obj->mesh_count && i < 32; i++) {
        if (i > 0) {
            const ANIM_BONE *const bone = Object_GetBone(obj, i - 1);
            if (bone->matrix_pop) {
                Matrix_Pop();
            }
            if (bone->matrix_push) {
                Matrix_Push();
            }
            Matrix_TranslateRel32(bone->pos);
            Matrix_Rot16(rots[i]);
        }
        const uint32_t bit = 1u << i;
        if ((mesh->mesh_mask & bit) == 0) {
            continue;
        }
        M_ApplyLight(
            light, (light->lit_mask & bit) != 0 ? &light->colors[i] : nullptr);
        Object_DrawMesh(obj->mesh_idx + i, 0, false);
    }
    Matrix_Pop();
}

static void M_PoseOnCanvas(
    const OUTPUT_UI_MESH *const mesh, const ANIM_FRAME *const frame)
{
    const VIEWPORT_RECT rect = mesh->rect;
    const float scale = M_GetMeshScale(rect, frame);
    const XYZ_32 origin = {
        .x = rect.x + rect.w / 2,
        .y = rect.y + rect.h / 2,
        .z = (OUTPUT_UI_NEAR_Z + OUTPUT_UI_FAR_Z) / 2,
    };
    // The lighting needs a world-to-view matrix.
    Matrix_LookAt(
        origin.x, origin.y, origin.z - WALL_L, origin.x, origin.y, origin.z, 0);

    Matrix_PushUnit();
    Matrix_TranslateSet32(origin);
    Matrix_RotX(mesh->tilt);
    if (mesh->held) {
        Matrix_RotX(DEG_1 * 15);
        Matrix_RotY(-DEG_180);
    }
    Matrix_Rot16(mesh->rot);
    Matrix_Scale((1 << W2V_SHIFT) * scale);
    Matrix_TranslateRel16(frame->offset);
    Matrix_TranslateRel32((XYZ_32) {
        .x = -(frame->bounds.min.x + frame->bounds.max.x) / 2,
        .y = -(frame->bounds.min.y + frame->bounds.max.y) / 2,
        .z = -(frame->bounds.min.z + frame->bounds.max.z) / 2,
    });
    Matrix_Rot16(Anim_GetFrameRots(frame)[0]);
}

static void M_PoseInWorld(const OUTPUT_UI_MESH_WORLD *const world)
{
    Viewport_AlterFOV(world->fov, FOV_MODE_PASSPORT);
    Output_ApplyFOV();
    int16_t angles[2];
    Math_GetVectorAngles(
        world->camera_target.x - world->camera_pos.x,
        world->camera_target.y - world->camera_pos.y,
        world->camera_target.z - world->camera_pos.z, angles);
    const XYZ_16 view_rot = { .x = angles[1], .y = angles[0], .z = 0 };
    Matrix_GenerateW2V(&world->camera_pos, &view_rot);
    Output_Uniforms_UploadViewMatrix(Output_GetUniforms(), &g_ViewMatrix);

    Matrix_Push();
    Matrix_TranslateAbs32((XYZ_32) {});
    for (int32_t i = 0; i < world->step_count; i++) {
        const XYZ_32 vec = world->steps[i].vec;
        switch (world->steps[i].kind) {
        case OUTPUT_UI_MESH_STEP_TURN:
            Matrix_Rot16((XYZ_16) {
                .x = (int16_t)vec.x,
                .y = (int16_t)vec.y,
                .z = (int16_t)vec.z,
            });
            break;
        case OUTPUT_UI_MESH_STEP_SCALE:
            Matrix_Scale((1 << W2V_SHIFT) * vec.x / 1024);
            break;
        case OUTPUT_UI_MESH_STEP_MOVE:
            Matrix_TranslateRel32(vec);
            break;
        }
    }
}

static const ANIM_FRAME *M_GetWorldFrame(
    const OBJECT *const object, const int32_t frame)
{
    const ANIM *const anim = Object_GetAnim(object, 0);
    if (frame < 0 || frame > anim->frame_end - anim->frame_base) {
        return nullptr;
    }
    return &object->frame_base[frame];
}

static void M_DrawWorldFrame(const OUTPUT_UI_MESH *const mesh)
{
    const OUTPUT_UI_MESH_WORLD *const world = mesh->world;
    const OBJECT *const obj = mesh->object;
    const ANIM_FRAME *const frame1 = M_GetWorldFrame(obj, world->frame);
    const ANIM_FRAME *frame2 = M_GetWorldFrame(obj, world->next_frame);
    if (frame1 == nullptr) {
        return;
    }
    if (frame2 == nullptr) {
        frame2 = frame1;
    }

    for (int32_t i = 0; i < world->bone_count; i++) {
        const int32_t bone = world->bones[i].bone;
        if (bone >= 0 && bone < obj->mesh_count - 1) {
            ANIM_BONE *const anim_bone = Object_GetBone(obj, bone);
            anim_bone->rot.x |= world->bones[i].axes.x;
            anim_bone->rot.y |= world->bones[i].axes.y;
            anim_bone->rot.z |= world->bones[i].axes.z;
        }
    }

    int16_t extra_rotation[(OUTPUT_UI_MESH_BONES + 32) * 3] = {};
    int32_t count = 0;
    for (int32_t bone = 0; bone < obj->mesh_count - 1 && bone < 32; bone++) {
        const ANIM_BONE *const anim_bone = Object_GetBone(obj, bone);
        XYZ_16 rot = {};
        for (int32_t i = 0; i < world->bone_count; i++) {
            if (world->bones[i].bone == bone) {
                rot = world->bones[i].rot;
            }
        }
        if (anim_bone->rot.y) {
            extra_rotation[count++] = rot.y;
        }
        if (anim_bone->rot.x) {
            extra_rotation[count++] = rot.x;
        }
        if (anim_bone->rot.z) {
            extra_rotation[count++] = rot.z;
        }
    }

    const int32_t rate = 1024;
    int32_t frac = (int32_t)(world->blend * rate + 0.5f);
    CLAMP(frac, 0, rate);
    if (world->at_pivot) {
        Object_DrawInterpolatedObjectAtPivot(
            obj, mesh->mesh_mask, extra_rotation, frame1, frame2, frac, rate);
    } else {
        Object_DrawInterpolatedObject(
            obj, mesh->mesh_mask, extra_rotation, frame1, frame2, frac, rate);
    }
}

static void M_DrawMeshes(const M_PRIV *const p)
{
    const SCENE_SOURCE *const objects = p->objects_source;
    // A model on the canvas is lit by its own fog, not by the room's.
    const int32_t old_fog_start = Output_GetFogStart();
    const int32_t old_fog_end = Output_GetFogEnd();
    Output_SetFogStart(20 * WALL_L);
    Output_SetFogEnd(100 * WALL_L);
    DC_Cel_SetAllowed(false);

    for (int32_t i = 0; i < p->meshes->count; i++) {
        const OUTPUT_UI_MESH *const mesh = Vector_Get(p->meshes, i);
        const ANIM_FRAME *const frame =
            Object_GetAnim(mesh->object, 0)->frame_ptr;
        if (frame == nullptr) {
            continue;
        }
        if (objects->render_begin != nullptr) {
            objects->render_begin(objects);
        }

        const int16_t old_fov = Viewport_GetSystemFOV();
        const FOV_MODE old_fov_mode = Viewport_GetFOVMode();
        OUTPUT_UI_MESH_LIGHT light =
            mesh->light != nullptr ? *mesh->light : m_PlainLight;
        if (mesh->world != nullptr) {
            M_PoseInWorld(mesh->world);
            for (int32_t j = 0; j < light.light_count; j++) {
                light.lights[j].dir = M_RotateToView(light.lights[j].dir);
            }
        } else {
            Output_Uniforms_UploadOrthoMatrix(Output_GetUniforms());
            M_PoseOnCanvas(mesh, frame);
        }
        if (light.lit_mask != 0 && g_TRVersion == 3) {
            const ANIM_FRAME *posed = frame;
            if (mesh->world != nullptr) {
                posed = M_GetWorldFrame(mesh->object, mesh->world->frame);
                if (posed == nullptr) {
                    posed = frame;
                }
                Matrix_TranslateRel16(posed->offset);
                Matrix_Rot16(Anim_GetFrameRots(posed)[0]);
            }
            M_DrawColoredMeshes(mesh, posed, &light);
        } else if (mesh->world != nullptr) {
            M_ApplyLight(&light, nullptr);
            M_DrawWorldFrame(mesh);
        } else {
            M_ApplyLight(&light, nullptr);
            Object_DrawStaticObject(mesh->object, frame, mesh->mesh_mask);
        }
        Matrix_Pop();

        objects->render_pass(objects, SCENE_PASS_OPAQUE);
        objects->render_pass(objects, SCENE_PASS_BLEND_SUB);
        objects->render_pass(objects, SCENE_PASS_BLEND_ADD);
        if (objects->render_end != nullptr) {
            objects->render_end(objects);
        }
        if (mesh->world != nullptr) {
            Viewport_AlterFOV(old_fov, old_fov_mode);
            Output_ApplyFOV();
            Output_Uniforms_UploadOrthoMatrix(Output_GetUniforms());
        }
    }

    Output_SetFogStart(old_fog_start);
    Output_SetFogEnd(old_fog_end);
    DC_Cel_SetAllowed(true);
}

static uint32_t M_PackF(const RGBA_F c)
{
    const float a = c.a < 0.0f ? 0.0f : (c.a > 1.0f ? 1.0f : c.a);
    const float r = c.r < 0.0f ? 0.0f : (c.r > 1.0f ? 1.0f : c.r);
    const float g = c.g < 0.0f ? 0.0f : (c.g > 1.0f ? 1.0f : c.g);
    const float b = c.b < 0.0f ? 0.0f : (c.b > 1.0f ? 1.0f : c.b);
    return ((uint32_t)(a * 255.0f) << 24) | ((uint32_t)(r * 255.0f) << 16)
        | ((uint32_t)(g * 255.0f) << 8) | (uint32_t)(b * 255.0f);
}

static uint32_t M_Pack8(const RGBA_8888 c)
{
    return ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16)
        | ((uint32_t)c.g << 8) | c.b;
}

// UI coordinates are in the UI viewport; the screen is 640x480.
static void M_ToScreen(
    const float x, const float y, const float z, DC_PVR_VERTEX *const out)
{
    const VIEWPORT_RECT ui = Viewport_GetRect(VIEWPORT_UI);
    const float sx = ui.width > 0 ? 640.0f / ui.width : 1.0f;
    const float sy = ui.height > 0 ? 480.0f / ui.height : 1.0f;
    out->x = x * sx;
    out->y = y * sy;
    // A smaller z lies nearer, as the desktop's orthographic projection
    // has it; the PowerVR wants the nearer bigger.
    out->z = 1.0f - (z - OUTPUT_UI_NEAR_Z) / (OUTPUT_UI_FAR_Z - OUTPUT_UI_NEAR_Z);
}

static void M_RenderBegin(const SCENE_SOURCE *const source)
{
    M_PRIV *const p = &m_Priv;
    Vector_Clear(p->prims);
    Vector_Clear(p->meshes);
    p->binocular_mask = false;
}

static void M_RenderPass(const SCENE_SOURCE *const source, const SCENE_PASS pass)
{
    M_PRIV *const p = &m_Priv;
    if (pass != SCENE_PASS_UI) {
        return;
    }
    // At 320x240 filtering blurs the text past reading.
    DC_PVR_SetFilter(
        g_Config.rendering.ui_filter == TEXTURE_FILTER_BILINEAR
        && !DC_PVR_IsLowRes());
    for (int32_t i = 0; i < p->prims->count; i++) {
        M_PRIM *const prim = Vector_Get(p->prims, i);
        for (int32_t j = 0; j < 4; j++) {
            prim->v[j].z = DC_PVR_Depth2D(prim->v[j].z);
        }
        DC_PVR_SetHalfPages(prim->half);
        DC_PVR_SetState(DC_BLEND_ALPHA, prim->page, true);
        if (prim->is_triangle) {
            DC_PVR_Triangle(&prim->v[0], &prim->v[1], &prim->v[2]);
        } else {
            DC_PVR_Quad(&prim->v[0], &prim->v[1], &prim->v[2], &prim->v[3]);
        }
    }
    DC_PVR_SetHalfPages(false);

    // The models go over the flat interface.
    if (p->meshes->count > 0) {
        DC_PVR_NextDepthLayer();
        M_DrawMeshes(p);
    }
}

static bool M_IsDirty(const SCENE_SOURCE *const source, const SCENE_PASS pass)
{
    return pass == SCENE_PASS_UI
        && (m_Priv.prims->count > 0 || m_Priv.meshes->count > 0);
}

void OutputSource_UI_Init(void)
{
    M_PRIV *const p = &m_Priv;
    p->prims = Vector_CreateAtCapacity(sizeof(M_PRIM), 256);
    p->meshes = Vector_Create(sizeof(OUTPUT_UI_MESH));
    p->objects_source = OutputSource_Objects_GetSource();
    p->source = (SCENE_SOURCE) {
        .render_begin = M_RenderBegin,
        .render_pass = M_RenderPass,
        .is_dirty = M_IsDirty,
        .priv = p,
    };
    SceneCompositor_AddSource(&p->source);
}

void OutputSource_UI_Shutdown(void)
{
    M_PRIV *const p = &m_Priv;
    if (p->prims != nullptr) {
        Vector_Free(p->prims);
        p->prims = nullptr;
    }
    if (p->meshes != nullptr) {
        Vector_Free(p->meshes);
        p->meshes = nullptr;
    }
}

void OutputSource_UI_StageMesh(const OUTPUT_UI_MESH mesh)
{
    Vector_Add(m_Priv.meshes, &mesh);
}

void OutputSource_UI_StageBinocularMask(void)
{
    DC_STUB();
    m_Priv.binocular_mask = true;
}

// At 320x240 (screen coordinates still 640x480, halved): puts a sprite's
// edges on whole pixels and, when it is drawn near a whole number of pixels
// per texel, at exactly that many, so that every texel is as wide as the
// next. Says whether it is drawn smaller than its texels.
static bool M_SnapLowRes(float *const lo, float *const hi, const uint16_t size)
{
    const float texels = size / 256.0f;
    const float p0 = roundf(*lo * 0.5f);
    float pixels = (*hi - *lo) * 0.5f;
    const float per_texel = roundf(pixels / texels);
    if (per_texel >= 1.0f && fabsf(pixels / texels - per_texel) < 0.25f) {
        pixels = per_texel * roundf(texels);
    } else {
        pixels = roundf(pixels);
    }
    *lo = p0 * 2.0f;
    *hi = (p0 + pixels) * 2.0f;
    return pixels < texels;
}

void OutputSource_UI_StageSprite(const OUTPUT_UI_SPRITE sprite)
{
    const SPRITE_TEXTURE *const tex = Output_GetSpriteTexture(sprite.sprite_idx);
    if (tex == nullptr) {
        return;
    }
    const float u0 = (tex->offset & 0xFF) / 256.0f;
    const float v0 = (tex->offset >> 8) / 256.0f;
    const float u1 = u0 + tex->width / 65536.0f;
    const float v1 = v0 + tex->height / 65536.0f;

    M_PRIM prim = { .page = tex->tex_page };
    const float x[4] = { sprite.x0, sprite.x1, sprite.x0, sprite.x1 };
    const float y[4] = { sprite.y0, sprite.y0, sprite.y1, sprite.y1 };
    const float u[4] = { u0, u1, u0, u1 };
    const float v[4] = { v0, v0, v1, v1 };
    // The sprite's colours go round the corners: tl, tr, br, bl.
    const int32_t color_idx[4] = { 0, 1, 3, 2 };
    for (int32_t i = 0; i < 4; i++) {
        M_ToScreen(x[i], y[i], sprite.z, &prim.v[i]);
        prim.v[i].u = u[i];
        prim.v[i].v = v[i];
        prim.v[i].argb = M_PackF(sprite.color[color_idx[i]]);
    }
    if (DC_PVR_IsLowRes()) {
        prim.half = M_SnapLowRes(&prim.v[0].x, &prim.v[1].x, tex->width)
            | M_SnapLowRes(&prim.v[0].y, &prim.v[2].y, tex->height);
        prim.v[2].x = prim.v[0].x;
        prim.v[3].x = prim.v[1].x;
        prim.v[1].y = prim.v[0].y;
        prim.v[3].y = prim.v[2].y;
    }
    Vector_Add(m_Priv.prims, &prim);
}

void OutputSource_UI_StageQuad(const OUTPUT_UI_QUAD quad)
{
    M_PRIM prim = { .page = DC_PVR_NO_TEXTURE };
    const float x[4] = { quad.x0, quad.x1, quad.x0, quad.x1 };
    const float y[4] = { quad.y0, quad.y0, quad.y1, quad.y1 };
    const RGBA_8888 c[4] = { quad.tl, quad.tr, quad.bl, quad.br };
    for (int32_t i = 0; i < 4; i++) {
        M_ToScreen(x[i], y[i], quad.z, &prim.v[i]);
        prim.v[i].argb = M_Pack8(c[i]);
    }
    Vector_Add(m_Priv.prims, &prim);
}

void OutputSource_UI_StageCircle(const OUTPUT_UI_CIRCLE circle)
{
    const float ri = (float)circle.r_inner;
    const float ro = (float)circle.r_outer;
    if (ro <= ri) {
        return;
    }
    const uint32_t color = M_Pack8(circle.color);
    for (int32_t i = 0; i < M_RING_SEGMENTS; i++) {
        const float a0 = 2.0f * (float)M_PI * i / M_RING_SEGMENTS;
        const float a1 = 2.0f * (float)M_PI * (i + 1) / M_RING_SEGMENTS;
        M_PRIM prim = { .page = DC_PVR_NO_TEXTURE };
        const float x[4] = {
            circle.cx + ro * cosf(a0), circle.cx + ro * cosf(a1),
            circle.cx + ri * cosf(a0), circle.cx + ri * cosf(a1),
        };
        const float y[4] = {
            circle.cy + ro * sinf(a0), circle.cy + ro * sinf(a1),
            circle.cy + ri * sinf(a0), circle.cy + ri * sinf(a1),
        };
        for (int32_t j = 0; j < 4; j++) {
            M_ToScreen(x[j], y[j], circle.z, &prim.v[j]);
            prim.v[j].argb = color;
        }
        Vector_Add(m_Priv.prims, &prim);
    }
}

bool OutputSource_UI_HasStaged(void)
{
    return m_Priv.prims->count > 0 || m_Priv.meshes->count > 0
        || m_Priv.binocular_mask;
}
