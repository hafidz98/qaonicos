/* aesgcm.h -- self-contained AES-256-GCM (B5.5 credential store).
 * Encrypt-side AES only (GCM never decrypts the block cipher),
 * GHASH per SP 800-38D. Verified against host-generated test
 * vectors by the SYS_CRED self-test. */
#ifndef QAONIC_AESGCM_H
#define QAONIC_AESGCM_H

/* AEAD encrypt: out_ct (len bytes) + out_tag (16 bytes).
 * nonce is 12 bytes. aad may be NULL when aad_len is 0. */
void	aes256_gcm_encrypt(const unsigned char key[32],
	    const unsigned char nonce[12],
	    const unsigned char *aad, unsigned int aad_len,
	    const unsigned char *pt, unsigned int len,
	    unsigned char *ct, unsigned char tag[16]);

/* AEAD decrypt: 0 ok (tag verified, pt written), -1 bad tag. */
int	aes256_gcm_decrypt(const unsigned char key[32],
	    const unsigned char nonce[12],
	    const unsigned char *aad, unsigned int aad_len,
	    const unsigned char *ct, unsigned int len,
	    const unsigned char tag[16], unsigned char *pt);

#endif
