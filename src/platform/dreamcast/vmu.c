// Dreamcast platform layer: saved games on the memory card (VMU).
//
// The engine keeps writing its saves as files in the saves directory on the
// RAM disk (see dc_main.c); this layer mirrors each of them to the first
// memory card as a VMS file, with the header and icon the Dreamcast's file
// manager wants, and copies them back at boot.
//
// A card file name holds 12 characters and the engine's ("save_tr2_03.dat")
// do not fit, so the card file is named after it (TRX_TR2_03) and its
// payload begins with the name it has in the saves directory.

#include "vmu.h"
#include "render/pvr.h"

#include <trx/core/log.h>

#include <dc/maple.h>
#include <dc/vmu_pkg.h>
#include <dc/vmufs.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M_APP_ID "TRX"
#define M_FILE_PREFIX "TRX_"
#define M_NAME_SIZE 32
#define M_ICON_SIZE 32

static char m_SavesDir[128] = "";
static int m_LastResult = 0;

int DC_VMU_LastResult(void)
{
    return m_LastResult;
}

static maple_device_t *M_GetCard(void)
{
    for (int i = 0;; i++) {
        maple_device_t *const device = maple_enum_type(i, MAPLE_FUNC_MEMCARD);
        if (device == nullptr) {
            return nullptr;
        }
        if (device->valid) {
            return device;
        }
    }
}

// The file's name in the saves directory, or nullptr if it is elsewhere.
static const char *M_GetRelativeName(const char *const path)
{
    const size_t dir_len = strlen(m_SavesDir);
    if (dir_len == 0 || strncmp(path, m_SavesDir, dir_len) != 0
        || path[dir_len] != '/') {
        return nullptr;
    }
    const char *const name = path + dir_len + 1;
    if (strlen(name) >= M_NAME_SIZE || strchr(name, '/') != nullptr) {
        return nullptr;
    }
    return name;
}

// save_tr2_03.dat -> TRX_TR2_03
static void M_GetCardName(const char *const name, char out[VMU_FILENAME_SIZE + 1])
{
    const char *src = name;
    if (strncmp(src, "save_", 5) == 0) {
        src += 5;
    }
    strcpy(out, M_FILE_PREFIX);
    size_t len = strlen(out);
    for (; *src != '\0' && *src != '.' && len < VMU_FILENAME_SIZE; src++) {
        out[len++] = isalnum((unsigned char)*src)
            ? (char)toupper((unsigned char)*src)
            : '_';
    }
    out[len] = '\0';
}

// The two letters on a dark tile with a gold frame.
static int M_IconPixel(const int x, const int y)
{
    if (x == 0 || y == 0 || x == M_ICON_SIZE - 1 || y == M_ICON_SIZE - 1) {
        return 2;
    }
    if (y < 8 || y > 23) {
        return 1;
    }
    // T
    if ((y <= 10 && x >= 3 && x <= 14) || (x >= 7 && x <= 10)) {
        return 3;
    }
    // R: stem, bowl, leg
    if (x >= 17 && x <= 19) {
        return 3;
    }
    if ((y <= 10 || (y >= 15 && y <= 17)) && x >= 17 && x <= 26) {
        return 3;
    }
    if (y > 10 && y < 15 && x >= 25 && x <= 28) {
        return 3;
    }
    const int leg = 20 + (y - 17);
    if (y > 17 && x >= leg && x <= leg + 3 && x <= 28) {
        return 3;
    }
    return 1;
}

static void M_FillIcon(vmu_pkg_t *const pkg, uint8_t *const icon)
{
    pkg->icon_cnt = 1;
    pkg->icon_anim_speed = 0;
    pkg->icon_pal[0] = 0x0000;
    pkg->icon_pal[1] = 0xF222;
    pkg->icon_pal[2] = 0xFC90;
    pkg->icon_pal[3] = 0xFFD6;
    for (int y = 0; y < M_ICON_SIZE; y++) {
        for (int x = 0; x < M_ICON_SIZE; x += 2) {
            icon[(y * M_ICON_SIZE + x) / 2] =
                (uint8_t)(M_IconPixel(x, y) << 4 | M_IconPixel(x + 1, y));
        }
    }
    pkg->icon_data = icon;
}

static bool M_WriteFile(const char *const path, const void *data, size_t size)
{
    FILE *const fp = fopen(path, "wb");
    if (fp == nullptr) {
        return false;
    }
    const bool ok = fwrite(data, 1, size, fp) == size;
    fclose(fp);
    return ok;
}

static bool M_RestoreFile(maple_device_t *const card, const vmu_dir_t *const dirent)
{
    void *raw = nullptr;
    int raw_size = 0;
    if (vmufs_read_dirent(card, dirent, &raw, &raw_size) < 0) {
        return false;
    }
    bool ok = false;
    vmu_pkg_t pkg;
    if (vmu_pkg_parse(raw, (size_t)raw_size, &pkg) == 0
        && strncmp(pkg.app_id, M_APP_ID, sizeof(M_APP_ID)) == 0
        && pkg.data_len > M_NAME_SIZE) {
        char name[M_NAME_SIZE];
        memcpy(name, pkg.data, M_NAME_SIZE);
        name[M_NAME_SIZE - 1] = '\0';
        if (name[0] != '\0' && strchr(name, '/') == nullptr) {
            char path[192];
            snprintf(path, sizeof(path), "%s/%s", m_SavesDir, name);
            ok = M_WriteFile(
                path, pkg.data + M_NAME_SIZE, (size_t)pkg.data_len - M_NAME_SIZE);
            if (ok) {
                // Ahead of the engine's log being set up.
                printf("[vmu] restored %s from the memory card\n", name);
            }
        }
    }
    free(raw);
    return ok;
}

void DC_VMU_RestoreSaves(const char *const saves_dir)
{
    snprintf(m_SavesDir, sizeof(m_SavesDir), "%s", saves_dir);
    maple_device_t *const card = M_GetCard();
    if (card == nullptr) {
        printf("[vmu] no memory card: saved games last until switched off\n");
        return;
    }
    vmu_dir_t *entries = nullptr;
    int count = 0;
    if (vmufs_readdir(card, &entries, &count) < 0) {
        printf("[vmu] the memory card could not be read\n");
        return;
    }
    for (int i = 0; i < count; i++) {
        if (entries[i].filetype != 0
            && strncmp(entries[i].filename, M_FILE_PREFIX, strlen(M_FILE_PREFIX))
                == 0) {
            M_RestoreFile(card, &entries[i]);
        }
    }
    free(entries);
}

static bool M_Store(const char *path, const char *title, bool icon);

bool DC_VMU_StoreSave(const char *const path, const char *const title)
{
    return M_Store(path, title, true);
}

bool DC_VMU_StoreSmall(const char *const path, const char *const title)
{
    return M_Store(path, title, false);
}

static bool M_Store(
    const char *const path, const char *const title, const bool icon)
{
    const char *const name = M_GetRelativeName(path);
    maple_device_t *const card = M_GetCard();
    m_LastResult = card == nullptr ? 3 : 2;
    if (name == nullptr || card == nullptr) {
        return false;
    }

    // The passport has drawn a box that says the card is being written (see
    // Passport:save_with_notice) before it asks for this.
    FILE *const fp = fopen(path, "rb");
    if (fp == nullptr) {
        return false;
    }
    fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *const payload = size > 0 ? malloc(M_NAME_SIZE + (size_t)size) : nullptr;
    bool ok = payload != nullptr;
    if (!ok) {
        // The heap is dry in the fullest levels (TR3's Temple Ruins, DC build
        // 313): the save stays on the RAM disk, which the power switch empties.
        LOG_WARNING(
            "%s could not be copied to the memory card: no memory for its %ld "
            "bytes", name, size);
    }
    if (ok) {
        memset(payload, 0, M_NAME_SIZE);
        strcpy((char *)payload, name);
        ok = fread(payload + M_NAME_SIZE, 1, (size_t)size, fp) == (size_t)size;
    }
    fclose(fp);

    uint8_t *file = nullptr;
    int file_size = 0;
    if (ok) {
        vmu_pkg_t pkg = {};
        snprintf(pkg.desc_short, sizeof(pkg.desc_short), "TOMB RAIDER");
        snprintf(
            pkg.desc_long, sizeof(pkg.desc_long), "%s",
            title != nullptr && title[0] != '\0' ? title : name);
        snprintf(pkg.app_id, sizeof(pkg.app_id), M_APP_ID);
        uint8_t icon_data[M_ICON_SIZE * M_ICON_SIZE / 2];
        if (icon) {
            M_FillIcon(&pkg, icon_data);
        }
        pkg.eyecatch_type = VMUPKG_EC_NONE;
        pkg.data_len = M_NAME_SIZE + (int)size;
        pkg.data = payload;
        ok = vmu_pkg_build(&pkg, &file, &file_size) == 0;
        if (!ok) {
            LOG_WARNING("%s could not be packed for the memory card", name);
        }
    }

    if (ok) {
        char card_name[VMU_FILENAME_SIZE + 1];
        M_GetCardName(name, card_name);
        ok = vmufs_write(card, card_name, file, file_size, VMUFS_OVERWRITE) == 0;
        if (ok) {
            LOG_INFO(
                "%s saved to the memory card as %s (%d blocks)", name,
                card_name, (file_size + 511) / 512);
        } else {
            LOG_WARNING(
                "%s could not be saved to the memory card (%d blocks free, "
                "%d needed)",
                name, vmufs_free_blocks(card), (file_size + 511) / 512);
        }
    }
    free(file);
    free(payload);
    m_LastResult = ok ? 1 : 2;
    return ok;
}

void DC_VMU_DeleteSave(const char *const path)
{
    const char *const name = M_GetRelativeName(path);
    maple_device_t *const card = M_GetCard();
    if (name == nullptr || card == nullptr) {
        return;
    }
    char card_name[VMU_FILENAME_SIZE + 1];
    M_GetCardName(name, card_name);
    vmufs_delete(card, card_name);
}

bool DC_VMU_WriteLog(const char *const card_name, const char *const text)
{
    maple_device_t *const card = M_GetCard();
    if (card == nullptr) {
        return false;
    }
    const size_t length = strlen(text);
    uint8_t *const payload = malloc(length + 1);
    if (payload == nullptr) {
        return false;
    }
    memcpy(payload, text, length + 1);
    vmu_pkg_t pkg = {};
    snprintf(pkg.desc_short, sizeof(pkg.desc_short), "TRX LOG");
    snprintf(pkg.desc_long, sizeof(pkg.desc_long), "Where the game stopped");
    snprintf(pkg.app_id, sizeof(pkg.app_id), M_APP_ID);
    pkg.icon_cnt = 0;
    pkg.eyecatch_type = VMUPKG_EC_NONE;
    pkg.data_len = (int)length + 1;
    pkg.data = payload;
    uint8_t *file = nullptr;
    int file_size = 0;
    bool ok = vmu_pkg_build(&pkg, &file, &file_size) == 0;
    if (ok) {
        ok = vmufs_write(card, card_name, file, file_size, VMUFS_OVERWRITE) == 0;
    }
    free(file);
    free(payload);
    return ok;
}
