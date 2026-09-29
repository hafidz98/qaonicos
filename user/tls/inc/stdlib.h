/*
 * user/tls/inc/stdlib.h -- stdlib.h minimal untuk mbedTLS bare-metal.
 * Hanya yang dirujuk sumber mbedTLS subset Q2b.
 */
#ifndef QAON_STDLIB_H
#define QAON_STDLIB_H

void *calloc(unsigned n, unsigned size);
void free(void *p);
void *malloc(unsigned size);
int rand(void);
void srand(unsigned seed);

/* Bump allocator (user/tls/tls_port.c); dipakai MBEDTLS_PLATFORM_STD_*. */
void *tls_calloc(unsigned n, unsigned size);
void tls_free(void *p);

#endif /* QAON_STDLIB_H */
