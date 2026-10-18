// Dreamcast platform layer: reports large heap allocations (debug builds).
//
// With 16 MiB of RAM the question is always where it went. The link wraps
// malloc, calloc and realloc (-Wl,--wrap=...) so that every allocation of
// at least DC_ALLOC_TRACE_MIN bytes is printed with its caller; feed the
// address to sh-elf-addr2line -f -e TRX.elf.

#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define DC_ALLOC_TRACE_MIN (128 * 1024)

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);

extern char _executable_start[];
extern char etext[];

static void M_Report(
    const char *const what, const size_t size, const void *const result,
    const void *const caller)
{
    const struct mallinfo mem = mallinfo();
    printf(
        "[alloc] %s %uK -> %p from %p (in use %uK) trace:", what,
        (unsigned)(size / 1024), result, caller,
        (unsigned)(mem.uordblks / 1024));
    // Words on the stack that point into the code: likely return addresses.
    uint32_t sp;
    __asm__ volatile("mov r15, %0" : "=r"(sp));
    const uint32_t *const stack = (const uint32_t *)sp;
    int frames = 0;
    for (int i = 0; i < 256 && frames < 10; i++) {
        const uint32_t word = stack[i];
        if (word >= (uint32_t)_executable_start && word < (uint32_t)etext
            && (word & 1) == 0) {
            printf(" %08lx", (unsigned long)word);
            frames++;
        }
    }
    printf("\n");
}

void *__wrap_malloc(const size_t size)
{
    void *const result = __real_malloc(size);
    if (size >= DC_ALLOC_TRACE_MIN || result == nullptr) {
        M_Report("malloc", size, result, __builtin_return_address(0));
    }
    return result;
}

void *__wrap_calloc(const size_t count, const size_t size)
{
    void *const result = __real_calloc(count, size);
    if (count * size >= DC_ALLOC_TRACE_MIN || result == nullptr) {
        M_Report("calloc", count * size, result, __builtin_return_address(0));
    }
    return result;
}

void *__wrap_realloc(void *const ptr, const size_t size)
{
    void *const result = __real_realloc(ptr, size);
    if (size >= DC_ALLOC_TRACE_MIN || result == nullptr) {
        M_Report("realloc", size, result, __builtin_return_address(0));
    }
    return result;
}
