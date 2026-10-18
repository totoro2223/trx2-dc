#pragma once

// Dreamcast shadow volumes (a proof of concept): while casting is on, every
// opaque mesh instance staged becomes a shadow caster. Each caster's bounding
// box, stretched away from the nearest room light, makes a closed volume
// that darkens the world inside it; the casters themselves are left out.

#include <stdbool.h>

void DC_Shadow_SetCasting(bool casting);
bool DC_Shadow_IsCasting(void);

// Sends this frame's volumes and forgets the casters. Goes after the world's
// passes, in the world's depth layer.
void DC_Shadow_DrawVolumes(void);
