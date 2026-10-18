// Stresses GD-ROM reads the way TRX does them: from a secondary thread with a
// large stack, through stdio, into unaligned buffers (one DMA per sector),
// while the main thread waits in thd_join. Tells driver or emulator trouble
// apart from engine bugs.
//
//   kos-cc -O2 -o cdtest.elf cdtest.c

#include <kos.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define FILE_PATH "/cd/games/tr2/levels/assault.tr2"

static void *M_Reader(void *const param)
{
    uint8_t *const buffer = malloc(4096 + 64);
    const uint64_t start = timer_ms_gettime64();
    for (int pass = 0; pass < 20; pass++) {
        FILE *const fp = fopen(FILE_PATH, "rb");
        if (fp == NULL) {
            printf("cdtest: cannot open %s\n", FILE_PATH);
            return NULL;
        }
        size_t total = 0;
        int c;
        // getc through a buffered stream, like the Lua loader.
        while (total < 256 * 1024 && (c = getc(fp)) != EOF) {
            buffer[total & 4095] = (uint8_t)c;
            total++;
        }
        fclose(fp);
        printf(
            "cdtest: t=%ums pass %d read %u bytes\n",
            (unsigned)(timer_ms_gettime64() - start), pass, (unsigned)total);
    }
    printf("cdtest: done\n");
    return NULL;
}

int main(void)
{
    const kthread_attr_t attr = {
        .stack_size = 1024 * 1024,
        .label = "reader",
    };
    kthread_t *const thread = thd_create_ex(&attr, M_Reader, NULL);
    thd_join(thread, NULL);
    return 0;
}
