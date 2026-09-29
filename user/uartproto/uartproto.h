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

#endif /* UARTPROTO_H */
