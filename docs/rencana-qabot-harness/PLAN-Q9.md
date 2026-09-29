# PLAN — Q9 Spawn Process (v0.1 finish line)

**Track:** Qabot/QaonicOS · **Tanggal:** 2026-09-29 · **Status:** riset selesai, implementasi belum mulai
**Tujuan v0.1:** shell `qaon>` bisa launch program; `qabot` built-in tidak lagi stub.

## 0. Temuan riset

1. **Task creation ada:** `setup_uprog_task()` di `kernel/kernel/arm/user.c:1195`
   memakai `task_create()` + `thread_create()` + `pmap_enter()` untuk
   memetakan image ke `INIT_CODE_VA`. Dipakai saat boot untuk daemon
   (face, uiapp, qabotd).
2. **Exit handling ada:** `SYS_EXIT` → `sched_exit_switch()` membersihkan
   task. Task yang spawn bisa exit dengan bersih.
3. **Syscall 79 bebas:** 78 = FACE_EXPR (Q4). 79 belum dipakai.
4. **Program image:** user program (.bin) di-build ke `kernel/build/*.bin`.
   Untuk spawn runtime, image harus bisa dibaca dari FAT (bukan embedded).
   Kernel punya akses ke driver FAT (dipakai syscall 53/54/56).
5. **Console sharing:** sh dan child berbagi console. v1: parent (sh)
   blokir sampai child exit (model `system()`, bukan background job).

## 1. Desain

**Syscall 79 `SYS_SPAWN`:**
- `r0` = path program (mis. `/bin/hello`), `r1` = argumen (0 untuk v1)
- Return: exit code child, atau -1 bila gagal
- Kernel:
  1. Baca file dari FAT ke buffer kernel (batas 64KB untuk v1)
  2. `task_create()` + `thread_create()` (ikut pola `setup_uprog_task`)
  3. Petakan image ke memori user, set PC ke entry, SP ke stack
  4. Tandai parent sebagai "menunggu"; scheduler jalankan child
  5. Saat child `SYS_EXIT`, bangunkan parent, kembalikan exit code
  6. Bebaskan memori task child

**Shell (`user/sh.c`):**
- Built-in baru: `run <path>` → `sys_spawn(path)`
- `qabot` built-in: ganti stub → `sys_spawn("/bin/qabotd")`? 
  ATAU: qabotd tetap daemon; `qabot` di sh kirim prompt via flag.
  **Keputusan:** v1 = `run` generik; `qabot` tetap info (qabotd daemon
  persisten, tak perlu spawn ulang).

**Ulib:**
- `int sys_spawn(const char *path);` di `ulib.h`/`ulib.c`

## 2. Kriteria

- Di QEMU: `qaon> run /bin/hello` → program jalan, output muncul,
  kembali ke prompt `qaon>` dengan exit code.
- `qaon> run /bin/tdkyada` → error "gagal", shell tetap jalan.
- 3/3 run hijau, tanpa crash kernel.

## 3. Batasan v1 (jujur)

- Tanpa argumen ke child (r1=0); tanpa background job (`&`).
- Tanpa pipe/redirection.
- Parent blokir penuh (tak bisa Ctrl-C).
- Batas image 64KB.
- Q10 kandidat: argumen, background, job control.
