#pragma once

// Dreamcast shadows of Lara's body; which kind is visuals.shadow_type.
//
// While casting is on, every opaque mesh instance staged becomes a shadow
// caster.
//
// SHADOW_TYPE_VOLUME (a proof of concept): each caster's bounding box,
// stretched away from the nearest room light, makes a closed volume that
// darkens the world inside it; the casters themselves are left out.
//
// SHADOW_TYPE_SOFT: the casters' silhouette as seen from each of the up to
// three nearest lights is drawn into a small texture (the shadow map) as the
// next frame starts, and the faces near Lara are drawn again in that frame
// with it projected on them, multiplying the colour already there. Bilinear
// filtering of the small texture softens the edges.

#include <stdbool.h>

void DC_Shadow_SetCasting(bool casting);
bool DC_Shadow_IsCasting(void);

// Ends the casters of this frame: sends their volumes, or keeps them for the
// soft shadows. Goes after the world's passes, in the world's depth layer.
void DC_Shadow_DrawVolumes(void);

// Draws the shadow map from the casters the last frame kept. Goes before the
// frame starts.
void DC_Shadow_RenderMaps(void);
