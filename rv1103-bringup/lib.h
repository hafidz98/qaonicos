/* lib.h - tiny freestanding libc subset for the kernel. */
#ifndef _KLIB_H_
#define _KLIB_H_

#include <stdint.h>
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
size_t strlen(const char *s);

#endif /* _KLIB_H_ */
