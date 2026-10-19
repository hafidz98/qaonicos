/*
 * uartproto.h - protokol UART v1 QaonicOS <-> co-MCU ESP32-C3.
 *
 * RENCANA-app §4: text line-based, "CMD arg..\n" -> "OK data.." /
 * "ERR kode pesan".  Dipakai layar WiFi/BLE/LLM/Passkey di uiapp.
 *
 * Transport diabstraksi (uproto_xchg): di QEMU = mock co-MCU in-process
 * (mock_comcu.c, JUJUR: bukan hardware); di HW RV1103 = UART data PL011
 * (belum diimplementasi — titik sambung sudah disiapkan).
 */
#ifndef UARTPROTO_H
#define UARTPROTO_H

/* Kirim cmd (tanpa '\n'), baca satu respons.
 * resp diisi payload setelah "OK " (tanpa newline akhir), selalu
 * NUL-terminated.  Return 0 bila OK, -1 bila ERR/timeout/format jelek. */
int	uproto_cmd(const char *cmd, char *resp, unsigned rsz);

/* "CMD <arg>" (satu argumen, tanpa spasi di arg). */
int	uproto_cmd1(const char *cmd, const char *arg, char *resp,
		    unsigned rsz);

/* "CMD <arg1> <arg2>". */
int	uproto_cmd2(const char *cmd, const char *a1, const char *a2,
		    char *resp, unsigned rsz);

/* --- Data plane v2 (F5 §5): frame biner ---
 *
 * [magic(3B "AUD") len(2B BE) payload(PCM16 LE) crc(2B BE)]
 * CRC = CRC-16-CCITT (poly 0x1021, init 0xFFFF) atas magic+len+payload.
 */
#define	VOICE_FRAME_MAGIC0	'A'
#define	VOICE_FRAME_MAGIC1	'U'
#define	VOICE_FRAME_MAGIC2	'D'
#define	VOICE_FRAME_HDR		5u	/* magic(3) + len(2) */
#define	VOICE_FRAME_CRC		2u
#define	VOICE_MAX_PAYLOAD	1024u

unsigned	voice_crc16(const unsigned char *p, unsigned len);

/* VOICE.START <rate> <bits> -> 0, atau -1. */
int	uproto_voice_start(unsigned rate, unsigned bits);
/* VOICE.STOP -> jumlah frame yang diterima co-MCU, atau -1. */
int	uproto_voice_stop(void);
/* Kirim satu frame AUD. Return 0/-1. */
int	uproto_aud_frame(const unsigned char *pcm, unsigned len);

#endif /* UARTPROTO_H */
