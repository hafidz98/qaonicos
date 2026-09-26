/* lib.c - tiny freestanding libc subset. Also satisfies the compiler:
 * with -ffreestanding it may still emit memcpy/memset calls. */
#include "lib.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (*s++) n++;
    return n;
}
