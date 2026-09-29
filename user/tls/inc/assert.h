/*
 * user/tls/inc/assert.h -- assert.h minimal untuk mbedTLS bare-metal.
 */
#ifndef QAON_ASSERT_H
#define QAON_ASSERT_H

void tls_assert_fail(const char *expr, const char *file, int line);

#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : tls_assert_fail(#e, __FILE__, __LINE__))
#endif

#endif /* QAON_ASSERT_H */
