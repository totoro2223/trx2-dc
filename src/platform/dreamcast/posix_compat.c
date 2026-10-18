// Dreamcast platform layer: POSIX calls newlib on KallistiOS leaves out.

#include <errno.h>
#include <sys/types.h>

// No KOS filesystem can shrink an open file in place. Callers report the
// failure and carry on.
int ftruncate(const int fd, const off_t length)
{
    errno = ENOSYS;
    return -1;
}
