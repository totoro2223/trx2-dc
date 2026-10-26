#include "dc_games.h"

#include <trx/core/log.h>

#include <arch/arch.h>
#include <arch/cache.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define M_MAX_GAMES 4
// Words left at the top of RAM, in the uncached mirror (below the main
// thread's stack, which the boot code does not reach into): a magic, the
// game, whether it is the expansion, and a check of the three.
#define M_MAGIC_ADDRESS 0xACFF0100u
#define M_MAGIC 0x54525843u // "TRXC"
#define M_CHECK 0x5A5A5A5Au

typedef struct {
    uint32_t magic;
    uint32_t game;
    uint32_t expansion;
    uint32_t crashes; // restarts in a row after a crash
    uint32_t level; // the level the next boot starts in, plus 2; 0 for none
    uint32_t check;
} M_NOTE;

char g_DC_Root[40] = "/cd";

static int m_Ids[M_MAX_GAMES];
static int m_Count = 0;
static int m_Index = 0;
static bool m_Expansion = false;
static uint32_t m_Crashes = 0;
static int m_StartLevel = -2;
// The game chosen to run next, as the title or the level list asked for it:
// the main loop (dc_main.c) starts it in place when this game ends.
typedef struct {
    int game; // 0 for none
    bool expansion;
    int level; // -1 the title, -2 the list of levels, from 0 a level
} M_PENDING;
static M_PENDING m_Pending = {};

static volatile M_NOTE *M_Note(void)
{
    // A dirty line of the cached copy of these words (left by whatever used
    // the memory before) would be written back over the note when the console
    // starts over: the line is dropped first, and the words are then read and
    // written uncached.
    dcache_purge_range(0x8CFF0100u & ~31u, 64);
    return (volatile M_NOTE *)M_MAGIC_ADDRESS;
}

static uint32_t M_Sum(const volatile M_NOTE *const note)
{
    return M_MAGIC ^ note->game ^ note->expansion ^ note->crashes ^ note->level
        ^ M_CHECK;
}

static void M_Leave(
    const int game, const bool expansion, const uint32_t crashes,
    const uint32_t level)
{
    volatile M_NOTE *const note = M_Note();
    note->game = (uint32_t)game;
    note->expansion = expansion ? 1u : 0u;
    note->crashes = crashes;
    note->level = level;
    note->check = M_Sum(note);
    note->magic = M_MAGIC;
}

void DC_Games_Init(void)
{
    m_Count = 0;
    // What the last boot left, once (a cold start finds RAM with anything in
    // it, which the check rejects).
    volatile M_NOTE *const note = M_Note();
    const bool noted = note->magic == M_MAGIC && note->check == M_Sum(note);
    m_Crashes = noted ? note->crashes : 0;
    const uint32_t noted_game = note->game;
    const uint32_t noted_expansion = note->expansion;
    m_StartLevel = noted && note->level != 0 ? (int)note->level - 2 : -2;
    note->magic = 0;
    FILE *const list = fopen("/cd/GAMES.TXT", "r");
    if (list == nullptr) {
        return;
    }
    // One line of ids (1 to 3), each once; whatever follows it is ignored.
    char line[32] = "";
    if (fgets(line, sizeof(line), list) != nullptr) {
        for (const char *at = line; *at != '\0' && m_Count < M_MAX_GAMES; at++) {
            if (*at < '1' || *at > '3') {
                continue;
            }
            bool seen = false;
            for (int i = 0; i < m_Count; i++) {
                seen = seen || m_Ids[i] == *at - '0';
            }
            if (!seen) {
                m_Ids[m_Count++] = *at - '0';
            }
        }
    }
    fclose(list);
    if (m_Count == 0) {
        return;
    }

    // The disc starts in the first game unless the note names another.
    m_Index = 0;
    if (noted) {
        for (int i = 0; i < m_Count; i++) {
            if ((uint32_t)m_Ids[i] == noted_game) {
                m_Index = i;
                m_Expansion = noted_expansion != 0;
            }
        }
    }
    snprintf(g_DC_Root, sizeof(g_DC_Root), "/cd/g%d", m_Ids[m_Index]);
    LOG_INFO(
        "disc of %d games: running TR%d%s", m_Count, m_Ids[m_Index],
        m_Expansion ? " expansion" : "");
}

void DC_Games_Path(char *const out, const size_t size, const char *const name)
{
    snprintf(out, size, "%s/%s", g_DC_Root, name);
}

int DC_Games_CurrentId(void)
{
    return m_Count > 0 ? m_Ids[m_Index] : 0;
}

int DC_Games_PrevId(void)
{
    return m_Count > 0 && m_Index > 0 ? m_Ids[m_Index - 1] : 0;
}

int DC_Games_NextId(void)
{
    return m_Count > 0 && m_Index + 1 < m_Count ? m_Ids[m_Index + 1] : 0;
}

const char *DC_Games_Expansion(void)
{
    if (!m_Expansion) {
        return "";
    }
    switch (DC_Games_CurrentId()) {
    case 1:
        return "tr1-ub";
    case 2:
        return "tr2-gm";
    case 3:
        return "tr3-la";
    default:
        return "";
    }
}

static const char *M_ExpansionOf(const int id)
{
    return id == 1 ? "tr1-ub" : id == 2 ? "tr2-gm" : id == 3 ? "tr3-la" : "";
}

bool DC_Games_ExpansionAvailable(void)
{
    if (m_Count == 0) {
        return false;
    }
    const char *const mod = M_ExpansionOf(DC_Games_CurrentId());
    if (mod[0] == '\0') {
        return false;
    }
    char path[96];
    snprintf(path, sizeof(path), "%s/games/%s/levels", g_DC_Root, mod);
    struct stat st;
    return stat(path, &st) == 0;
}

int DC_Games_TakeStartLevel(void)
{
    const int level = m_StartLevel;
    m_StartLevel = -2;
    return level;
}

bool DC_Games_Choose(const int delta, const bool title)
{
    if (m_Count == 0) {
        return false;
    }
    const int index = m_Index + (delta < 0 ? -1 : 1);
    if (index < 0 || index >= m_Count) {
        return false;
    }
    LOG_INFO("next boot: TR%d", m_Ids[index]);
    // The level is stored plus 2, so the title (-1) is 1.
    m_Pending = (M_PENDING) { .game = m_Ids[index], .level = title ? -1 : -2 };
    return true;
}

bool DC_Games_ChooseExpansion(const int level)
{
    if (!DC_Games_ExpansionAvailable()) {
        return false;
    }
    LOG_INFO("next boot: the expansion of TR%d", DC_Games_CurrentId());
    m_Pending = (M_PENDING) {
        .game = DC_Games_CurrentId(), .expansion = true, .level = level
    };
    return true;
}

void DC_Games_Remember(void)
{
    if (m_Count > 0) {
        M_Leave(DC_Games_CurrentId(), false, 0, 0);
    }
}

bool DC_Games_ApplyPending(void)
{
    if (m_Pending.game == 0) {
        return false;
    }
    for (int i = 0; i < m_Count; i++) {
        if (m_Ids[i] == m_Pending.game) {
            m_Index = i;
        }
    }
    m_Expansion = m_Pending.expansion;
    m_StartLevel = m_Pending.level;
    m_Pending = (M_PENDING) {};
    snprintf(g_DC_Root, sizeof(g_DC_Root), "/cd/g%d", m_Ids[m_Index]);
    LOG_INFO(
        "changing in place to TR%d%s", m_Ids[m_Index],
        m_Expansion ? " expansion" : "");
    return true;
}

bool DC_Games_Recover(void)
{
    // A crash that comes back at once is left to stop the console: starting
    // over would only loop.
    if (m_Crashes >= 3) {
        return false;
    }
    M_Leave(DC_Games_CurrentId(), false, m_Crashes + 1, 0);
    return true;
}
