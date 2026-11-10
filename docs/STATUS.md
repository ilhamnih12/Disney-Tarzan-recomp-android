# Status: belum bisa dimainkan

Ini bukan port yang bisa dimainkan. Tidak ada RT64, tidak ada Vulkan, dan tidak ada angka FPS perangkat. Layar tidak punya tombol Play.

## Yang baru diukur

Tes host `python3 tools/test_checks.py` lulus:

- SHA-256 `abc` cocok dengan vektor standar.
- ROM lokal NTSC-U dikenali. Buffer pendek, buffer nol, dan ROM yang diubah satu byte ditolak. File ROM tidak disalin ke APK.
- Tulis atomik `tarzan.eep.tmp` lalu `rename` menghasilkan 512 byte yang sama, dan file `.tmp` tidak tertinggal. Direktori yang tidak ada gagal tanpa dianggap sukses.
- Zona sentuh 1280x720 dan 800x480 tidak saling tindih, tetap di dalam layar, dan target tombol minimal 44x28. Titik di A hanya mengirim A. D-pad atas mengirim stick positif. Titik di luar zona mengosongkan input.

Host game, build ini, ROM lokal, bukan perangkat:

- 6 detik: `vi=356 gfx=0 audio=361 tris=0 fills=0 vtx=0 tex=0 unresolved=0 dma=579776 unknown_cmd=0 threads=7 unimpl=11`.
- 12 detik: `vi=714 gfx=0 audio=718 tris=0 vtx=0 dma=579776`.
- Tunggu frame tetap jalan pada run 6 detik: nilai tersimpan maju, misalnya 5 lalu 7. Tugas SP yang tercatat bertipe 2 (audio). Tidak ada tugas grafis tipe 1.
- PPM run ini hitam 320x240 karena tidak ada frame grafis yang diterbitkan. Jangan dibaca sebagai gambar permainan.
- Permainan tidak menulis EEPROM pada run itu. Tes atomik di atas menguji fungsi tulis, bukan save di dalam game.

## Shell Android

`artifacts/tarzan-experimental.apk` adalah APK lokal NativeActivity ARM64, `minSdkVersion` 24, ditandatangani JAR v1 dengan kunci debug. Bukan tanda tangan rilis.

- Entri ZIP: manifest, `lib/arm64-v8a/libtarzan.so`, `META-INF`. Tidak ada `.z64`.
- `android.app.lib_name=tarzan`. Tidak ada string Play di library.
- Layar berbahasa Indonesia: belum bisa dimainkan, ROM tidak ada di aplikasi, HLE F3DEX2 bukan RT64/Vulkan, kontrol bukan tombol main, fokus hilang menolkan input.
- ROM hanya dibaca dari penyimpanan privat `internalDataPath/tarzan.z64`, dan hanya jika SHA-256-nya cocok dengan NTSC-U `e56d09772489a1a7426693396e6cbb24c1bfcd0ced11b92bc1546b081e9f3c06`.
- Sentuh: D-pad, stick, A, B, Z, Start, L, R, dan C bila muat. Gamepad tombol juga dipetakan. `APP_CMD_LOST_FOCUS` dan `APP_CMD_PAUSE` menolkan input dan menghentikan tick VI host. Belum diuji di ponsel.
- APK lokal ini memuat CPU hasil rekompilasi dari ROM pengguna. Jangan dipublikasikan. Lihat `docs/NOTICE.md`.

## Yang terukur sebelumnya, tidak diulang build ini

Pada build host yang lebih lama, satu tugas tipe 1 sempat masuk (`SP 800E4268`, data `800A7130`). Histogramnya perintah setup F3DEX2, bukan vertex `0x01` atau segitiga `0x05`. PPM saat itu satu warna `fffff6`. Build ini tidak mengulang tugas itu dalam 6 atau 12 detik, jadi decoder F3DEX2 belum terukur pada daftar perintah hidup.

## Yang belum

- Tidak ada uji perangkat, emulator Android, menu, level, input di ponsel, atau FPS.
- RSPRecomp, N64ModernRuntime, RT64, dan Vulkan tidak terpasang. NDK yang ada adalah r15.
- Empat fungsi hasil rekompilasi tetap stub karena batas fungsi salah.
- Audio host tetap kira-kira satu tugas per VI, bukan bukti suara permainan.
- Picker SAF tidak ada. Tidak ada Java/Gradle di lingkungan ini.

## Cara membangun

```text
python3 tools/test_checks.py
python3 tools/build_host.py
python3 tools/build_android.py
PYTHONPATH=.toolchains/python python3 tools/pack_apk.py
```

NDK clang membutuhkan `libncurses.so.5` hanya di `LD_LIBRARY_PATH` proses itu. Jangan mengekspor `.toolchains/compat` ke shell: symlink `libtinfo.so.6` merusak bash.
