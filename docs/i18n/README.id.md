<a id="ngx_cache_purge"></a>

# ngx_cache_purge

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

Modul NGINX untuk menghapus cache `FastCGI`, `proxy`, `SCGI`, dan `uWSGI`. Versi 4.0 mempertahankan antarmuka lama serta menambahkan invalidasi generasi yang diaktifkan secara eksplisit sebelum penghapusan fisik tertunda, dan REFRESH bersyarat dengan pekerjaan terbatas untuk cache proxy.

---

<a id="contents"></a>

## Daftar isi

- [Fitur](#features)
- [Kompatibilitas](#compatibility)
- [Instalasi](#installation)
- [Model eksekusi](#execution-model)
- [Direktif](#directives)
- [Penghapusan berdasarkan sebagian kunci](#partial-key-purge)
- [Invalidasi generasi](#generation-invalidation)
- [REFRESH bersyarat](#conditional-refresh)
- [Contoh konfigurasi](#sample-configurations)
- [Penyetelan kinerja](#performance-tuning)
- [Pemantauan dan debugging](#monitoring-and-debugging)
- [Pemecahan masalah](#troubleshooting)
- [Pengujian](#testing)
- [Migrasi](#migration)
- [Direktori sementara](#temporary-directories)
- [Keamanan](#security)
- [Lisensi](#license)

---

<a id="features"></a>

## Fitur

- **Purge inline** — metode HTTP khusus (`PURGE`) dengan kontrol akses IP
- **Location purge terpisah** — sintaks zona/kunci `proxy_cache_purge zone key` untuk kunci tangkapan regex tanpa `proxy_pass`
- **Wildcard / purge sebagian** — `*` di akhir menelusuri direktori dan menghapus entri yang cocok
- **`purge_all`** — menghapus seluruh zona dalam satu permintaan
- **Antrean latar belakang** — penelusuran tertunda dalam langkah event loop terbatas atau pool thread NGINX opsional; batch dan jeda dapat diatur
- **Purge sadar Vary** — setelah kunci tepat dihapus, semua varian file gzip/Vary dengan kunci sama ikut dihapus
- **Format respons** — `json` (bawaan), `html`, `xml`, `text`, `none`
- **Pool bernama** — pilihan diwariskan, dengan eksekusi inkremental tanpa thread dan di Windows
- **Deduplikasi tugas aktif** — satu operasi terindeks dan paling banyak satu pengulangan tertunda untuk purge yang masuk saat penelusuran
- **Status** — metrik antrean, penghapusan, dan HIT/MISS per cache tanpa mengambil mutex antrean saat membaca
- **Kunci generasi** — invalidasi tepat, prefiks, atau namespace tanpa I/O disk pada jalur permintaan; penulisan lama yang terlambat tetap tak terjangkau
- **REFRESH bersyarat** — profil HEAD eksplisit, anggaran probe bersama, halaman lanjutan 32 entri

---

<a id="compatibility"></a>

## Kompatibilitas

Matriks ini menunjukkan target CI yang dikonfigurasi. Job yang berhasil memvalidasi konfigurasi sumber dan build tersebut, bukan setiap rilis dalam serinya.

| NGINX | CI: push / PR | CI: + full |
|---|---|---|
| 1.30.5, 1.31.6 | `test-matrix` | — |
| 1.31.6 | `build-compatibility` (GCC/Clang), `sanitizer`, `native-macos`, `native-windows` (x64/x86) | `memory-leak-test` (Valgrind) |
| 1.7.9 | `build-compatibility` (GCC) | — |
| 1.7.11, 1.7.12 | `build-compatibility` (GCC) | — |
| 1.20.2, 1.26.3, 1.28.3 | — | `test-matrix` |
| 1.11.4, 1.11.5 | — | `build-compatibility` (GCC) |

Job Linux memakai image Docker dan titik masuk yang sama dengan pengujian lokal. Job native macOS/Windows menguji API file dan proses masing-masing; Windows dibangun dengan MSVC dan memeriksa HTTP, jalur, serta reload `nginx.exe`. Skenario proses/sinyal POSIX berjalan di Linux dan macOS, bukan Windows. Windows juga tidak menjalankan jalur pool thread NGINX opsional.

Tanpa `--with-threads`, penelusuran berjalan secara bertahap. Build epoll sebelum NGINX 1.7.12 juga memakai mode ini untuk menghindari kondisi balapan pada notifikasi inti. BSD tidak memiliki CI native. Eksekusi biasa memeriksa stable/mainline saat ini dan batas API; eksekusi lengkap menambahkan versi lama dan Valgrind. Perubahan dokumentasi saja melewati build. Jadwal dan perintah: [pengujian](../../t/TESTING.md).

Kode dipisahkan menjadi konfigurasi, handler, antrean, penelusuran, invalidasi, REFRESH, dan respons. Fungsi path bersama berada dalam `ngx_cache_purge.h`, operasi platform dalam header POSIX/Win32. `generation.*` diganti nama menjadi `invalidation.*`; generasi tetap menunjukkan versi kunci. Lihat [arsitektur](../ARCHITECTURE.md).

Modul mengikuti gaya NGINX yang kompatibel C89 dan API event, slab, rbtree, serta atomik inti. Tidak memerlukan atomik C11 atau API filesystem baru. Build GNU89 ketat memeriksa urutan deklarasi dan peringatan; compiler/platform yang benar-benar diuji dicatat bersama hasilnya.

---

<a id="installation"></a>

## Instalasi

```bash
cd /path/to/nginx-source
./configure --add-module=/path/to/ngx_cache_purge
make
make install
```

Modul dinamis:

```bash
./configure --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

---

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

---

<a id="directives"></a>

## Direktif

<a id="proxy_cache_purge"></a>

### `proxy_cache_purge`

```
Syntax:  proxy_cache_purge on | off;
         proxy_cache_purge <method> [purge_all] from all | <cidr> ...;
         proxy_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

**Bentuk inline** (`from …`) menangkap metode HTTP pada location proxy dan menghapus entri. `on` adalah singkatan `PURGE`, `off` menonaktifkan. Batasi dengan CIDR atau izinkan semua lewat `from all`. `purge_all` sebelum `from` mengosongkan zona tanpa bergantung URI.

**Location terpisah** (zona dan kunci) hanya boleh pada location purge, biasanya regex penangkap kunci. Mencari zona berdasarkan nama dan menghapus menurut ekspresi terkompilasi. Tidak kompatibel dengan `proxy_cache` dan `proxy_pass` di location yang sama.

<a id="fastcgi_cache_purge"></a>

### `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Setara `proxy_cache_purge` untuk zona `fastcgi_cache` / `fastcgi_cache_path`.

<a id="scgi_cache_purge"></a>

### `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Setara `proxy_cache_purge` untuk zona `scgi_cache` / `scgi_cache_path`.

<a id="uwsgi_cache_purge"></a>

### `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Setara `proxy_cache_purge` untuk zona `uwsgi_cache` / `uwsgi_cache_path`.

<a id="cache_purge_response_type"></a>

### `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

Memilih format isi respons PURGE yang berhasil; default-nya JSON. `Cache-Purge-Code` sesuai dengan kode hasil dalam isi respons. Respons modul menyertakan `Cache-Control: no-store`; kesalahan memakai `application/problem+json` sesuai RFC 9457. Kunci teks di-escape sesuai format, sedangkan kunci biner memakai Base64 dengan penanda pengodean.

`none` meniadakan isi PURGE: keberhasilan yang selesai menghasilkan `204`, pekerjaan latar belakang yang diterima tetap `202`, dan kesalahan tetap 4xx/5xx. Endpoint status dan REFRESH tetap memakai JSON terstruktur. Kode, kolom, dan batasannya dijelaskan dalam [kontrak HTTP](../HTTP_RESPONSES.md).

<a id="cache_purge_background_queue"></a>

### `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

Mengaktifkan pemrosesan tertunda wildcard dan `purge_all`. Mengembalikan `202 Accepted` setelah masuk antrean atau bergabung dengan tugas identik tertunda/aktif. Kunci tepat tidak berubah.

Mengontrol perilaku wildcard/seluruh cache lama. Ikatan `cache_purge_key` selalu memakai penghapusan fisik tertunda dan respons logis `200`, meskipun opsi ini mati.

Kapasitas atau alokasi gagal membuat permintaan tetap terbuka sampai diterima, tanpa penelusuran selama menunggu. Duplikat tugas yang ada dapat diterima saat penuh. Gagal mengalokasikan status tunggu kecil menghasilkan `500`. Pembersihan koneksi membatalkan timer dan melepas indikator tunggu.

Untuk opsi ini dan `cache_purge_vary_aware`, nilai deklarasi terakhir berlaku. Nilai tidak valid ditolak. `cache_purge_legacy_status` sudah usang dan diabaikan. Thread dan siklus hidup: [model eksekusi](#execution-model).

<a id="cache_purge_queue_size"></a>

### `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

Maksimum tugas belum selesai seluruh jalur. Tertunda dan ditugaskan sama-sama memakai kapasitas. Duplikat memakai satu alokasi; pengulangan mempertahankan alokasi dan jatah. Tugas berbeda menunggu slot kosong.

Zona bersama dibuat saat latar atau endpoint status dikonfigurasi. Perubahan kapasitas memerlukan restart meski ukuran pembulatan sama. Reload menolak perubahan demi mempertahankan tugas. Menambah akar cache dapat memerlukan memori dan restart; tuntaskan dahulu tugas tertunda/aktif yang wajib selesai.

Deklarasi namespace juga membuat antrean tugas.

Ukuran zona mencakup tugas, metadata jalur dan metrik worker yang selaras. Bergantung build/jumlah jalur; lihat [perhitungan memori](#memory-sizing).

<a id="cache_purge_thread_pool"></a>

### `cache_purge_thread_pool`

```
Syntax:  cache_purge_thread_pool <name>;
Default: default
Context: http, server, location
```

Memilih eksekutor tugas latar yang diterima dengan pewarisan biasa. Contoh:

```nginx
thread_pool purge_io threads=2 max_queue=128;
http {
    cache_purge_background_queue on;
    cache_purge_thread_pool purge_io;
}
```

NGINX memvalidasi pool dan mengirim penyelesaian melalui event tugas native, tanpa menyalin struktur privat atau antrean penyelesaian. Dengan thread, pool khusus wajib ada. Tanpa thread/di Windows, direktif diterima dengan langkah event loop terbatas. Pool penuh menunda, tidak memindahkan I/O kembali ke event loop. Pool khusus menambah stack thread dan pergantian konteks.

<a id="cache_purge_batch_size"></a>

### `cache_purge_batch_size`

```
Syntax:  cache_purge_batch_size <number>;
Default: 10
Minimum: 1
Maximum: 64
Context: http
```

Jumlah tugas maksimal per worker antara jeda. Kemajuan dipertahankan antara langkah dan penyelesaian thread. Antrean kosong mengakhiri batch. Nilai di atas 64 dibatasi dengan peringatan; nol ditolak.

Mengatur tugas, bukan file atau durasi callback. Nilai kecil menambah jeda antartugas tanpa memutus penelusuran thread aktif. Setel bersama `cache_purge_throttle_ms` untuk beban disk.

<a id="cache_purge_throttle_ms"></a>

### `cache_purge_throttle_ms`

```
Syntax:  cache_purge_throttle_ms <time>;
Default: 10ms
Minimum: 1ms
Context: http
```

Jeda antarbatch. Menerima `10ms`, `500ms`, `1s`, dan gabungan bertanda kutip seperti `"2s 500ms"`. Bilangan tanpa unit berarti milidetik: `10` sama dengan `10ms`. Tanpa unit menghasilkan peringatan; unit eksplisit menghindarinya.

Worker menganggur memeriksa setelah sepuluh kali interval. Langkah belum selesai dan mutex sibuk memakai timer 1 ms. Ini interval penjadwalan minimum, bukan tenggat. Nol dan nilai di luar rentang aman platform ditolak.

Penerimaan, tekanan pool dan percobaan pembersihan tertunda memakai polling 10 ms.

<a id="cache_purge_queue_status"></a>

### `cache_purge_queue_status`

```
Syntax:  cache_purge_queue_status <cache_path>;
Default: not configured
Context: location
```

Menyediakan snapshot JSON GET/HEAD dengan `Cache-Control: no-store`. Jalur harus menunjuk akar cache terkonfigurasi; jalur tak dikenal menggagalkan pemuatan konfigurasi walau latar mati. Batasi akses:

```nginx
location = /cache_purge_queue_status {
    allow 127.0.0.1;
    deny  all;
    cache_purge_queue_status /var/cache/nginx;
}
```

```json
{
  "code": "status",
  "queue": { "size": 0, "running": 0, "max_size": 1024, "wait_time": 0, "full": false },
  "cache": {
    "pending": { "total": 0, "full": 0 },
    "active": 0,
    "requests": { "overflow": 0, "deduplicated": 0, "waiting": 0 },
    "files": { "deleted": 0, "errors": 0, "bytes": 0 },
    "logical": { "invalidations": 0 },
    "cleanup": { "scan_errors": 0, "retries": 0, "exhausted": 0 },
    "responses": { "hit": 0, "miss": 0, "hit_ratio": 0.000000 }
  }
}
```

`queue` menjelaskan antrean bersama semua jalur:

| Kolom | Tipe / unit | Arti |
|---|---|---|
| `size` | Integer / tugas | Menunggu worker |
| `running` | Integer / tugas | Ditugaskan, termasuk menunggu pool |
| `max_size` | Integer / tugas | Kapasitas gabungan tertunda dan ditugaskan |
| `wait_time` | Integer / milidetik | Usia tunggu tertua; nol jika kosong |
| `full` | Boolean | Sampel jumlah tertunda dan ditugaskan mencapai kapasitas |

`cache` menjelaskan akar yang dipilih location:

| Kolom | Tipe / unit | Arti |
|---|---|---|
| `pending.total` | Integer / tugas | Tugas tertunda cache ini |
| `pending.full` | Integer / tugas | `purge_all` tertunda, termasuk dalam `pending.total` |
| `active` | Integer / tugas | Ditugaskan hingga pencatatan penyelesaian selesai |
| `requests.overflow` | Integer / permintaan | Masuk penantian penerimaan, termasuk tekanan lock/alokasi |
| `requests.deduplicated` | Integer / permintaan | Digabung dengan pekerjaan tertunda/aktif |
| `requests.waiting` | Integer / permintaan | Penunggu penerimaan saat ini untuk cache ini |
| `files.deleted` | Integer / file | Penghapusan latar berhasil yang dilaporkan setelah putaran |
| `files.errors` | Integer / kesalahan | Penghapusan gagal selain file yang sudah hilang |
| `files.bytes` | Integer / byte | Perkiraan ukuran logis file terhapus |
| `responses.hit`, `responses.miss` | Integer / permintaan utama | Keputusan HIT/MISS inti yang diamati pada fase log |
| `responses.hit_ratio` | Angka / 0..1 | HIT / (HIT + MISS), atau nol tanpa pengamatan |
| `logical.invalidations` | Integer / keputusan | Invalidasi generasi terpublikasi, termasuk duplikat dan REFRESH |
| `cleanup.scan_errors` | Integer / kesalahan | Kesalahan metadata/buka/baca/tutup/alokasi latar |
| `cleanup.retries` | Integer / putaran | Putaran tambahan karena pembersihan gagal |
| `cleanup.exhausted` | Integer / tugas | Tugas dihentikan setelah tiga percobaan ulang gagal |

`pending.full` menghitung operasi seluruh cache; wildcard dengan kunci `*` adalah jenis berbeda. `active` mencakup penunggu pool, di luar `queue.size` dan `pending.total` tetapi tetap menggunakan kapasitas.

Untuk penyapuan namespace gabungan, `pending.full` mengikuti klasifikasi tugas pertama. Pemindaian/probe REFRESH tidak masuk kedalaman antrean atau error penghapusan. Memutar tugas percobaan tertunda dapat membuat `wait_time` menjadi usia kepala yang disampel, bukan usia tunggu maksimum.

Satu penulis event loop per slot proses memperbarui counter dengan awal/jarak selaras cache line. Thread mengembalikan hasil lewat event penyelesaian. Pembaca status tidak memakai mutex maupun atomic fetch-add, termasuk kenaikan nol. Build 64-bit memakai baca/tulis volatile selaras biasa; 32-bit memakai pemeriksaan urutan terbatas untuk melindungi baca 64-bit, sehingga shard sibuk dapat dilewati dalam satu sampel. Kolom bersifat perkiraan, bukan transaksi lintas worker. Penerimaan memakai status kontrol terkunci, bukan metrik.

Counter kumulatif jenuh pada maksimum unsigned 64-bit, bertahan saat reload dan reset saat restart. Indikator tunggu mengikuti pembersihan permintaan. Operasi sinkron dan penghapusan cache manager tidak termasuk total latar. Byte memperkirakan panjang file, bukan blok atau bukti ruang kembali; pembaca terbuka dan penulisan bersamaan memengaruhi hasil. Hasil parsial dihitung; hasil worker crash dapat hilang. Bukan log audit.

HIT/MISS hanya menghitung permintaan utama dengan keputusan cache native. Upstream hilang, status nol, BYPASS, EXPIRED, STALE, UPDATING dan REVALIDATED tidak masuk rasio. Permintaan biasa tanpa cache tidak menjadi MISS. Pemantauan saja dapat membuat SHM meski purge latar mati. Pada 32-bit, usia antrean mencakup kurang dari sekitar 24,9 hari; selisih waktu bertanda negatif dilaporkan nol.

<a id="cache_purge_legacy_status"></a>

### `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

Direktif usang yang diterima hanya untuk migrasi konfigurasi. Kedua nilai menghasilkan peringatan dan diabaikan; hapus direktif ini. Nilai tidak valid tetap ditolak. Operasi sinkron tanpa kecocokan menghasilkan `200 no_match`, pembersihan fisik yang diterima `202 accepted`, dan invalidasi generasi yang dipublikasikan `200 invalidated`. Pemilihan `412`/`404` telah dihapus. Lihat [kontrak HTTP](../HTTP_RESPONSES.md).

<a id="cache_purge_vary_aware"></a>

### `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

Dengan `on`, purge tepat yang berhasil menghapus file utama lalu menelusuri cache untuk menghapus sisa file dengan string `KEY:` identik. Mencakup `Vary` dan `gzip_vary` pada jalur berbeda tetapi satu kunci logis. Byte setelah kunci wajib `\n`, mencegah kecocokan palsu karena prefiks sama.

Nonaktif secara default karena setiap purge tepat menambahkan penelusuran seluruh direktori. Aktifkan untuk beberapa representasi `Vary`/`gzip_vary`. Penelusuran tetap dilakukan jika entri utama sudah tidak ada. Wildcard dan `purge_all` sudah mencakup varian. Kunci Vary tepat peka huruf besar/kecil dan dibatasi 511 byte; kunci yang lebih panjang menghasilkan `400 invalid_selector` sebelum penghapusan apa pun.

Invalidasi generasi mencakup seluruh representasi kunci tanpa opsi ini.

<a id="cache_purge_namespace"></a>

### `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

Mendeklarasikan namespace sebelum location diikat. Nama berisi 1–32 huruf ASCII, digit, `_` atau `-`; SHM minimal delapan halaman native. Gunakan direktori status persisten khusus di luar akar cache dan temporer. Inti membuat penanda persisten secara eksklusif untuk mengalokasikan epoch restart. Simpan file kecil ini selama cache terkait mungkin masih ada; menghapus status sambil mempertahankan cache dapat memakai ulang epoch usang. Tidak menjamin durabilitas mati listrik/fsync. Reload biasa menjaga epoch/aturan; perubahan ukuran/jalur atau penghapusan namespace memerlukan restart penuh. Ukuran SHM namespace dan antrean terpisah.

<a id="cache_purge_key"></a>

### `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

Mengikat location ke namespace yang ada dan menyediakan `$cache_purge_key`. Gunakan variabel ini sebagai seluruh `proxy_cache_key`, `fastcgi_cache_key`, `scgi_cache_key`, atau `uwsgi_cache_key`, termasuk location PURGE terpisah terkait. Bila kunci native hasil ekspansi berbeda dari kunci generasi, PURGE gagal dengan `500` alih-alih mengklaim invalidasi ketat. Jangan merujuk `$cache_purge_key` dalam ekspresinya sendiri. Batas kunci logis 8096 byte. Ikat hanya location peserta bila lainnya perlu perilaku lama; tidak ada `off` untuk membatalkan ikatan warisan.

<a id="proxy_cache_refresh"></a>

### `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

Membuat endpoint REFRESH eksplisit khusus permintaan utama; metode lain mendapat `405`. Memerlukan ikatan `cache_purge_key`, ekspresi native `$cache_purge_key` dan profil refresh. Kunci logis memilih kunci tepat, prefiks berakhiran `*`, atau `*` untuk namespace pada jalur ini. Lindungi dengan akses/autentikasi inti. REFRESH hanya untuk proxy; PURGE tetap untuk keempat protokol.

<a id="cache_purge_refresh_profile"></a>

### `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

Memetakan representasi yang diketahui ke location proxy internal literal. Menghapus prefiks tepat yang peka huruf harus menyisakan URI origin berawalan `/`; query string dan escape dipertahankan. Modul tidak menebak host atau mencari `://` dalam kunci. Target wajib `internal`, cache mati, penerusan header/body mati, metode HEAD, Host literal tidak kosong dan header representasi literal. Header bersyarat harus persis `$cache_purge_refresh_etag` dan `$cache_purge_refresh_last_modified`; `$cache_purge_refresh_uri` memasok URI untuk `proxy_pass`. Ketiga variabel murni ini hanya bermakna dalam probe. Target diperiksa di preaccess, termasuk pengalihan internal. Administrator wajib memastikan representasi origin sama dengan kunci; Vary dilewati karena metadata native tidak menyimpan header permintaan asli.

Tetapkan `proxy_intercept_errors off` di location probe. Intersepsi warisan ditolak agar error origin tidak ditulis ulang menjadi sukses yang menginvalidasi.

<a id="cache_purge_refresh_timeout"></a>

### `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

Membatasi pengiriman scan/probe baru dalam halaman REFRESH, termasuk menunggu pemindai worker. Kedaluwarsa menghentikan pekerjaan baru dan menandai respons tidak lengkap. HEAD yang aktif selesai menurut `proxy_connect_timeout`, `proxy_send_timeout`, dan `proxy_read_timeout`; atur eksplisit. Bukan tenggat respons keras, dan tidak memutus syscall. Putus koneksi/penghentian anggun menghentikan pengiriman sambil mempertahankan memori thread aktif sampai penyelesaian native.

---

<a id="partial-key-purge"></a>

## Penghapusan berdasarkan sebagian kunci

Tambahkan `*` pada **kunci cache yang sudah dievaluasi** untuk mencocokkan prefiks tanpa membedakan huruf besar/kecil. Kunci tepat dan Vary tetap peka huruf besar/kecil. Prefiks pembersihan fisik maksimal 511 byte, tidak termasuk `*` terakhir. Prefiks yang lebih panjang dicatat sekali dan menghasilkan `400 invalid_selector` tanpa menelusuri berkas.

Dengan `cache_purge_key`, pencocokan memakai ekspresi logis dan menerima hingga 8096 byte, termasuk `*` akhir.

Untuk ekspresi yang berakhir `$uri`, permintaan ini memilih prefiks URI:

```http
PURGE /images/header*
```

Bintang harus menjadi byte terakhir kunci yang dievaluasi, bukan sekadar jalur URI. Perhitungkan query string atau akhiran variabel di `proxy_cache_key`. Kunci `*` saja menelusuri semua entri yang memenuhi syarat.

---

<a id="generation-invalidation"></a>

## Invalidasi generasi

Pada namespace terikat, PURGE memublikasikan generasi baru dan menjadwalkan penyapuan generasi usang yang terdeduplikasi sebelum mengembalikan `200` berstatus `invalidated`. Jalur penerimaan tidak membuka/menghapus file cache. Permintaan baru yang pertama kali mengevaluasi kunci setelah publikasi tidak dapat HIT generasi sebelumnya. Permintaan mengunci generasi per namespace/kunci logis, termasuk pengalihan internal; respons yang sudah mulai boleh selesai tetapi tulisan terlambatnya tidak menjadi HIT bagi permintaan baru. Kecocokan tepat peka huruf, prefiks tidak peka huruf ASCII. `purge_all` mengubah seluruh namespace termasuk jalur lain yang sengaja berbagi. Pembersihan fisik dijadwalkan untuk jalur tujuan; jalur lain memakai expiry inti atau purge berikutnya.

Invalidasi diterima tidak menjamin ruang disk langsung kembali. Tulisan generasi lama setelah penyapuan tetap tak terjangkau, lalu dihapus oleh pembersihan berikutnya atau expiry inactive inti. Saat SHM tertekan, aturan dipadatkan ke baseline namespace baru: kunci tambahan menjadi dingin, bukan kunci terinvalidasi muncul lagi. Setiap invalidasi berikutnya mereklamasi paling banyak 64 node trie terlepas. PURGE berulang pada kunci populer tidak memperbesar pohon tanpa batas. Kompleksitas bergantung panjang kunci terbatas dan indeks tugas, bukan jumlah file; tidak menjanjikan waktu nyata O(1) atau bebas starvation saat overload. Penerimaan dapat menunggu. Gunakan `proxy_cache_lock` dan kontrol setara untuk menahan lonjakan GET setelah invalidasi.

<a id="conditional-refresh"></a>

## REFRESH bersyarat

REFRESH memvalidasi objek yang masih ada sebelum TTL habis; tidak memanaskan cache atau memperpanjang TTL pada `304`. Melengkapi `proxy_cache_revalidate` dan `proxy_cache_background_update` untuk lalu lintas biasa objek kedaluwarsa. Origin yang mengabaikan validator dan selalu mengirim `200` menjadikan REFRESH invalidasi plus HEAD tambahan. Kandidat hanya entri terkini, valid, dengan jalur hash kanonis. Penggantian fisik pada generasi sama dapat diinvalidasi secara konservatif; perbandingan generasi mencegah HEAD lama merusak generasi PURGE lebih baru.

| Hasil origin mentah | Tindakan / hasil |
|---|---|
| `304` | Pertahankan; `kept` |
| `200`, `404`, `410` | Invalidasi generasi bersyarat dan pembersihan tertunda; `invalidated` |
| Status lain | Pertahankan; `upstream_status` |
| Tanpa status / gagal transport | Pertahankan; `transport_error` |
| Metadata Vary | Tanpa HEAD; `skipped_vary` |
| Pemetaan/profil tidak valid | Pertahankan; `skipped_profile` |
| Generasi sudah diganti | Tanpa invalidasi baru; `raced_replaced` |
| Kegagalan internal invalidasi/alokasi | Pertahankan; `invalidation_error` |
| Pekerjaan belum dikirim/selesai saat halaman berhenti | `undispatched`; kursor tidak melewatinya |

Worker memiliki paling banyak satu halaman aktif dan satu probe; anggaran bersama membatasi total 16 probe antarworker. Halaman lain menunggu tanpa array kandidat. Setiap halaman memindai dengan kursor bersama, menyimpan maksimal 32 kandidat, lalu mengirim HEAD native berurutan. Ini membatasi memori dan subrequest yang berbagi pool utama. Permintaan REFRESH identik tidak dideduplikasi, tetapi berbagi batas konkurensi. Invalidasinya memakai antrean PURGE terdeduplikasi bersama.

Mulai tanpa kursor, kemudian kirim `X-Cache-Purge-Cursor: <next_cursor>` ke endpoint sama dengan selektor/profil sama sampai `complete` benar. Kursor adalah nama file hex 32 karakter terakhir yang selesai; header invalid/duplikat menghasilkan `400`. `scan_complete` berarti enumerasi sukses; `complete` juga mensyaratkan tidak ada halaman berikutnya dan semua keputusan selesai. `timed_out`/`scan_error` membedakan interupsi/kegagalan. `undispatched` menghitung kandidat belum selesai; scan tidak lengkap hanya memberi batas bawah, bukan total cache. Jika scan belum lengkap, tidak ada probe, hasil kosong dan kursor masuk tidak berubah. Hasil menampilkan hash dan status upstream mentah, bukan kunci/kredensial.

Setiap halaman memindai ulang untuk hash berikutnya: memori terbatas dibayar I/O berulang, `O(files × pages)` untuk seluruh operasi. Paginasi berkonsistensi lemah saat penambahan/penghapusan bersamaan; mulai ulang jika perlu snapshot lengkap sesudahnya. Pilih selektor/timeout sesuai kebutuhan. Antarmuka ini tidak cocok untuk memindai cache aktif sangat besar dengan frekuensi tinggi.

<a id="generation-and-refresh-example"></a>

### Contoh generasi dan REFRESH

```nginx
thread_pool purge_io threads=2 max_queue=32;
http {
    upstream origin { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx/objects levels=1:2 keys_zone=objects:10m;
    cache_purge_namespace objects 1m /var/lib/nginx/purge-epochs;
    cache_purge_thread_pool purge_io;
    cache_purge_response_type json;

    server {
        listen 8080;
        location /objects/ {
            cache_purge_key objects "origin.test$request_uri";
            proxy_cache objects;
            proxy_cache_key $cache_purge_key;
            proxy_cache_lock on;
            proxy_cache_valid 200 10m;
            proxy_set_header Host origin.test;
            proxy_pass http://origin;
            proxy_cache_purge PURGE from 127.0.0.1;
        }
        location = /admin/refresh {
            allow 127.0.0.1; deny all;
            cache_purge_key objects $http_x_cache_key;
            proxy_cache_refresh objects $cache_purge_key;
            cache_purge_refresh_profile /internal/cache-probe origin.test;
            cache_purge_refresh_timeout 10s;
        }
        location = /internal/cache-probe {
            internal;
            proxy_cache off;
            proxy_intercept_errors off;
            proxy_pass_request_headers off;
            proxy_pass_request_body off;
            proxy_connect_timeout 1s;
            proxy_send_timeout 2s;
            proxy_read_timeout 2s;
            proxy_method HEAD;
            proxy_set_header Host origin.test;
            proxy_set_header Authorization "";
            proxy_set_header Cookie "";
            proxy_set_header If-None-Match $cache_purge_refresh_etag;
            proxy_set_header If-Modified-Since $cache_purge_refresh_last_modified;
            proxy_pass http://origin$cache_purge_refresh_uri;
        }
    }
}
```

```sh
curl -X PURGE 'http://127.0.0.1:8080/objects/item'
curl -X REFRESH -H 'X-Cache-Key: origin.test/objects/*'      'http://127.0.0.1:8080/admin/refresh'
# Repeat with X-Cache-Purge-Cursor set to the returned next_cursor.
```

<a id="sample-configurations"></a>

## Contoh konfigurasi

<a id="basic--inline-purge-method"></a>

### Dasar — metode purge inline

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        location / {
            proxy_pass        http://backend;
            proxy_cache       main;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }
    }
}
```

```bash
curl -X PURGE http://localhost/images/logo.png
# 200 — entry purged
# 200 + Cache-Purge-Code: no_match
# 403 — client IP not in the allowed list
```

<a id="basic--separate-purge-location"></a>

### Dasar — location purge terpisah

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        location / {
            proxy_pass      http://backend;
            proxy_cache     main;
            proxy_cache_key "$host$uri$is_args$args";
        }

        # Capture the cache key from the URI and pass it to the zone/key form.
        # No proxy_pass or proxy_cache required in this location.
        location ~ ^/purge(/.*) {
            allow             127.0.0.1;
            deny              all;
            proxy_cache_purge main "$host$1$is_args$args";
        }
    }
}
```

<a id="background-queue-enabled"></a>

### Antrean latar diaktifkan

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;
    cache_purge_queue_size        2048;
    cache_purge_batch_size        20;
    cache_purge_throttle_ms       10ms;

    server {
        location / {
            proxy_pass        http://backend;
            proxy_cache       main;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }
    }
}
```

<a id="purge-all-entries"></a>

### Menghapus semua entri

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        location /content {
            proxy_pass        http://backend;
            proxy_cache       main;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE purge_all from 127.0.0.1;
        }
    }
}
```

```bash
curl -X PURGE http://localhost/content/anything
# 200 — walk completed; inspect error_log for file errors
```

<a id="json-responses-with-per-location-override"></a>

### Respons JSON dengan pengaturan per location

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;

    server {
        cache_purge_response_type json;   # default for all locations in this server
        location / {
            proxy_pass        http://backend;
            proxy_cache       main;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }

        location /admin/purge {
            allow 127.0.0.1;
            deny  all;
            # Override to plain text for scripting
            cache_purge_response_type text;
            proxy_cache_purge         main "$arg_key";
        }
    }
}
```

<a id="vary-aware-purge"></a>

### Purge sadar Vary

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

Setelah kunci dihapus, semua varian gzip/Vary pada jalur berbeda ikut dihapus otomatis.

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

[Endpoint status](#cache_purge_queue_status) menunjukkan tugas tertunda/aktif, penunggu, duplikat, penghapusan, HIT/MISS. `202 Accepted` mengonfirmasi penerimaan/penggabungan, bukan penghapusan atau invalidasi logis seketika. Pantau latensi bersama usia antrean dan perebutan penyimpanan.

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

<a id="testing"></a>

## Pengujian

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

Lihat [`t/TESTING.md`](../../t/TESTING.md) untuk build ketat, sanitizer, Valgrind, platform dan laporan. Seluruh job GitHub didefinisikan di `ci.yml`.

---

<a id="migration"></a>

## Migrasi

<a id="queue-and-status-changes"></a>

### Perubahan antrean dan status

Perubahan struktur SHM memerlukan restart penuh. Sintaks metode/ACL dan handler protokol tetap tersedia. v4 memakai JSON secara default, kode hasil yang sama pada header dan isi, serta `200 no_match` untuk entri yang tidak ada. `cache_purge_legacy_status` hanya menghasilkan peringatan. Perbarui klien sesuai [kontrak HTTP](../HTTP_RESPONSES.md). Kapasitas mencakup tugas yang sudah ditetapkan; kelebihan beban menunggu penerimaan. Duplikat aktif dapat menjadwalkan penelusuran ulang.

Status menambah kolom tanpa mengubah nama lama. `requests.overflow` kini menghitung penunggu penerimaan. Metrik bersifat perkiraan dengan counter kumulatif 64-bit. Build berthread mendaftarkan `default` otomatis bila dirujuk; pool bernama mengisolasi I/O. Konfigurasi status saja mengalokasikan SHM.

Generasi/REFRESH diaktifkan eksplisit pada v4.0. Ikat `cache_purge_key` dan gunakan `$cache_purge_key` sebagai seluruh kunci native di setiap location peserta. Nama fisik berubah; file lama menjadi dingin dan menunggu expiry inti atau pembersihan lama eksplisit. Simpan penanda status bersama cache. Location PURGE lama tidak otomatis mendapat REFRESH. Pengguna PR60 eksperimental harus mengatur endpoint/profil baru dan paginasi; respons tak terbatas, tebakan URI/Host dan perluasan metode implisit PR60 bukan janji kompatibilitas.

<a id="from-the-original-frickle-module"></a>

### Dari modul FRiCKLE asli

Bentuk metode/daftar akses dan zona/kunci tetap tersedia. Entri yang tidak ada menghasilkan `200 no_match`; hapus `cache_purge_legacy_status` dan perbarui klien. Untuk mengaktifkan pemrosesan latar belakang, tambahkan:

```nginx
http {
    cache_purge_background_queue on;
}
```

<a id="from-nginx-selective-cache-purge-module"></a>

### Dari nginx-selective-cache-purge-module

Ganti modul; antrean sudah bawaan. Petakan parameter ke `cache_purge_queue_size`, `cache_purge_batch_size` dan `cache_purge_throttle_ms`.

---

<a id="temporary-directories"></a>

## Direktori sementara

Penelusuran melewati direktori temporer terkonfigurasi di bawah akar dengan membandingkan komponen jalur lengkap. Nama seperti `proxy_temp` tidak dicadangkan bila bukan jalur temporer terkonfigurasi. Hanya file biasa bernama tepat 32 karakter hex menjadi kandidat. File temporer penulisan bernama numerik/berakhiran dilewati, termasuk `use_temp_path=off`.

Setiap penelusuran memvalidasi versi header native, offset, penanda `KEY: ` lengkap, CRC32 dan hash nama kunci/varian dengan batas tetap 8192 byte. File salah format, terpotong, versi asing atau tidak cocok dilewati bahkan purge-all. Panjang ETag/Vary tepat maksimum inti diterima. Validasi format tidak mengautentikasi file berbahaya.

Akar boleh berupa symlink atau junction Windows; symlink/reparse point turunan dilewati. POSIX dengan deteksi lengkap fitur relatif-deskriptor memakai `ngx_openat_file`/`ngx_file_at_info` inti dengan no-follow untuk anak, lalu menghapus relatif terhadap induk terbuka. Ini mencegah pengalihan oleh symlink anak yang diganti, bukan sandbox terhadap pemindahan direktori yang sudah terbuka. Windows/Unix lama memakai jalur penuh dan pemeriksaan link sehingga race penggantian tetap ada. Lookup/penghapusan tepat lama juga memakai jalur penuh. Jaga akar privat tanpa penulis tak tepercaya.

Gunakan penulisan jalur kanonis konsisten. Perbandingan tidak menyelesaikan alias symlink, mount, atau Unicode. Tugas menyimpan perlindungan; duplikat baru memperbarui tugas tertunda atau pengulangan berikutnya. Putaran aktif mempertahankan set lama, bukan gabungan seluruh generasi worker hidup. Jangan alihfungsikan jalur saat tugas terkait masih tertunda/aktif.

---

<a id="security"></a>

## Keamanan

- Batasi endpoint purge dengan `allow` / `deny` atau lapisan autentikasi sebelumnya. Jangan buka untuk publik.
- Atur `cache_purge_queue_size` sesuai lalu lintas untuk membatasi SHM saat banjir permintaan.
- Pertimbangkan `limit_req` agar laju purge dibatasi terpisah dari lalu lintas biasa.

---

<a id="license"></a>

## Lisensi

```
Copyright (c) 2009-2014, FRiCKLE <info@frickle.com>
Copyright (c) 2009-2014, Piotr Sikora <piotr.sikora@frickle.com>
Copyright (C) 2016-2026, Denis Denisov

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in
   the documentation and/or other materials provided with the
   distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

<a id="see-also"></a>

## Lihat juga

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [Dokumentasi nginx](https://nginx.org/en/docs/)
