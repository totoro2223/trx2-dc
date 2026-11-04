// What the shared code calls and only the no-Lua build (nolua_hud.c,
// nolua_passport.c) answers; the build with Lua (DC_LUA=1) says nothing on the
// screen for them. A test build: the scripts have their own messages.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

void DC_Msg(const char *const fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

void DC_QuickSave_Say(const char *const text, const int32_t hold)
{
    (void)text;
    (void)hold;
}
