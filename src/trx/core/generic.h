#pragma once

// Where int32_t is not int - newlib on SH-4 makes it long - a plain int or an
// enum matches none of the fixed-width associations of a _Generic. These name
// int and unsigned int as further spellings of the 32-bit types there, and
// nothing elsewhere, where they would duplicate an association.
#if defined(__DREAMCAST__)
    #define M_GENERIC_INT_ALIASES(s32_, u32_) int: s32_, unsigned int: u32_,
#else
    #define M_GENERIC_INT_ALIASES(s32_, u32_)
#endif
