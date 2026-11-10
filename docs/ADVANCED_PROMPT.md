# Advanced implementation prompt — Disney’s Tarzan: native Android recompilation

Dokumen ini dibuat **sebelum implementasi**. Gunakan sebagai kontrak kerja, bukan sebagai klaim bahwa fitur sudah selesai. Hasil aktual dan batasannya wajib dicatat dalam `docs/STATUS.md`.

## 1. Peran dan misi

Bertindak sebagai engineer C++20, Android NDK, Vulkan, dan reverse-engineering N64. Bangun fondasi port **Disney’s Tarzan (Nintendo 64, NTSC-U)** untuk Android yang menjalankan kode hasil recompilation secara native pada ARM64. Jangan menyamarkan emulator, streaming video, WebView game, atau layar demo sebagai game yang sudah direcompile.

Repo awal hanya berisi arsip ROM pengguna. Keberadaan ROM tidak berarti symbol map, runtime replacement, atau port yang langsung playable sudah tersedia. Jangan menghapus atau mengubah arsip asli. Jangan menambahkan salinan ROM, aset hasil ekstraksi, atau kode hasil translasi ROM ke distribusi aplikasi.

## 2. Tool yang diminta dan identitas upstream

Tautan pengguna:

`https://sourceforge.net/projects/n64-recompiled.mirror/files/latest/download?hem=9d685acd4a3ec55a0aed7a97cfd410be2066b58d51b9a3c324de29f81cd7d776`

Mirror mengidentifikasi dirinya sebagai mirror tidak resmi dari `https://github.com/N64Recomp/N64Recomp`. Halaman file mirror menunjuk **RecompModTool**, alat untuk membuat mod, bukan executable N64Recomp/RSPRecomp untuk membangun port. Jangan menjalankan atau mengandalkan `.exe` tersebut sebagai compiler Android.

Gunakan source upstream yang benar, pin commit yang dipakai, build `N64Recomp` dan `RSPRecomp` dari source, dan pertahankan lisensi serta atribusi. Jika arsip mirror ternyata berbeda dari metadata halaman, verifikasi isinya sebelum digunakan. Jangan menyebut binary yang tidak diverifikasi sebagai binary resmi.

## 3. Definisi native dan batas kejujuran teknis

- CPU: MIPS game diterjemahkan ahead-of-time menjadi C oleh N64Recomp, lalu dikompilasi NDK menjadi ARM64. Tidak ada interpreter CPU atau JIT emulator pada jalur gameplay.
- Metadata fungsi, jump table, overlay, dan entrypoint harus sesuai ROM target. Jangan menganggap semua ROM N64 dapat dijalankan tanpa analisis per game.
- GPU: gunakan RT64/Vulkan dan display list asli melalui N64ModernRuntime bila kompatibilitas memungkinkan. Rendering native bukan berarti seluruh perilaku konsol otomatis akurat.
- OS: replacement libultra untuk thread, message queue, VI, input, PI DMA, controller, dan penyimpanan. RSP/audio ditangani melalui microcode yang benar, bukan fungsi kosong yang mengaku sukses.
- Target awal: Android 9/API 28+, `arm64-v8a`, perangkat dengan Vulkan yang memenuhi kebutuhan renderer.
- “Full native performance” adalah tujuan arsitektur, **bukan janji FPS**. Ukur frame time, p95/p99, stutter, memori, dan thermal behavior pada perangkat nyata sebelum membuat klaim performa.
- Tidak ada perangkat Android fisik dalam lingkungan kerja awal. Build silang, test host, dan inspeksi ELF bukan pengganti uji gameplay pada ponsel.

## 4. Arsitektur yang dituju

```text
ROM milik pengguna (host build / impor lokal)
  → validasi identitas, ukuran, endian, checksum
  → N64Recomp + metadata Tarzan → C
  → NDK clang → kode CPU ARM64
  → N64ModernRuntime + adapter Tarzan
  → RT64/Plume/Vulkan + SDL Android
  → Activity, lifecycle, surface, audio, storage

Kontrol sentuh / gamepad → mapping N64 → runtime
Save game → penyimpanan privat aplikasi, tulis atomik
```

Pisahkan lapisan berikut agar dapat diuji:

1. **ROM ingestion:** parsing ZIP aman, normalisasi `.z64/.v64/.n64`, validasi hash, tidak mengekstrak path arbitrer.
2. **Game recipe:** konfigurasi N64Recomp/RSPRecomp, symbol map, lokasi microcode, dan patch yang spesifik Tarzan.
3. **Native runtime:** integrasi libultra, task scheduler, renderer, audio, dan data asli yang tetap berasal dari ROM pengguna.
4. **Android shell:** Kotlin/Android framework untuk picker, izin minimum, lifecycle, diagnostics, dan pengaturan; gameplay bukan implementasi Kotlin/WebView.
5. **Input dan persistensi:** multi-touch D-pad/analog, A/B/Z/Start, C-buttons, L/R, gamepad, pause/resume, dan save yang tahan interupsi.
6. **Build/test:** skrip reproducible, host checks, CI, pemeriksaan ABI/library, dan petunjuk build yang tidak bergantung pada file rahasia di sandbox.

## 5. Tahap pelaksanaan dan evidence

### A. Audit dan reproduksi toolchain

- Periksa ROM hanya untuk informasi teknis yang diperlukan; catat metadata tanpa mendistribusikan isinya.
- Ambil source berlisensi jelas dari GitHub dengan versi terpin; periksa kompatibilitas N64Recomp, runtime, RT64, Plume, dan SDL.
- Bootstrap compiler host; uji dengan input kecil sebelum memakai ROM target.
- Evidence: lockfile versi, perintah build, hasil test, hash tool input.

### B. Analisis Tarzan dan vertical slice CPU

- Identifikasi header, entrypoint, wilayah kode/data, kompresi, overlay, dan fungsi SDK/libultra.
- Gunakan symbol yang dapat dipertanggungjawabkan; heuristic discovery harus diberi label heuristic, bukan disamakan dengan decompilation yang sudah cocok byte-for-byte.
- Hasilkan C dengan tool asli dan kompilasi untuk host dan ARM64.
- Cari unresolved function/overlay dan failure yang nyata; jangan menghilangkannya melalui stub tanpa diagnosa.
- Evidence: laporan cakupan symbol, jumlah fungsi, output compiler/linker, trace boot sejauh yang benar-benar teruji.

### C. Grafis, audio, dan runtime Android

- Hubungkan display-list task ke RT64/Vulkan; periksa microcode dan framebuffer asli.
- Identifikasi audio microcode dan hubungkan output mixer ke audio Android melalui backend native yang sesuai.
- Implementasikan penggantian layanan OS yang dibutuhkan game. Jangan memakai memory access ke alamat konsol sebagai pointer host tanpa mapping yang benar.
- Tangani creation/loss surface, background/foreground, audio focus, dan penghentian thread dengan teruji.
- Evidence: log task/frame, screenshot perangkat jika tersedia, hasil cross-build dan pemeriksaan dynamic dependencies.

### D. Shell aplikasi dan kontrol

- Layar pembuka menjelaskan kebutuhan ROM dan status dukungan, bukan tombol Play palsu.
-Picker SAF, validasi sebelum penggunaan, progres/error yang dapat dipahami, tanpa izin storage luas atau internet yang tidak diperlukan.
- Layout sentuh landscape dengan target tekan memadai, pointer tracking, dan reset input ketika kehilangan fokus.
- Gamepad mapping dan pengaturan kualitas native yang benar-benar tersambung ke renderer.
- Import/export save yang eksplisit serta atomic write; jangan menjanjikan cloud save atau save state emulator.

### E. Packaging dan pengujian

- Build APK debug yang dapat dipasang untuk pengujian; release signing memakai kredensial pengguna, bukan kunci debug yang disamarkan sebagai release.
- Pisahkan shell berlisensi dari data game dan kode turunannya. ROM/aset tidak masuk APK publik.
- Jalankan unit test ROM parser, checksum, input state, save serialization, serta smoke test native.
- Uji lifecycle, rotasi, interupsi, gamepad disconnect, ROM salah, Vulkan tidak tersedia, dan kegagalan alokasi.
- Catat pengujian yang belum dijalankan sebagai belum dijalankan; jangan membuat angka FPS atau screenshot gameplay sintetis.

## 6. Prioritas keputusan

1. Kebenaran dan reproduktibilitas lebih penting daripada banyaknya toggle kosmetik.
2. Port Tarzan yang terukur lebih penting daripada klaim universal N64 loader.
3. Jika port lengkap tidak selesai, serahkan komponen yang benar-benar dibangun dan test, blocker spesifik, serta langkah berikutnya. Jangan mengubah scope diam-diam menjadi emulator lalu menyebut permintaan native terpenuhi.
4. Jangan mengklaim APK playable hanya karena Gradle sukses atau aplikasi menampilkan menu.
5. Setiap hambatan toolchain, lisensi, metadata, atau pengujian perangkat harus tampak dalam dokumentasi dan UI yang relevan.

## 7. Definition of done untuk rilis playable

- [ ] Tool N64Recomp/RSPRecomp dibangun dari source terpin.
- [ ] ROM target dikenali berdasarkan identitas yang telah diverifikasi.
- [ ] Kode CPU hasil recompilation terkompilasi native ARM64 tanpa stub kritis yang disembunyikan.
- [ ] Boot, menu, level yang diuji, input, grafis, audio, dan save telah diuji dengan ROM yang sah.
- [ ] Lifecycle dan kontrol kehilangan fokus tidak meninggalkan input tertahan atau thread liar.
- [ ] APK tidak menyertakan ROM/aset game; lisensi dan source notice lengkap.
- [ ] Build dapat direproduksi dari checkout bersih dengan dependensi yang didokumentasikan.
- [ ] Performa diukur pada perangkat nyata; tidak ada klaim “full speed” tanpa evidence.
- [ ] `docs/STATUS.md` membedakan selesai, teruji sebagian, belum teruji, dan terblokir.

## 8. Cara melaporkan hasil

Laporkan dalam bahasa Indonesia: file yang dibuat, perintah yang benar-benar berhasil, artefak yang tersedia, test yang lulus/gagal, dan batas kompatibilitas. Tunjukkan dokumen ini serta artefak utama. Jangan menyebut pekerjaan selesai hanya karena scaffold tersedia.

Dokumen boleh diperbarui dengan temuan baru melalui bagian **Catatan pelaksanaan**, tetapi definisi native dan larangan klaim palsu tidak boleh dilemahkan.

## Catatan pelaksanaan

Ini catatan, bukan perubahan definisi selesai.

- N64Recomp host terpin `598ce208` dan dipakai untuk menghasilkan C. RSPRecomp, RT64, Plume, dan Vulkan tidak terpasang. NDK yang tersedia adalah r15, API 24, bukan API 28.
- Satu tugas grafis tipe 1 pernah terlihat di host sebelum interpreter F3DEX2. Isinya perintah setup (`E3`, `DB`, `DE`, `DF`), tanpa vertex atau segitiga. PPM saat itu satu warna. Build host berikutnya, 6 detik dan 12 detik, tidak mengulang tugas tipe 1: `gfx=0`, audio kira-kira satu per VI, DMA tetap 579776. Itu bukan bukti adegan.
- Shell Android adalah NativeActivity, bukan picker SAF. ROM tidak dibundel. Kontrol sentuh dan reset input saat fokus hilang ada di kode, belum diuji di perangkat.
- Definition of done playable di bagian 7 tetap tidak terpenuhi.
