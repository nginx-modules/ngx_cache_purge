# ngx_cache_purge

[README](README.id.md) · [Direktif](DIRECTIVES.id.md)

<a id="execution-model"></a>

## Model eksekusi

**Tugas** adalah satu operasi wildcard atau `purge_all`. **Batch** adalah jumlah tugas per worker sebelum jeda. **Langkah** adalah satu bagian penelusuran inkremental yang dibatasi; satu tugas dapat memerlukan banyak langkah.

Dengan generasi, satu penyapuan fisik per jalur/namespace menggabungkan seluruh invalidasi yang diterima. Halaman REFRESH adalah operasi HTTP terpisah, bukan tugas antrean purge.

| Permintaan dan konfigurasi | Penelusuran file | Penyelesaian HTTP |
|---|---|---|
| Kunci tepat lama, dengan/tanpa Vary | Worker permintaan; Vary menambah penelusuran | Setelah operasi |
| Wildcard atau `purge_all`, antrean mati | Worker permintaan, sinkron | Setelah penelusuran |
| Tugas latar, pool terpilih | Satu tugas pool per worker pada satu waktu | `202` setelah penerimaan |
| Tugas latar tanpa pool yang dapat digunakan | Event loop worker, langkah inkremental | `202` setelah masuk antrean |
| Kapasitas/alokasi habis | Permintaan menunggu penerimaan tanpa penelusuran sendiri | `202` setelah diterima |
| PURGE generasi, bentuk kunci apa pun | Penyapuan namespace tertunda; tanpa membuka/menghapus cache dalam permintaan | `200 invalidated` setelah publikasi dan penerimaan |
| Endpoint REFRESH | Pemindaian terbatas dan hingga 32 HEAD berurutan | JSON `200` dengan kolom penyelesaian/lanjutan |

Baris tanpa generasi menjelaskan perilaku lama. Permintaan generasi yang menunggu kapasitas mengembalikan `200`, bukan `202`, setelah diterima.

Langkah berhenti setelah paling banyak 64 iterasi direktori atau mengamati 5 ms berlalu, termasuk entri terlewati dan akhir direktori. Ini tidak membatasi durasi satu syscall.

Untuk memindahkan I/O latar ke thread, build dengan `--with-threads` dan deklarasikan pool inti dalam konteks **main**, di luar `http`:

```nginx
thread_pool default threads=2;
```

`cache_purge_thread_pool <name>` memilih pool yang ada dalam `http`, `server`, atau `location`. Bawaan `default` dibuat NGINX dengan parameter standar bila dirujuk tanpa deklarasi. Nama khusus wajib dideklarasikan lewat `thread_pool`. Pilihan diwariskan; permintaan pertama yang diterima menentukan eksekutor pekerjaan gabungan. Setiap worker memiliki pool sendiri. Pool penuh memicu pengiriman ulang setelah 10 ms dengan mempertahankan jatah penerimaan, tanpa beralih ke penelusuran sinkron. Tanpa thread, pada Windows, mode proses tunggal, atau jika reload menghapus pool tugas lama, kursor inkremental digunakan.

REFRESH memakai pool terpilih untuk satu pemindai tambahan per worker. Subrequest HTTP native berjalan di event loop; thread I/O tidak mengakses statusnya.

Perubahan struktur antrean/indeks memakai mutex slab. Penerimaan dan callback latar memakai trylock serta pengulangan bertimer. Tugas tertunda **dan berjalan** tetap di rbtree string inti per jalur; perbandingan kunci lengkap mengatasi benturan hash. Biaya indeks `O(log Q)` untuk `Q` tugas belum selesai; pencarian jalur linear terhadap jumlah jalur terdaftar. Daftar `ngx_queue_t` melacak tugas tertunda/aktif. Duplikat aktif pertama meminta pengulangan, berikutnya bergabung. Pengulangan masuk ekor antrean setelah putaran kini agar PURGE berikutnya tidak hilang diam-diam. Purge terus-menerus dapat memicu putaran tambahan. Jatah baru dilepas pada penyelesaian final. I/O dan format respons di luar kunci.

Rbtree hanya mendeduplikasi pembersihan fisik. Aturan generasi berada dalam trie byte terbatas di SHM terpisah. Penyapuan namespace ketat mempunyai identitas indeks berbeda dari tugas kunci lama; keputusan REFRESH memakai jalur pembersihan yang sama.

Antrean penuh atau kegagalan alokasi membuat HTTP menunggu dengan timer 10 ms dan penanganan putus koneksi inti. Permintaan/handler pembersihan tetap di worker pemilik; SHM tidak berisi pointer permintaan. Pembacaan seluruh koneksi HTTP/2 dan HTTP/3 tetap aktif. Tekanan antrean tidak menghasilkan 429/503; PURGE pendek yang telah dibaca tidak menjamin TCP Zero Window. Penantian tetap memakan memori/koneksi: gunakan timeout dan kebijakan penerimaan inti sesuai anggaran koneksi.

Tugas diterima mempertahankan kunci, nama pool, dan perlindungan jalur temporer melewati reload biasa. Duplikat baru memperbarui perlindungan tugas tertunda atau putaran berikutnya. Menutup penerimaan baru mempertahankan pekerjaan lama. Worker yang berhenti secara anggun menyelesaikan putaran kini tanpa mengambil tugas baru. Keluar cepat mengembalikan tugas inkremental belum selesai ke antrean tanpa menuntaskan filesystem di hook keluar. Masa hidup tugas thread yang sudah dikirim dikelola penghentian pool inti.

Epoch PID/slot mengidentifikasi penugasan. Setiap detik worker memeriksa paling banyak 64 catatan aktif per putaran pemulihan, lalu mengantrekan ulang tugas yang pemiliknya keluar. Ini memulihkan penugasan terlantar, bukan jurnal transaksional. Crash saat memperbarui daftar/slab dapat merusak status dan memerlukan restart; penghapusan tepat sebelum crash dapat hilang dari metrik. Tugas tertunda/pengulangan butuh worker aktif: kosongkan antrean sebelum berhenti jika penyelesaian penting. Antrean tidak persisten.

Struktur internal antrean bukan ABI peningkatan. Build dengan struktur berbeda memerlukan restart penuh setelah pekerjaan yang wajib selesai dituntaskan. Reload build yang sama memakai ulang zona.

Penghapusan bersifat best effort. `202` mengonfirmasi penerimaan, bukan penghapusan; putaran selesai bukan snapshot atomik. Permintaan bersamaan dapat membuat entri selama/setelah putaran, dan kesalahan file dicatat. Wildcard/purge penuh menghapus tanpa memperbarui setiap node native secara sinkron. Operasi tepat dan Vary menginvalidasi node yang cocok.

Putaran latar gagal diulang setelah 100 ms, 1 s, dan 10 s, maksimal tiga kali. Tugas tertunda tetap memiliki slot penerimaan/deduplikasi sambil membiarkan tugas lain berjalan. PURGE baru mengatur ulang jendela ini. Setelah habis, tugas dihentikan dengan log error; periksa `cleanup.exhausted` dan ulangi setelah memperbaiki penyimpanan. Invalidasi generasi tetap berlaku walau pembersihan fisik gagal. File hilang adalah race yang diharapkan; file rusak dilewati tanpa pengulangan tak berujung.

<a id="reading-cache-files"></a>

### Membaca berkas cache dalam mode generasi

Penjelajah cache, pemuat awal, atau laporan penggunaan tidak dapat menyimpulkan validitas generasi hanya dari berkas: berkas usang dapat tetap ada setelah invalidasi, bahkan tanpa pengganti. Baris `KEY:` berkas generasi adalah `p4:<namespace>:<epoch>:<generation>:<logical key>`. [Endpoint status](DIRECTIVES.id.md#cache_purge_queue_status) menyediakan status namespace. Generasi aktif suatu kunci adalah `baseline`, kecuali aturan exact atau prefiks menaikkannya; aturan tersebut tidak disertakan dalam respons.

| Status namespace berkas | Kesimpulan |
|---|---|
| Namespace tidak ada dalam daftar | unknown |
| Epoch berkas berbeda dari `epoch` | stale |
| Epoch cocok; generasi berkas lebih kecil dari `baseline` | stale |
| Epoch cocok; generasi setidaknya `baseline`; `rules` bernilai `false` | live |
| Epoch cocok; generasi setidaknya `baseline`; `rules` bernilai `true` | unknown |

Di sini, **live** hanya berarti berkas tidak usang menurut generasi namespace; ini tidak membuktikan kesegaran konten, validitas TTL, atau HIT pada cache NGINX. Dengan `namespaces_complete=false`, entri yang dikembalikan tetap dapat digunakan dan namespace yang tidak disertakan dianggap tidak diketahui.

Berkas dengan generasi lebih tinggi, namespace, epoch, dan kunci logis yang sama membuktikan bahwa berkas bergenerasi lebih rendah sudah usang: generasi aktif tidak pernah menurun dalam satu epoch. Selain itu, pertahankan kesimpulan unknown.

Baca status sebelum dan sesudah pemindaian, cocokkan entri berdasarkan nama namespace. Gunakan kesimpulan berdasarkan status untuk suatu namespace hanya jika namespace ada dalam kedua respons dan `epoch`, `baseline`, serta `serial` tidak berubah. Status yang hilang atau berubah membatalkan kesimpulan untuk namespace tersebut. Bandingkan epoch hanya untuk kesamaan. Pengamatan ini tidak mencegah invalidasi berikutnya.

Antrean pembersihan kosong tidak membuktikan validitas. Permintaan yang dimulai sebelum invalidasi dapat menulis berkas generasi lama setelah pembersihan selesai. Berkas tetap ada sampai pembersihan berikutnya atau pengusiran oleh cache bawaan. Pembersihan juga hanya menargetkan jalur cache pada location PURGE, sedangkan satu namespace dapat dipakai oleh beberapa jalur.

---

<a id="performance-tuning"></a>

## Penyetelan kinerja

`queue_size` membatasi pekerjaan belum selesai, `batch_size` mengelompokkan tugas per worker, `throttle_ms` menjeda kelompok. Ukur latensi, perebutan disk dan usia antrean sebelum/sesudah mengubah. Kapasitas lebih besar tidak menaikkan laju pengosongan.

Preset adalah titik awal, bukan klaim kinerja perangkat keras. Nama kolom menghilangkan prefiks umum `cache_purge_`.

| Beban / anggaran penyimpanan | `queue_size` | `batch_size` | `throttle_ms` |
|---|---:|---:|---:|
| Host atau penyimpanan terbatas | 512 | 5 | 25ms |
| Titik awal umum | 2048 | 20 | 10ms |
| Penyimpanan cepat dengan ruang kapasitas terukur | 8192 | 50 | 5ms |
| Lonjakan singkat dengan pengosongan konservatif | 8192 | 5 | 50ms |

Contoh preset umum:

```nginx
cache_purge_background_queue on;
cache_purge_queue_size        2048;
cache_purge_batch_size        20;
cache_purge_throttle_ms       10ms;
```

Antrean besar menyerap lonjakan, tetapi kedatangan berkelanjutan melebihi penyelesaian tetap memenuhinya. Perkirakan ruang tambahan sebagai `max(0, arrival_rate - completion_rate) * burst_seconds` tugas dengan laju tugas/s setelah deduplikasi. Sisakan ruang bagi variasi durasi dan backlog. Menunggu menambah permintaan/koneksi terbuka; hitung memori dan FD saat penyimpanan jenuh.

<a id="memory-sizing"></a>

### Perhitungan memori

Ukuran zona dihitung dari ukuran struktur native:

```text
Q = cache_purge_queue_size; P = native page size
I = sizeof(ngx_http_cache_purge_queue_item_t)
H = sizeof(ngx_http_cache_purge_queue_t)
A = max(ngx_cacheline_size, 64)
R = align(sizeof(ngx_http_cache_purge_stats_t), A)
S = 2 * (I + 2 * (512 + 1))
M = sum over cache paths of
    2 * (sizeof(ngx_http_cache_purge_path_t) + path_length + 1
         + A + R * NGX_MAX_PROCESSES)
zone_bytes = max(8 * P, align(H + Q * S + M, P))
```

| Komponen | Memori dicadangkan/digunakan |
|---|---|
| Cadangan tugas | `Q * S`, termasuk pembulatan slab dan nama kunci/pool inline |
| Metrik satu jalur | `R * NGX_MAX_PROCESSES` dan alignment; 192 KiB dengan stride 192 byte serta 1024 slot |
| Kontrol antrean | `H`, termasuk kepemilikan proses dan array epoch |
| Cadangan jalur | `M`, termasuk nama, akar rbtree, alignment, array metrik |
| Zona minimum | Delapan halaman native |

Untuk layout LP64 terukur (`I=184`, `H=20576`, record jalur 104 byte, statistik 144), cache line 64 byte, halaman 4 KiB, dan satu jalur 20 byte:

| Kapasitas | Byte zona | MiB zona |
|---:|---:|---:|
| 512 | 1654784 | 1.58 |
| 2048 | 5373952 | 5.12 |
| 8192 | 20242432 | 19.30 |

Memori aktual tugas adalah `I + index_bytes + 1 + pool_name_bytes` sebelum pembulatan slab. Indeks tertanam berisi tag tindakan satu byte dan kunci lama atau nama namespace generasi. `purge_all` lama hanya menyimpan tag. Kapasitas membatasi alokasi tertunda/aktif bersama. Ukuran native, cache line, `NGX_MAX_PROCESSES` dan jalur menentukan hasil; Windows/32-bit berbeda. Perlindungan reload memakai SHM tambahan. Alokasi dapat gagal sebelum batas tugas.

Memori proses terpisah. Tiap penelusuran aktif memakai buffer kunci tetap, buffer jalur pakai ulang, frame tetap per direktori terbuka. Frame menyimpan panjang prefiks tanpa menyalin leluhur: `O(depth + longest_path)`, tak bergantung jumlah file. Stream direktori punya buffer platform. Permintaan menunggu menyimpan konteks timer/pembersihan kecil tanpa kursor. Stack thread dan pool inti memiliki biaya tersendiri.

Namespace memakai ukuran SHM tersendiri. Pemindai REFRESH memiliki buffer/konteks LP64 tetap 283864 byte (32 entri), frame dan maksimal 32 subrequest dalam pool HTTP. Halaman selesai membebaskan permintaan; kelanjutan memulai yang baru. Penunggu tambahan tidak mengalokasikan array kandidat.

<a id="throughput-and-latency"></a>

### Throughput dan latensi

Untuk worker yang terus sibuk, perkiraan berguna:

```text
B = tasks completed in a batch
T = throttle delay in milliseconds
D = elapsed batch processing time in milliseconds, including slice waits
rate_per_worker ~= 1000 * B / (D + T) tasks/s
```

Mengabaikan `D` memberi batas penjadwalan `1000 * B / T`: bawaan 1000 tugas/s per worker. Bukan laju penghapusan file atau hasil ukur. Sepuluh tugas berjumlah 200 ms dengan jeda 10 ms menghasilkan sekitar 47,6 tugas/s. Worker berbagi penyimpanan; mengalikan jumlah worker bukan jaminan. Worker menganggur dapat membutuhkan `10 * T` ditambah keterlambatan event loop untuk memeriksa tugas baru.

Kecilkan batch atau perpanjang jeda untuk mengurangi kerja antaristirahat. Gunakan pool opsional untuk latensi worker lebih rendah saat I/O. Keduanya tidak membatasi syscall atau membagi tugas thread menjadi batch.

---

<a id="monitoring-and-debugging"></a>

## Pemantauan dan debugging

[Endpoint status](DIRECTIVES.id.md#cache_purge_queue_status) menunjukkan tugas tertunda/aktif, penunggu, duplikat, penghapusan, HIT/MISS. `202 Accepted` mengonfirmasi penerimaan/penggabungan, bukan penghapusan atau invalidasi logis seketika. Pantau latensi bersama usia antrean dan perebutan penyimpanan.

| Level | Kondisi |
|---|---|
| `warn` | Kunci terlalu panjang, pool dihapus, pengaturan dibatasi |
| `error` | Akses ditolak, lookup/profil REFRESH gagal, percobaan habis; pool inti atau baca Windows gagal |
| `crit` | Buka/stat/hapus atau operasi direktori gagal; baca POSIX inti gagal |
| `alert` | Menutup file gagal |
| `emerg` | Konfigurasi salah, SHM tidak kompatibel, perubahan ukuran atau alokasi metadata reload gagal |
| `debug` | Konteks tambahan permintaan/penelusuran |

Diagnostik memakai fungsi logging NGINX native dan prefiks `ngx_cache_purge: `. NGINX menambahkan level, waktu, ID proses/thread, teks errno dan konteks permintaan/konfigurasi. Pesan file menyebut operasi gagal sebenarnya dan jalur dalam kutipan; namespace/REFRESH menyebut objek serta alasan penolakan. Gunakan parser error log NGINX standar dan filter prefiks.

Debug menggunakan `NGX_LOG_DEBUG_HTTP` serta memerlukan build `--with-debug` dan `error_log /var/log/nginx/error.log debug;`.

Tekanan antrean tampil sebagai metrik tunggu, bukan peringatan per percobaan. File yang telah hilang adalah race wajar tanpa diagnostik modul. Pesan helper inti — parser, allocator, I/O dan pool — mempertahankan format inti; periksa log lengkap untuk diagnosis.

```bash
tail -f /var/log/nginx/error.log | grep 'ngx_cache_purge:'
```

Lihat [pemeriksaan log](../../t/TESTING.md#logging-contracts) untuk diagnostik yang diuji.

---

<a id="troubleshooting"></a>

## Pemecahan masalah

| Gejala | Pemeriksaan/penyesuaian |
|---|---|
| Latensi worker saat penelusuran besar | Aktifkan antrean dan pool opsional untuk I/O. Tepat/Vary serta latar mati tetap sinkron. |
| Penunggu penerimaan bertambah | Periksa laju masuk, usia, pool, disk. Kapasitas ekstra menyerap lonjakan; perubahan ukuran perlu restart. |
| iowait tinggi | Kecilkan `cache_purge_batch_size`, besarkan `cache_purge_throttle_ms`, batasi purge masuk. |
| Antrean lambat habis | Ukur durasi dan perebutan sebelum memperbesar batch/memperpendek jeda. |
| Klien mengharapkan kode lama `412`/`404` | Hapus `cache_purge_legacy_status`; tangani `200` dengan `Cache-Purge-Code: no_match`. |
| Varian Vary tertinggal | Aktifkan `cache_purge_vary_aware`; periksa hasil utama dan batas 511 byte. |
| Pekerjaan diterima tampak belum selesai | Periksa tugas aktif, pemulihan, error file. `202` bukan bukti selesai; dugaan SHM rusak memerlukan restart. |

---

<a id="temporary-directories"></a>

## Direktori sementara

Penelusuran melewati direktori temporer terkonfigurasi di bawah akar dengan membandingkan komponen jalur lengkap. Nama seperti `proxy_temp` tidak dicadangkan bila bukan jalur temporer terkonfigurasi. Hanya file biasa bernama tepat 32 karakter hex menjadi kandidat. File temporer penulisan bernama numerik/berakhiran dilewati, termasuk `use_temp_path=off`.

Setiap penelusuran memvalidasi versi header native, offset, penanda `KEY: ` lengkap, CRC32 dan hash nama kunci/varian dengan batas tetap 8192 byte. File salah format, terpotong, versi asing atau tidak cocok dilewati bahkan purge-all. Panjang ETag/Vary tepat maksimum inti diterima. Validasi format tidak mengautentikasi file berbahaya.

Akar boleh berupa symlink atau junction Windows; symlink/reparse point turunan dilewati. POSIX dengan deteksi lengkap fitur relatif-deskriptor memakai `ngx_openat_file`/`ngx_file_at_info` inti dengan no-follow untuk anak, lalu menghapus relatif terhadap induk terbuka. Ini mencegah pengalihan oleh symlink anak yang diganti, bukan sandbox terhadap pemindahan direktori yang sudah terbuka. Windows/Unix lama memakai jalur penuh dan pemeriksaan link sehingga race penggantian tetap ada. Lookup/penghapusan tepat lama juga memakai jalur penuh. Jaga akar privat tanpa penulis tak tepercaya.

Gunakan penulisan jalur kanonis konsisten. Perbandingan tidak menyelesaikan alias symlink, mount, atau Unicode. Tugas menyimpan perlindungan; duplikat baru memperbarui tugas tertunda atau pengulangan berikutnya. Putaran aktif mempertahankan set lama, bukan gabungan seluruh generasi worker hidup. Jangan alihfungsikan jalur saat tugas terkait masih tertunda/aktif.

---

<a id="testing"></a>

## Pengujian

[Peta cakupan](../TEST_COVERAGE.md) (bahasa Inggris) membedakan kasus yang diuji dan kekurangan yang tersisa.

Build dan jalankan seluruh suite dalam Docker:

```sh
make test-all
NGINX_VERSION=1.31.6 make test-all
```

Dari `t/`, layanan Compose yang ada tetap tersedia:

```sh
docker-compose -f docker-compose.test.yml build nginx-test
docker-compose -f docker-compose.test.yml run --rm nginx-test

docker-compose -f docker-compose.test.yml run --rm test-basic
docker-compose -f docker-compose.test.yml run --rm test-queue
docker-compose -f docker-compose.test.yml run --rm test-config
docker-compose -f docker-compose.test.yml run --rm test-memory
docker-compose -f docker-compose.test.yml run --rm test-performance
docker-compose -f docker-compose.test.yml run --rm test-native
docker-compose -f docker-compose.test.yml run --rm test-runtime
```

Dengan plugin Compose, ganti `docker-compose` menjadi `docker compose`; Makefile mendeteksi keduanya. `nginx-test` dan `make test-all` menjalankan ketujuh `t/*.t` melalui `prove` yang sama dengan CI. Image integrasi mencakup pemeriksaan helper/layout native dan runtime pool. Build ketat tanpa thread menguji runtime inkremental. Setiap tes gagal menggagalkan perintah.

Lihat [`t/TESTING.md`](../../t/TESTING.md) untuk build ketat, sanitizer, Valgrind, platform dan laporan. Seluruh job GitHub didefinisikan di `build-test-release.yml`.
