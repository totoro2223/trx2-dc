#include "dc_shz.h"

#include <sh4zam/shz_sh4zam.h>

float DC_Shz_InvSqrt(const float x)
{
    return shz_inv_sqrtf_fsrra(x);
}

float DC_Shz_Acos(const float x)
{
    return shz_acosf(x);
}

float DC_Shz_Sin(const float radians)
{
    return shz_sinf(radians);
}
