/* Minimal string shim; implementations live in lfs_port.c. */
#ifndef QAONIC_SHIM_STRING_H
#define QAONIC_SHIM_STRING_H
typedef unsigned int size_t;
#ifndef NULL
#define NULL ((void *)0)
#endif
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memmove(void *dst, const void *src, size_t n);
size_t strlen(const char *s);
char *strchr(const char *s, int c);
size_t strspn(const char *s, const char *set);
size_t strcspn(const char *s, const char *set);
#endif
