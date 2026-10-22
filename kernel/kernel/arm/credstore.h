/* credstore.h -- B5.5 credential store: secrets in littlefs,
 * AES-256-GCM encrypted at rest (PLAN-B5 section 4).
 * Names map to littlefs files "cfg-<name>". */
#ifndef QAONIC_CREDSTORE_H
#define QAONIC_CREDSTORE_H

#define CRED_NAME_MAX	16
#define CRED_VAL_MAX	512

int	credstore_set(const char *name, const unsigned char *val,
	    unsigned int len);
int	credstore_get(const char *name, unsigned char *buf,
	    unsigned int max);		/* len or -1 */
int	credstore_present(const char *name);
int	credstore_clear(const char *name);
/* AES-GCM host vectors + store roundtrip; pass bitmask
 * (bit0..2 vectors V1..V3, bit3 store). */
int	credstore_selftest(void);

#endif
