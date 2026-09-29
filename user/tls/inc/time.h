/*
 * user/tls/inc/time.h -- time.h minimal untuk mbedTLS bare-metal.
 */
#ifndef QAON_TIME_H
#define QAON_TIME_H

typedef long time_t;

long tls_time_get(long *t);	/* implementasi: sys_time_get() */

#endif /* QAON_TIME_H */
