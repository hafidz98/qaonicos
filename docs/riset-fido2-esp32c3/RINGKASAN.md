# Riset Feasibility: Companion Passkey / FIDO2 Authenticator di ESP32-C3

Tanggal: 2026-09-28. Konteks arsitektur (asumsi tetap): Pico Mini (QaonicOS) =
face UI + agent harness; ESP32-C3 = co-MCU (WiFi/BLE/TLS/HTTP) via UART;
transport FIDO2 yang dievaluasi = **CTAP2 over BLE** di ESP32-C3.

## Ringkasan eksekutif

- **Feasible, tapi proyek medium-besar tersendiri** (bukan fitur kecil).
  Estimasi kasar: 2–4 bulan untuk satu embedded engineer berpengalaman bila
  mem-port stack open-source yang matang; lebih lama bila tulis CTAP2 dari nol.
- **Tidak ada stack FIDO2 yang "tinggal pakai" untuk ESP32-C3 + BLE.**
  Kandidat terbaik untuk di-port: **LionKey core** (C, CTAP 2.1, lolos uji
  konformansi FIDO, core portable, 1 dependensi) atau **canokey-core**
  (C11, Apache-2.0, sangat matang, dipakai produk komersial).
- ESP32-C3 **tidak punya akselerator ECC** (ECDSA P-256 jalan software via
  mbedTLS — puluhan ms, masih OK untuk autentikasi interaktif). Yang ada di
  hardware: TRNG, AES, SHA, RSA/big-int, HMAC, eFuse, flash encryption,
  secure boot.
- **Konsekuensi arsitektur penting**: rencana "firmware AT command tanpa
  effort" untuk co-MCU **gugur** — firmware ESP32-C3 harus custom ESP-IDF
  yang menggabungkan network coprocessor + authenticator FIDO2 dalam satu
  firmware (keduanya bisa koeksis: WiFi + BLE jalan bareng di C3).
- Rekomendasi: standalone roaming authenticator (CTAP2/BLE). "Hybrid/caBLE"
  adalah mekanisme sisi *client* (browser + HP) — authenticator kita otomatis
  diuntungkan tanpa implementasi khusus.

## 1. Kandidat stack FIDO2/CTAP2 open-source (C/C++)

| Proyek | Bahasa | CTAP | Lisensi | Kematangan | Cocok untuk C3? |
|---|---|---|---|---|---|
| **LionKey core** (pokusew/lionkey) | C | **2.1**, lolos uji konformansi FIDO | *cek repo langsung* (tidak terverifikasi di riset ini) | Usable, masih WIP (thesis 2025) | **Ya — kandidat utama.** Core MCU-independent, tanpa alokasi dinamis, 1 dep (TinyCBOR). Perlu: backend crypto → mbedTLS, transport USB → BLE |
| **canokey-core** (canokeys) | C11 | 2.1 (+U2F, OpenPGP, PIV, OATH) | **Apache-2.0** | Sangat matang (v3.1.0, dipakai produk CanoKey komersial) | Ya — kandidat kedua. Codebase lebih besar (banyak applet), tapi terstruktur (`ENABLE_IFACE_*`, tanpa kode HW-specific). Perlu backend crypto/storage + transport BLE |
| SoloKeys solo1 | C | 2.0-era | Dilaporkan MIT (verifikasi sebelum pakai) | Tua, kurang terawat | Referensi arsitektur saja (STM32F072, USB HID only) |
| OpenFIDO-ESP | C (ESP-IDF) | "Basic" 2.0 | **MIT** | Prototipe (badge build rusak, sinyal maintenance rendah) | Referensi pola ESP-IDF saja (NVS, TRNG, mbedTLS). Targetnya S2+USB, bukan C3+BLE |
| Zephyr subsys fido2 | C | 2.1 | Apache-2.0 | Sample resmi Zephyr, teruji | Tidak — butuh Zephyr RTOS, arsitektur sudah putuskan ESP-IDF |

Catatan porting (berlaku untuk LionKey maupun canokey-core):
- Ganti HAL crypto: STM32 PKA / asumsi generik → mbedTLS software
  (atau MPI hooks ESP-IDF untuk percepat big-int).
- Ganti transport: CTAPHID/USB → CTAP-over-BLE GATT (§2). Core CTAP2-nya
  (CBOR, perintah, state machine) bisa dipakai ulang hampir utuh.
- Storage credential → NVS terenkripsi / partisi custom (§3, §4).
- User presence: tombol fisik (standar) atau konfirmasi layar via UART (§6).

## 2. CTAP2 over BLE (sumber: CTAP 2.1 §11.4)

Authenticator wajib implementasi **FIDO GATT service, UUID `0xFFFD`**
(primary service), plus Device Information Service dan Generic Access Service.

| Characteristic | UUID | Properti | Fungsi |
|---|---|---|---|
| fidoControlPoint | `F1D0FFF1-DEAA-ECEE-B42F-C9BA7ED623BB` | Write (20–512 byte) | Buffer perintah: client tulis frame request |
| fidoStatus | `F1D0FFF2-DEAA-ECEE-B42F-C9BA7ED623BB` | Notify | Authenticator kirim frame respons (maks `ATT_MTU-3` per notifikasi) |
| fidoControlPointLength | `F1D0FFF3-DEAA-ECEE-B42F-C9BA7ED623BB` | Read (2 byte) | Ukuran tulis maksimum yang didukung |
| fidoServiceRevisionBitfield | `F1D0FFF4-DEAA-ECEE-B42F-C9BA7ED623BB` | Read/Write | Bitfield revisi protokol |
| fidoServiceRevision | `0x2A28` (standar) | Read | String revisi, mis. "1.0" |

- **Discovery**: client scan BLE → temukan service `0xFFFD` di advertising
  (atau connect lalu discover) → baca characteristics → tulis perintah.
- **Pairing**: opsional menurut spec. Tanpa pairing pun model ancaman aman:
  challenge selalu fresh dari RP, respons tidak bisa di-replay, sign counter
  deteksi kloning. (BLE pairing dengan MITM protection boleh ditambah,
  tapi bukan syarat.)
- **MTU & fragmentasi**: ATT_MTU default 23 (payload 20 byte) — **jangan
  asumsikan lebih** (iOS negosiasi konservatif). Spec mendefinisikan framing
  request/response dengan fragmentasi; implementasi wajib handle reassembly
  di kedua arah. Di ESP-IDF (NimBLE, disarankan karena ringan) MTU bisa
  dinegosiasi sampai 517, tapi kode harus benar di MTU minimum.
- Latensi BLE (~puluhan ms per round-trip) tidak masalah: WebAuthn interaktif,
  bukan real-time.

## 3. Crypto & secure storage di ESP32-C3

| Kebutuhan FIDO2 | Status di ESP32-C3 | Sumber |
|---|---|---|
| ECDSA P-256 (sign/verify) | **Software** (mbedTLS). **Tidak ada** peripheral ECC (`SOC_ECC_SUPPORTED=0`); RSA/big-int accel hanya bantu parsial via MPI hooks | noxtls ESP-IDF port README, ESP32-C3 datasheet §3.9 |
| SHA-256 | **Hardware** (SHA-1/224/256) | datasheet §3.9 |
| TRNG | **Hardware** RNG (`esp_random()`) — wajib dipakai untuk keygen, jangan PRNG software | ESP-IDF |
| AES (credential wrapping) | **Hardware** AES-128/256 | datasheet §3.9 |
| Secure boot | **Ada** (Secure Boot V2, skema RSA; cegah firmware tak-signed) | ESP-IDF security example (C3) |
| Flash encryption | **Ada** (XTS-AES-128, kunci 256-bit di 1 blok eFuse, software-inaccessible) | ESP-IDF docs |
| NVS terenkripsi | **Ada** (skema HMAC: kunci derivasi dari HMAC key di eFuse, **tanpa** perlu flash encryption) | ESP-IDF docs v5.2/v5.3 |
| eFuse key blocks | 6 blok; 1 untuk flash enc, 1 untuk HMAC NVS — cukup | ESP-IDF |
| Anti-tamper | eFuse `DISABLE_JTAG`, `DISABLE_ROM_DL_MODE` (irreversibel, hati-hati) | praktik umum ESP-IDF |

- **Attestation cert**: untuk v1 pakai **self attestation** (surrogate) —
  diterima mayoritas RP; persis pola passkey hasil sync (iCloud/Google pakai
  `none` + AAGUID tetap). **Jangan** memalsukan AAGUID Yubico/orang lain.
  Generate satu AAGUID tetap milik proyek, hardcode. Basic attestation
  (batch cert + listing di FIDO Metadata Service) = kerja sertifikasi
  tersendiri, tunda sampai ada kebutuhan enterprise.
- Kunci wrapping credential (AES) harus diturunkan dari secret di eFuse
  (via HMAC), **jangan** hardcode di flash.

## 4. Passkey (discoverable credential / resident key)

- **Non-discoverable** (server menyimpan credential ID): dengan key wrapping
  (private key dienkripsi AES → jadi credential ID), kapasitas **praktis
  tak terbatas** — pola yang dipakai OpenFIDO-ESP & SoloKeys.
- **Discoverable / resident** (passkey beneran, login tanpa username):
  tiap kredensial ~100–300 byte (keypair P-256 96B + rpId + userHandle +
  flags + sign counter). Realistis **25–100 kredensial** di partisi
  64–128KB (pembanding: YubiKey 5 = 25, generasi baru = 100). Butuh partisi
  flash dedicated + record store sederhana (NVS bisa, tapi partisi raw +
  wear-leveling sendiri lebih terkontrol).
- **Implikasi UX**: saat `getAssertion` dengan allowList kosong (login
  usernameless), device **wajib** tampilkan account picker → di sinilah layar
  240×240 bersinar (daftar akun per rpId, pilih pakai tombol).

## 5. Opsi arsitektur: standalone vs hybrid/caBLE

| | A. Standalone roaming authenticator (rekomendasi) | B. "Phone-assisted" custom |
|---|---|---|
| Cara kerja | ESP32-C3 = GATT server CTAP2/BLE. Browser/HP apa pun yang support WebAuthn langsung pakai | Device bicara ke HP via BLE custom, HP yang relay WebAuthn |
| Effort | Sedang-besar (port core + transport BLE) | Besar + rapuh (butuh companion app, relay protocol sendiri) |
| UX | Standar: "gunakan security key" di browser | Tidak standar, user bingung |
| Phishing resistance | Penuh (origin binding di CTAP2) | Berisiko terdegradasi |
| caBLE/hybrid | **Otomatis diuntungkan**: hybrid adalah flow sisi client (browser tampilkan QR → HP scan → terowongan Noise ke *client*). Authenticator BLE kita tetap opsi "security key" biasa | — |

Kesimpulan: **opsi A**. Tidak ada yang perlu diimplementasikan khusus untuk
"companion" — kata "companion" di sini artinya *perangkat pendamping* (seperti
"gunakan HP sebagai security key"), dan itu persis peran roaming
authenticator BLE.

## 6. Display confirmation (pola hardware-wallet)

- CTAP2 tidak punya kanal display — tapi request membawa `rp.name`, `rpId`,
  dan `user.displayName`. **Tampilkan `rpId` (domain registrable) secara
  menonjol** — ini garis pertahanan anti-phishing (user melihat "githvb.com"
  bukan "github.com").
- Alur: ESP32 terima `makeCredential`/`getAssertion` → kirim
  `CONFIRM_REQ{rpId, user, action}` via UART → QaonicOS tampilkan layar
  konfirmasi (face + teks) → user tekan tombol fisik → `CONFIRM_OK/DENY`
  via UART → ESP32 lanjutkan/tolak.
- **Batas trust UART**: link UART di dalam enclosure dianggap terpercaya,
  tapi **jangan kirim private key** lewat UART — hanya metadata + approve/deny.
- v1 boleh lebih sederhana: user presence = **tombol fisik di ESP32**
  (UX security key standar), konfirmasi layar via UART = v2.
- ClientPIN (PIN diketik di *browser*, diverifikasi authenticator) tidak butuh
  keypad di device — tetap support penuh.

## 7. Estimasi effort, fase, dan risiko

### Fase

| Fase | Lingkup | Estimasi |
|---|---|---|
| P0 — Spike | BLE GATT service `0xFFFD` + echo; uji baca/tulis dari `nRF Connect` / libfido2 | 1–2 minggu |
| P1 — CTAP2 inti | Port core (LionKey/canokey): `getInfo`, `makeCredential`, `getAssertion`, `getNextAssertion`, `reset`; ECDSA SW; self attestation; key wrapping; kredensial non-discoverable | 4–8 minggu |
| P2 — Passkey penuh | Resident key storage + account picker (UART→layar), clientPIN, display confirmation, auth config | 3–6 minggu |
| P3 — Hardening | Secure boot + flash/NVS encryption enable, interop Chrome/Edge/Firefox/Android/iOS, uji conformance FIDO, fuzzing CBOR | 4–8 minggu |

Total kasar: **3–6 bulan** satu engineer embedded berpengalaman (dengan port
core matang). Dari nol tanpa referensi: 2× lipat.

### Risiko & jebakan umum (jujur)

1. **Sign counter**: wajib monoton naik per kredensial; salah = false
   alarm kloning di RP atau celah replay. Jangan reset kecuali
   `authenticatorReset`.
2. **RNG**: keygen wajib `esp_random()` (HW TRNG). PRNG software = fatal.
3. **CBOR**: pakai encoder kanonis (TinyCBOR); tolak indefinite-length dari
   client yang strict.
4. **Credential wrapping**: kunci AES dari eFuse/HMAC, bukan hardcode.
   Rotasi kunci wrapping = wipe efektif.
5. **clientPIN retry**: 8× persisten; baca spec §6.5 baik-baik — salah
   implementasi = PIN bisa di-brute-force.
6. **Bit UP**: hanya set setelah presence fisik asli; jangan auto-UP.
7. **MTU 23**: uji dengan MTU minimum (iOS!) — bug fragmentasi = gagal
   interop misterius.
8. **AAGUID**: milik sendiri, tetap, jangan tiru vendor lain.
9. **BLE link drop** saat tunggu user presence — handle timeout dengan
   elegan, jangan hang.
10. **Scope creep firmware**: satu firmware ESP32-C3 kini menanggung
    WiFi + BLE + TLS/HTTP + FIDO2 — budgeting RAM (400KB) dan flash (4MB)
    harus disiplin sejak P0.
11. Sertifikasi FIDO = biaya & waktu tersendiri; **jangan janjikan** di v1.

## 8. Rekomendasi

1. **Stack**: port **LionKey core** (C, CTAP 2.1, lolos conformance, 1 dep)
   — verifikasi lisensinya di repo sebelum commit. Fallback: canokey-core
   (Apache-2.0, lebih besar tapi battle-tested).
2. **Transport**: CTAP2 over BLE (NimBLE) di ESP32-C3, sesuai keputusan.
3. **Firmware**: satu firmware ESP-IDF custom (network coprocessor + FIDO2);
   skema AT-command gugur untuk co-MCU.
4. **Attestation v1**: self attestation + AAGUID tetap milik proyek.
5. **UX**: account picker + konfirmasi rpId di layar 240×240 = diferensiasi
   vs security key buta.
6. **Jangan**: USB HID (ditunda sesuai keputusan), tulis CTAP2 dari nol,
   janjikan sertifikasi FIDO di v1.

## 9. Sumber

- LionKey — FIDO2 USB key, CTAP 2.1, C, core portable:
  https://github.com/pokusew/lionkey
- canokey-core — C11, Apache-2.0, FIDO2/U2F, tanpa kode HW-specific:
  https://github.com/canokeys/canokey-core/blob/HEAD/README.md
- SoloKeys (referensi arsitektur):
  https://github.com/solokeys
- OpenFIDO-ESP — MIT, ESP-IDF, prototipe (referensi pola):
  https://github.com/zequinha-taveira/openfido-esp
- Spesifikasi CTAP 2.1 (bagian 11.4 = CTAP over BLE):
  http://www.fmldo.org/pdf/fido-client-to-authenticator-protocol-v2.1-ps-20210615.pdf
- ESP32-C3 crypto accelerators (tidak ada ECC peripheral;
  `SOC_ECC_SUPPORTED=0` di C3):
  https://github.com/argenox/noxtls/blob/HEAD/ports/esp-idf/README.md
- Flash encryption ESP32-C3 (XTS-AES-128, kunci di eFuse):
  https://github.com/espressif/esp-idf/blob/1558b05d1c9/docs/en/security/flash-encryption.rst
- NVS encryption HMAC-based (tanpa perlu flash encryption):
  https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32c3/api-reference/storage/nvs_encryption.html
- Hybrid transport / caBLE (mekanisme sisi client):
  https://github.com/fourjuaneight/wiki/blob/HEAD/content/security/passkeys.md
- Attestation: self vs basic, AAGUID:
  https://fidoalliance.org/fido-technotes-the-truth-about-attestation/?query-cdbd12d0-page=60
- Praktik AAGUID tetap + `none`/self attestation (seperti passkey sync):
  https://github.com/chebizarro/nostrc/blob/HEAD/docs/plans/signet-passkeys-fido-2026-07-02.md
- Contoh desain CTAP2 di ESP32 (S3, USB) — pola interop & display prompt:
  https://github.com/generaldussduss/poseidon/blob/HEAD/docs/specs/2026-07-03-kerberos-phase2-ctap2-design.md
