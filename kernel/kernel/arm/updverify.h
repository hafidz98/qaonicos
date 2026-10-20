/*
 * updverify.h -- B5.3: signed update package (QUP1) verification.
 *
 * upd_verify_package(pkg, len, version_out):
 *   0 ok (signature valid; *version_out set), -1 malformed/bad sig.
 * Format + threat model: docs/rencana-hardening/PLAN-B5.md.
 */
#ifndef QAONIC_UPDVERIFY_H
#define QAONIC_UPDVERIFY_H

int	upd_verify_package(const unsigned char *pkg, unsigned int len,
	    unsigned int *version_out);

#endif
