/* Minimal stdlib shim; littlefs uses LFS_MALLOC/LFS_FREE overrides. */
#ifndef QAONIC_SHIM_STDLIB_H
#define QAONIC_SHIM_STDLIB_H
typedef unsigned int size_t;
#ifndef NULL
#define NULL ((void *)0)
#endif
void *malloc(size_t n);
void free(void *p);
#endif
