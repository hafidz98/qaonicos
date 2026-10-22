/* qr.h -- tiny QR encoder for the ONB-2 setup screen.
 * Byte mode, ECC level M, versions 1..6 (auto), all 8 masks
 * scored with the standard penalty rules. No assets, no deps. */
#ifndef QAONIC_QR_H
#define QAONIC_QR_H

/* Encode text; returns the module count N (matrix is N x N),
 * or -1 if the text does not fit version 6-M. */
int	qr_encode(const char *text);
/* Module at (x,y): 1 = dark, 0 = light. Valid after qr_encode. */
int	qr_module(int x, int y);

#endif
