/*
 * cfg.c - Baca/tulis NVS setting aplikasi via sys_sd_read/sys_sd_write.
 *
 * Gaya K&R C seperti modul user lain; tanpa libc (loop salin manual).
 */
#include "cfg/cfg.h"
#include "ulib/ulib.h"

#define SECTOR_SZ	512

/* Pastikan ukuran struct sesuai asumsi layout (gagal compile bila tidak). */
typedef char nvs_size_ok[sizeof(struct nvs_state) == 868 ? 1 : -1];

/* Buffer 2 sektor di .bss (bukan stack) agar aman untuk stack user. */
static unsigned char	nvs_buf[NVS_NSECTORS * SECTOR_SZ];

/* CRC-16/CCITT: poly 0x1021, init 0xFFFF. */
static unsigned short
crc16_ccitt(const unsigned char *p, unsigned n)
{
	unsigned short crc = 0xFFFF;
	unsigned i, j;

	for (i = 0; i < n; i++) {
		crc ^= (unsigned short)p[i] << 8;
		for (j = 0; j < 8; j++)
			crc = (crc & 0x8000) ?
			    (unsigned short)((crc << 1) ^ 0x1021) :
			    (unsigned short)(crc << 1);
	}
	return crc;
}

int
cfg_load(struct nvs_state *s)
{
	unsigned short ver, crc, calc;
	unsigned i;

	/* Baca 2 sektor NVS dari SD. */
	for (i = 0; i < NVS_NSECTORS; i++)
		if (sys_sd_read(NVS_SECTOR + i, nvs_buf + i * SECTOR_SZ) != 0)
			return -1;

	/* Cek magic "QNVS". */
	if (nvs_buf[0] != 'Q' || nvs_buf[1] != 'N' ||
	    nvs_buf[2] != 'V' || nvs_buf[3] != 'S')
		return -1;

	/* Cek versi (little-endian). */
	ver = (unsigned short)nvs_buf[4] |
	      ((unsigned short)nvs_buf[5] << 8);
	if (ver != NVS_VERSION)
		return -1;

	/* Cek CRC dari byte offset 8 sampai akhir struct. */
	crc = (unsigned short)nvs_buf[6] |
	      ((unsigned short)nvs_buf[7] << 8);
	calc = crc16_ccitt(nvs_buf + 8, sizeof(struct nvs_state) - 8);
	if (crc != calc)
		return -1;

	/* Valid: salin struct ke pemanggil. */
	for (i = 0; i < sizeof(struct nvs_state); i++)
		((unsigned char *)s)[i] = nvs_buf[i];
	return 0;
}

int
cfg_save(const struct nvs_state *s)
{
	unsigned short crc;
	unsigned i;

	/* Bangun citra: struct + padding nol. */
	for (i = 0; i < sizeof(nvs_buf); i++)
		nvs_buf[i] = 0;
	for (i = 0; i < sizeof(struct nvs_state); i++)
		nvs_buf[i] = ((const unsigned char *)s)[i];

	/* Stempel magic + versi + crc (little-endian). */
	nvs_buf[0] = 'Q';
	nvs_buf[1] = 'N';
	nvs_buf[2] = 'V';
	nvs_buf[3] = 'S';
	nvs_buf[4] = NVS_VERSION & 0xFF;
	nvs_buf[5] = (NVS_VERSION >> 8) & 0xFF;
	crc = crc16_ccitt(nvs_buf + 8, sizeof(struct nvs_state) - 8);
	nvs_buf[6] = crc & 0xFF;
	nvs_buf[7] = (crc >> 8) & 0xFF;

	/* Tulis 2 sektor ke SD. */
	for (i = 0; i < NVS_NSECTORS; i++)
		if (sys_sd_write(NVS_SECTOR + i,
				 nvs_buf + i * SECTOR_SZ) != 0)
			return -1;
	return 0;
}
