#include <trx/game/game_strings/entries.h>

#include <trx/core/log.h>
#include <trx/core/memory.h>
#include <trx/core/subsystem.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <uthash.h>

// Resolve string references that start with "$". A value such as
// "$objects/compass_item" makes "objects/compass_option" stand for
// "objects/compass_item". A lookup with no direct value uses the nearest
// parent reference and carries over the rest of the path, so
// "objects/compass_option/name" reads "objects/compass_item/name". References
// follow the value in force at lookup time, including values supplied by a
// language file or a level.
#define M_REF_MARK '$'

// Limit one lookup before treating the reference chain as a loop.
#define M_REF_DEPTH_MAX 8

// Limit the path produced by following a reference.
#define M_KEY_SIZE 256

// One-slot-per-string for stable indirection on reload.
typedef struct M_SLOT {
    const char *value;
} M_SLOT;

// Who holds a value's text: the entry (its own copy), a loaded strings file,
// or the program (a string literal). Only the first is freed here.
typedef enum {
    M_OWNED,
    M_FROM_TABLE,
    M_STATIC,
} M_HOLDER;

typedef struct {
    char *key;
    M_SLOT *slot;
    bool is_overridden;
    const char *base_value;
    M_HOLDER value_holder;
    M_HOLDER base_holder;
    // The slot lives in the entry, which never moves once made.
    M_SLOT slot_storage;
    UT_hash_handle hh;
} M_STRING_ENTRY;

static M_STRING_ENTRY *m_StringTable = nullptr;

// Return the value stored at a path without following references.
static const char *M_GetRaw(const char *const key)
{
    M_STRING_ENTRY *entry;
    HASH_FIND_STR(m_StringTable, key, entry);
    return entry != nullptr ? entry->slot->value : nullptr;
}

// Report whether a value names another path.
static bool M_IsRef(const char *const value)
{
    return value != nullptr && value[0] == M_REF_MARK;
}

// Write the referenced parent path plus the remaining path to out. Return
// false if no parent has a reference or if the result does not fit.
static bool M_FollowParentRef(const char *const key, char *const out)
{
    char head[M_KEY_SIZE];
    if ((size_t)snprintf(head, sizeof(head), "%s", key) >= sizeof(head)) {
        return false;
    }
    for (char *sep = strrchr(head, '/'); sep != nullptr;
         sep = strrchr(head, '/')) {
        *sep = '\0';
        const char *const value = M_GetRaw(head);
        if (!M_IsRef(value)) {
            continue;
        }
        char next[M_KEY_SIZE];
        if ((size_t)snprintf(
                next, sizeof(next), "%s%s", value + 1, key + strlen(head))
            >= sizeof(next)) {
            return false;
        }
        strcpy(out, next);
        return true;
    }
    return false;
}

static M_STRING_ENTRY *M_FindOrAdd(const char *const key)
{
    M_STRING_ENTRY *entry;
    HASH_FIND_STR(m_StringTable, key, entry);
    if (entry == nullptr) {
        entry = Memory_Alloc(sizeof(*entry));
        entry->key = Memory_DupStr(key);
        entry->slot = &entry->slot_storage;
        HASH_ADD_KEYPTR(
            hh, m_StringTable, entry->key, strlen(entry->key), entry);
    }
    return entry;
}

void GameString_RestoreDefaults(void)
{
#include <trx/game/game_strings/entries.def>
}

void GameString_Reset(void)
{
    GameString_Clear();
    GameString_RestoreDefaults();
}

static void M_Release(const char *const value, const M_HOLDER holder)
{
    if (holder == M_OWNED) {
        Memory_Free((void *)value);
    }
}

static void M_Define(
    const char *const key, const char *const value, const M_HOLDER holder)
{
    M_STRING_ENTRY *const entry = M_FindOrAdd(key);
    // Defined again as each level loads: the same value is kept where it is,
    // rather than made anew in the middle of the memory the level fills.
    if (entry->slot->value != nullptr && value != nullptr
        && holder == entry->value_holder
        && strcmp(entry->slot->value, value) == 0) {
        return;
    }
    M_Release(entry->slot->value, entry->value_holder);
    entry->slot->value = holder == M_OWNED ? Memory_DupStr(value) : value;
    entry->value_holder = holder;
}

static void M_Override(
    const char *const key, const char *const value, const M_HOLDER holder)
{
    M_STRING_ENTRY *const entry = M_FindOrAdd(key);
    if (!entry->is_overridden) {
        entry->is_overridden = true;
        entry->base_value = entry->slot->value;
        entry->base_holder = entry->value_holder;
        entry->slot->value = nullptr;
        entry->value_holder = M_OWNED;
    }
    M_Define(key, value, holder);
}

void GameString_Define(const char *const key, const char *value)
{
    M_Define(key, value, M_OWNED);
}

void GameString_DefineStatic(const char *const key, const char *const value)
{
    M_Define(key, value, M_STATIC);
}

void GameString_DefineFromTable(const char *const key, const char *const value)
{
    M_Define(key, value, M_FROM_TABLE);
}

void GameString_Override(const char *const key, const char *const value)
{
    M_Override(key, value, M_OWNED);
}

void GameString_OverrideFromTable(
    const char *const key, const char *const value)
{
    M_Override(key, value, M_FROM_TABLE);
}

void GameString_ForgetTables(void)
{
    M_STRING_ENTRY *entry, *tmp;
    HASH_ITER(hh, m_StringTable, entry, tmp)
    {
        if (entry->value_holder == M_FROM_TABLE) {
            entry->slot->value = nullptr;
            entry->value_holder = M_OWNED;
        }
        if (entry->base_holder == M_FROM_TABLE) {
            entry->base_value = nullptr;
            entry->base_holder = M_OWNED;
        }
    }
}

void GameString_RevertOverrides(void)
{
    M_STRING_ENTRY *entry, *tmp;
    HASH_ITER(hh, m_StringTable, entry, tmp)
    {
        if (!entry->is_overridden) {
            continue;
        }
        M_Release(entry->slot->value, entry->value_holder);
        entry->slot->value = entry->base_value;
        entry->value_holder = entry->base_holder;
        entry->base_value = nullptr;
        entry->base_holder = M_OWNED;
        entry->is_overridden = false;
    }
}

bool GameString_IsKnown(const char *const key)
{
    M_STRING_ENTRY *entry;
    HASH_FIND_STR(m_StringTable, key, entry);
    return entry != nullptr && entry->slot->value != nullptr;
}

const char *GameString_Get(const char *const key)
{
    char buf[M_KEY_SIZE];
    const char *cur_key = key;
    for (int32_t depth = 0; depth < M_REF_DEPTH_MAX; depth++) {
        const char *const value = M_GetRaw(cur_key);
        if (value != nullptr && !M_IsRef(value)) {
            return value;
        }
        if (M_IsRef(value)) {
            if ((size_t)snprintf(buf, sizeof(buf), "%s", value + 1)
                >= sizeof(buf)) {
                return nullptr;
            }
        } else if (!M_FollowParentRef(cur_key, buf)) {
            return nullptr;
        }
        cur_key = buf;
    }
    LOG_ERROR("'%s' stands on a loop of references", key);
    return nullptr;
}

const char *const *GameString_GetPtr(const char *const key)
{
    M_STRING_ENTRY *entry;
    HASH_FIND_STR(m_StringTable, key, entry);
    return entry ? &entry->slot->value : nullptr;
}

void GameString_Clear(void)
{
    M_STRING_ENTRY *entry, *tmp;

    HASH_ITER(hh, m_StringTable, entry, tmp)
    {
        HASH_DEL(m_StringTable, entry);
        Memory_Free(entry->key);
        M_Release(entry->slot->value, entry->value_holder);
        M_Release(entry->base_value, entry->base_holder);
        Memory_Free(entry);
    }
}

REGISTER_SUBSYSTEM(.init = GameString_Reset, .shutdown = GameString_Clear)
