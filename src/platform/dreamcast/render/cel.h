#pragma once

// The cel shading of the meshes lit by the object light (see batcher.c).

#include <stdbool.h>

// Whether the meshes drawn from here on may be cel shaded, as the player's
// setting and the mode allow; off for the interface's models, such as the
// inventory's.
void DC_Cel_SetAllowed(bool allowed);
