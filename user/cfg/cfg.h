/*
 * cfg.h - Persistensi setting aplikasi (NVS) di kartu SD.
 *
 * Setting (KV LLM, jaringan WiFi tersimpan, state BLE) disimpan di
 * 2 sektor khusus di kartu SD (area NVS: cluster 3 ditandai bad
 * cluster oleh tools/mkfat32.py) agar selamat dari reboot dan dari
 * format ulang FAT32 oleh run-qemu.sh.
 *
 * Layout: struct nvs_state (868 byte) di awal sektor NVS_SECTOR,
 * sisanya padding nol. crc16 = CRC-16/CCITT (poly 0x1021, init
 * 0xFFFF) dihitung dari byte offset 8 sampai akhir struct.
 */
#ifndef QAONIC_CFG_H
#define QAONIC_CFG_H

#define NVS_SECTOR	552	/* LBA area NVS (hasil probe mkfat32.py) */
#define NVS_NSECTORS	2
#define NVS_MAGIC	"QNVS"
#define NVS_VERSION	1

#define NVS_KV_MAX	8

struct nvs_kv {
	char	key[32];
	char	val[64];
};					/* 96 byte */

struct nvs_state {
	char		magic[4];
	unsigned short	version;
	unsigned short	crc16;
	unsigned char	kv_count;
	unsigned char	ble_on;
	unsigned char	wifi_nsaved;
	unsigned char	_pad;
	char		ble_name[24];
	char		wifi_saved[2][32];
	struct nvs_kv	kv[NVS_KV_MAX];
};					/* 868 byte < 1024 */

/* Muat state dari SD: 0 ok, -1 bila kosong/rusak (magic, versi, crc). */
int	cfg_load(struct nvs_state *s);

/* Simpan state ke SD (hitung crc dulu): 0 ok, -1 gagal tulis. */
int	cfg_save(const struct nvs_state *s);

#endif /* QAONIC_CFG_H */
