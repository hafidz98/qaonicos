#!/usr/bin/env python3
"""mkfat32.py - Buat image FAT32 untuk kartu SD emulasi QaonicOS (Fase 16).

Tanpa tools eksternal (murni struct + file I/O).

Pemakaian:
    mkfat32.py create <img> [size_mb]   # buat image FAT32 baru (default 128MB)
    mkfat32.py check  <img>             # 0 bila sector 0 = boot sector FAT32 valid
    mkfat32.py ls     <img>             # list root dir + dump file contoh

Layout (128MB = 262144 sektor):
    sektor 0        : boot sector FAT32 (+ 0x55AA)
    sektor 1        : FSInfo, sektor 6/7: backup boot/FSInfo
    2 FAT @ 256 sektor, 8 sektor/cluster (4KB), root cluster = 2
    cluster TERAKHIR ditandai BAD (0x0FFFFFF7) -> sektor terakhir
    (262143) dicadangkan untuk magic superblock kernel "QAONSD01"
    (blk.c menulisnya tiap boot; FAT allocator kernel hanya memakai
    entri FAT == 0 sehingga cluster BAD tak pernah terpakai).
    cluster 3 (= data cluster bebas pertama) juga ditandai BAD:
    2 sektor pertamanya (LBA = data_start + 8) adalah area NVS untuk
    persistensi setting aplikasi; cmd_create mempertahankan isinya
    (bila bermagic "QNVS") saat image di-format ulang.

Isi awal:
    /HELLO.TXT            teks sambutan
    /DOCS/                direktori (+ entri . dan ..)
    /DOCS/NOTES.TXT       teks contoh
"""
import struct
import sys

SECTOR = 512


def fat_layout(total_sectors, spc=8, reserved=32, nfats=2):
    """Hitung ukuran FAT (sektor) dengan iterasi konvergen."""
    fatsz = 1
    for _ in range(8):
        data = total_sectors - reserved - nfats * fatsz
        clusters = data // spc
        need = (clusters + 2) * 4
        new = (need + SECTOR - 1) // SECTOR
        if new == fatsz:
            break
        fatsz = new
    data = total_sectors - reserved - nfats * fatsz
    clusters = data // spc
    return fatsz, clusters, reserved + nfats * fatsz


def name83(name):
    """'HELLO.TXT' -> (b'HELLO   ', b'TXT'). Uppercase, 8.3."""
    name = name.upper()
    if "." in name:
        base, ext = name.rsplit(".", 1)
    else:
        base, ext = name, ""
    base = (base[:8]).ljust(8)
    ext = (ext[:3]).ljust(3)
    ok_chars = lambda s: all(c.isalnum() or c in "_$%'-~!#()" for c in s)
    assert ok_chars(base.strip()) and ok_chars(ext.strip()), \
        "nama 8.3 tidak valid: %r" % name
    return base.encode("ascii"), ext.encode("ascii")


def dirent(name, attr, cluster, size):
    if name in (".", ".."):
        raw = name.ljust(11).encode("ascii")
    else:
        base, ext = name83(name)
        raw = base + ext
    e = bytearray(32)
    e[0:11] = raw
    e[11] = attr
    e[20:22] = struct.pack("<H", (cluster >> 16) & 0xFFFF)
    e[26:28] = struct.pack("<H", cluster & 0xFFFF)
    e[28:32] = struct.pack("<I", size)
    return bytes(e)


class FatImage:
    def __init__(self, total_sectors, spc=8):
        self.total = total_sectors
        self.spc = spc
        self.fatsz, self.clusters, self.data_start = fat_layout(total_sectors, spc)
        self.reserved = 32
        self.nfats = 2
        self.root_clust = 2
        self.img = bytearray(total_sectors * SECTOR)
        self.fat = [0] * (self.clusters + 2)
        self.next_free = 3  # cluster 2 = root

    def wsec(self, n, data):
        assert len(data) == SECTOR
        self.img[n * SECTOR:(n + 1) * SECTOR] = data

    def clus_sec(self, cluster, idx):
        return self.data_start + (cluster - 2) * self.spc + idx

    def alloc(self):
        while self.next_free < len(self.fat) and self.fat[self.next_free] != 0:
            self.next_free += 1
        assert self.next_free < len(self.fat), "FAT penuh"
        c = self.next_free
        self.fat[c] = 0x0FFFFFFF
        self.next_free += 1
        return c

    def nvs_reserve(self):
        """Cadangkan 1 cluster untuk NVS: alokasi cluster data bebas
        pertama via alloc(), tandai BAD (0x0FFFFFF7) di FAT (yang nanti
        ditulis ke KEDUA salinan). Kembalikan (cluster, LBA sektor
        pertama). Deterministik: dipanggil sebagai alloc() pertama di
        build() -> selalu cluster 3."""
        c = self.alloc()
        self.fat[c] = 0x0FFFFFF7
        lba = self.data_start + (c - 2) * self.spc
        return c, lba

    def chain_write(self, data):
        """Alokasi cluster secukupnya, tulis data, kembalikan cluster awal."""
        ncl = (len(data) + self.spc * SECTOR - 1) // (self.spc * SECTOR)
        first = prev = None
        off = 0
        for _ in range(ncl):
            c = self.alloc()
            if first is None:
                first = c
            if prev is not None:
                self.fat[prev] = c
            prev = c
            chunk = data[off:off + self.spc * SECTOR]
            chunk = chunk + b"\x00" * (self.spc * SECTOR - len(chunk))
            for i in range(self.spc):
                self.wsec(self.clus_sec(c, i), chunk[i * SECTOR:(i + 1) * SECTOR])
            off += self.spc * SECTOR
        return first

    def build(self):
        # --- boot sector ---
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xeb\x58\x90"
        bs[3:11] = b"QAONIC  "
        struct.pack_into("<H", bs, 11, SECTOR)      # bytes/sektor
        bs[13] = self.spc                            # sektor/cluster
        struct.pack_into("<H", bs, 14, self.reserved)
        bs[16] = self.nfats
        struct.pack_into("<H", bs, 19, 0)            # total16
        bs[21] = 0xF8                                # media
        struct.pack_into("<H", bs, 24, 63)           # sektor/track
        struct.pack_into("<H", bs, 26, 255)          # heads
        struct.pack_into("<I", bs, 32, self.total)   # total32
        struct.pack_into("<I", bs, 36, self.fatsz)   # FAT size 32
        struct.pack_into("<H", bs, 40, 0)            # ext flags
        struct.pack_into("<I", bs, 44, self.root_clust)
        struct.pack_into("<H", bs, 48, 1)            # FSInfo
        struct.pack_into("<H", bs, 50, 6)            # backup boot
        bs[64] = 0x80
        bs[66] = 0x29
        struct.pack_into("<I", bs, 67, 0x51414F53)   # volume id
        bs[71:82] = b"QAONIC SD  "
        bs[82:90] = b"FAT32   "
        struct.pack_into("<H", bs, 510, 0xAA55)
        self.wsec(0, bytes(bs))
        self.wsec(6, bytes(bs))  # backup

        # --- FSInfo (sektor 1 + backup 7) ---
        fi = bytearray(SECTOR)
        struct.pack_into("<I", fi, 0, 0x41615252)
        struct.pack_into("<I", fi, 484, 0x61417272)
        free = self.clusters - 2  # cluster 2 = root, cluster 3 = NVS (BAD)
        struct.pack_into("<I", fi, 488, free)
        struct.pack_into("<I", fi, 492, 3)
        struct.pack_into("<I", fi, 508, 0xAA550000)
        self.wsec(1, bytes(fi))
        self.wsec(7, bytes(fi))

        # --- FAT: cluster terakhir = BAD (cadangan sektor magic kernel) ---
        last = self.clusters + 1
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = 0x0FFFFFFF
        self.fat[self.root_clust] = 0x0FFFFFFF
        self.fat[last] = 0x0FFFFFF7  # BAD -> tak pernah dialokasi

        # --- NVS: cluster data bebas pertama dicadangkan untuk setting
        # aplikasi (2 sektor pertama dipakai, sisa cluster tak terpakai).
        # Ditandai BAD agar allocator FAT kernel tak pernah memakainya.
        self.nvs_cluster, self.nvs_lba = self.nvs_reserve()
        print("NVS: cluster %d -> LBA %d (2 sektor)" %
              (self.nvs_cluster, self.nvs_lba))

        # --- root dir: HELLO.TXT + DOCS/ ---
        hello = b"Halo dari QaonicOS! Ini file di kartu SD (FAT32).\r\n"
        c_hello = self.chain_write(hello)

        # direktori DOCS (cluster sendiri, dengan . dan ..)
        c_docs = self.alloc()
        notes = b"Catatan contoh di subdirektori FAT32 QaonicOS.\r\nBaris kedua.\r\n"
        c_notes = self.chain_write(notes)
        dents = bytearray(self.spc * SECTOR)
        dents[0:32] = dirent(".", 0x10, c_docs, 0)
        dents[32:64] = dirent("..", 0x10, self.root_clust, 0)
        dents[64:96] = dirent("NOTES.TXT", 0x20, c_notes, len(notes))
        for i in range(self.spc):
            self.wsec(self.clus_sec(c_docs, i), bytes(dents[i * SECTOR:(i + 1) * SECTOR]))

        root = bytearray(self.spc * SECTOR)
        root[0:32] = dirent("QAONICSD", 0x08, 0, 0)  # volume label
        root[32:64] = dirent("HELLO.TXT", 0x20, c_hello, len(hello))
        root[64:96] = dirent("DOCS", 0x10, c_docs, 0)
        for i in range(self.spc):
            self.wsec(self.clus_sec(self.root_clust, i),
                      bytes(root[i * SECTOR:(i + 1) * SECTOR]))

        # --- tulis FAT (2 copy) ---
        for f in range(self.nfats):
            base = self.reserved + f * self.fatsz
            raw = bytearray(self.fatsz * SECTOR)
            for i, v in enumerate(self.fat):
                struct.pack_into("<I", raw, i * 4, v)
            for i in range(self.fatsz):
                self.wsec(base + i, bytes(raw[i * SECTOR:(i + 1) * SECTOR]))

        return bytes(self.img)


def is_fat32(path):
    try:
        with open(path, "rb") as f:
            s0 = f.read(SECTOR)
    except OSError:
        return False
    return (len(s0) == SECTOR and s0[510] == 0x55 and s0[511] == 0xAA
            and s0[82:90] == b"FAT32   ")


def cmd_create(path, size_mb):
    total = size_mb * 1024 * 1024 // SECTOR
    # NVS: hitung LBA dari layout (instans probe, tanpa build) agar
    # sektor lama bisa dibaca SEBELUM image fresh menimpa file.
    _, nvs_lba = FatImage(total).nvs_reserve()
    saved = None
    try:
        with open(path, "rb") as f:
            f.seek(nvs_lba * SECTOR)
            data = f.read(2 * SECTOR)
            if len(data) == 2 * SECTOR and data[:4] == b"QNVS":
                saved = data
    except OSError:
        pass  # file belum ada -> tidak ada yang di-preserve
    img = FatImage(total).build()
    with open(path, "wb") as f:
        f.write(img)
    if saved is not None:
        # Tulis kembali HANYA bila bermagic QNVS: data NVS selamat dari
        # reformat, FS tetap fresh/deterministik untuk test usd/ufs.
        with open(path, "r+b") as f:
            f.seek(nvs_lba * SECTOR)
            f.write(saved)
        print("NVS: preserve 2 sektor @ LBA %d" % nvs_lba)
    print("FAT32 image: %s (%d MB, %d sektor, %d cluster, FAT %d sektor)" %
          (path, size_mb, total, FatImage(total).clusters,
           FatImage(total).fatsz))


def read_chain(img, data_start, spc, fat, cluster, size):
    out = bytearray()
    while cluster >= 2 and cluster < 0x0FFFFFF7 and len(out) < size:
        for i in range(spc):
            s = data_start + (cluster - 2) * spc + i
            out += img[s * SECTOR:(s + 1) * SECTOR]
        cluster = fat[cluster] & 0x0FFFFFFF
        if cluster >= 0x0FFFFFF8:
            break
    return bytes(out[:size])


def cmd_ls(path):
    with open(path, "rb") as f:
        img = f.read()
    s0 = img[:SECTOR]
    spc = s0[13]
    reserved = struct.unpack_from("<H", s0, 14)[0]
    nfats = s0[16]
    fatsz = struct.unpack_from("<I", s0, 36)[0]
    root = struct.unpack_from("<I", s0, 44)[0]
    data_start = reserved + nfats * fatsz
    fat = struct.unpack("<%dI" % (fatsz * SECTOR // 4),
                        img[reserved * SECTOR:(reserved + fatsz) * SECTOR])

    def listdir(cluster, prefix):
        c = cluster
        seen = 0
        while 2 <= c < 0x0FFFFFF7 and seen < 64:
            seen += 1
            for i in range(spc):
                sec = img[(data_start + (c - 2) * spc + i) * SECTOR:]
                for o in range(0, SECTOR, 32):
                    e = sec[o:o + 32]
                    if e[0] == 0x00:
                        return
                    if e[0] == 0xE5 or e[11] == 0x0F or e[11] == 0x08:
                        continue
                    nm = e[0:8].decode("ascii").rstrip()
                    ex = e[8:11].decode("ascii").rstrip()
                    full = nm + ("." + ex if ex else "")
                    cl = struct.unpack_from("<H", e, 26)[0] | \
                        (struct.unpack_from("<H", e, 20)[0] << 16)
                    sz = struct.unpack_from("<I", e, 28)[0]
                    isdir = bool(e[11] & 0x10)
                    print("%s%s%s  (cluster %d, %d byte)" %
                          (prefix, full, "/" if isdir else "", cl, sz))
                    if isdir and full not in (".", ".."):
                        data = read_chain(img, data_start, spc, fat, cl, sz)
                        listdir(cl, prefix + full + "/")
            c = fat[c] & 0x0FFFFFFF
            if c >= 0x0FFFFFF8:
                break

    print("root cluster %d, %d sektor/cluster:" % (root, spc))
    listdir(root, "/")
    # dump HELLO.TXT
    c = root
    while 2 <= c < 0x0FFFFFF7:
        for i in range(spc):
            sec = img[(data_start + (c - 2) * spc + i) * SECTOR:]
            for o in range(0, SECTOR, 32):
                e = sec[o:o + 32]
                if e[0] == 0x00:
                    break
                if e[0:11] == b"HELLO   TXT":
                    cl = struct.unpack_from("<H", e, 26)[0]
                    sz = struct.unpack_from("<I", e, 28)[0]
                    print("--- /HELLO.TXT ---")
                    print(read_chain(img, data_start, spc, fat, cl, sz)
                          .decode("ascii", "replace"))
                    return
        c = fat[c] & 0x0FFFFFFF
        if c >= 0x0FFFFFF8:
            break


def main(argv):
    if len(argv) < 3 or argv[1] not in ("create", "check", "ls"):
        sys.exit("pakai: mkfat32.py create|check|ls <img> [size_mb]")
    if argv[1] == "create":
        cmd_create(argv[2], int(argv[3]) if len(argv) > 3 else 128)
    elif argv[1] == "check":
        sys.exit(0 if is_fat32(argv[2]) else 1)
    else:
        cmd_ls(argv[2])


if __name__ == "__main__":
    main(sys.argv)
