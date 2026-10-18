[ngx_cache_purge](README.id.md)

<a id="directives"></a>

# Direktif

<a id="proxy_cache_purge"></a>

## `proxy_cache_purge`

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

## `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Setara `proxy_cache_purge` untuk zona `fastcgi_cache` / `fastcgi_cache_path`.

<a id="scgi_cache_purge"></a>

## `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Setara `proxy_cache_purge` untuk zona `scgi_cache` / `scgi_cache_path`.

<a id="uwsgi_cache_purge"></a>

## `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Setara `proxy_cache_purge` untuk zona `uwsgi_cache` / `uwsgi_cache_path`.

<a id="cache_purge_response_type"></a>

## `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

Memilih format isi respons PURGE yang berhasil; default-nya JSON. `Cache-Purge-Code` sesuai dengan kode hasil dalam isi respons. Respons modul menyertakan `Cache-Control: no-store`; kesalahan memakai `application/problem+json` sesuai RFC 9457. Kunci teks di-escape sesuai format, sedangkan kunci biner memakai Base64 dengan penanda pengodean.

`none` meniadakan isi PURGE: keberhasilan yang selesai menghasilkan `204`, pekerjaan latar belakang yang diterima tetap `202`, dan kesalahan tetap 4xx/5xx. Endpoint status dan REFRESH tetap memakai JSON terstruktur. Kode, kolom, dan batasannya dijelaskan dalam [kontrak HTTP](../HTTP_RESPONSES.md).

<a id="cache_purge_background_queue"></a>

## `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

Mengaktifkan pemrosesan tertunda wildcard dan `purge_all`. Mengembalikan `202 Accepted` setelah masuk antrean atau bergabung dengan tugas identik tertunda/aktif. Kunci tepat tidak berubah.

Mengontrol perilaku wildcard/seluruh cache lama. Ikatan `cache_purge_key` selalu memakai penghapusan fisik tertunda dan respons logis `200`, meskipun opsi ini mati.

Kapasitas atau alokasi gagal membuat permintaan tetap terbuka sampai diterima, tanpa penelusuran selama menunggu. Duplikat tugas yang ada dapat diterima saat penuh. Gagal mengalokasikan status tunggu kecil menghasilkan `500`. Pembersihan koneksi membatalkan timer dan melepas indikator tunggu.

Untuk opsi ini dan `cache_purge_vary_aware`, nilai deklarasi terakhir berlaku. Nilai tidak valid ditolak. `cache_purge_legacy_status` sudah usang dan diabaikan. Thread dan siklus hidup: [model eksekusi](../ARCHITECTURE.md#execution-model).

<a id="cache_purge_queue_size"></a>

## `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

Maksimum tugas belum selesai seluruh jalur. Tertunda dan ditugaskan sama-sama memakai kapasitas. Duplikat memakai satu alokasi; pengulangan mempertahankan alokasi dan jatah. Tugas berbeda menunggu slot kosong.

Zona bersama dibuat saat latar atau endpoint status dikonfigurasi. Perubahan kapasitas memerlukan restart meski ukuran pembulatan sama. Reload menolak perubahan demi mempertahankan tugas. Menambah akar cache dapat memerlukan memori dan restart; tuntaskan dahulu tugas tertunda/aktif yang wajib selesai.

Deklarasi namespace juga membuat antrean tugas.

Ukuran zona mencakup tugas, metadata jalur dan metrik worker yang selaras. Bergantung build/jumlah jalur; lihat [perhitungan memori](../OPERATIONS.md#memory-sizing).

<a id="cache_purge_thread_pool"></a>

## `cache_purge_thread_pool`

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

## `cache_purge_batch_size`

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

## `cache_purge_throttle_ms`

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

## `cache_purge_queue_status`

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

`namespaces` menampilkan entri `cache_purge_namespace` yang dikonfigurasi, terlepas dari cache yang dipilih location ini: semua jalur cache yang memakai namespace berbagi status generasinya. Setiap entri disalin dengan kunci namespace tersebut dipegang. Entri diambil secara terpisah; daftar ini bukan snapshot global.

| Kolom | Tipe / satuan | Makna |
|---|---|---|
| `name` | String | Nama namespace dalam prefiks kunci cache `p4:<name>:` |
| `epoch` | String / 10 digit | Epoch namespace saat ini; restart penuh membuat epoch baru |
| `baseline` | String / 16 digit heksadesimal huruf kecil | Generasi kunci yang tidak dinaikkan oleh aturan; invalidasi seluruh namespace menetapkannya ke `serial` |
| `serial` | String / 16 digit heksadesimal huruf kecil | Nomor urut mulai dari 1, bertambah pada setiap invalidasi yang dipublikasikan dalam epoch ini |
| `rules` | Boolean | `true` jika ada aturan exact atau prefiks; invalidasi seluruh namespace menghapusnya, termasuk ketika kekurangan memori bersama memperluas invalidasi |

`baseline` dan `serial` memiliki lebar tetap yang sama dengan generasi pada kunci cache dan dapat dibandingkan sebagai string. `namespaces_complete` bernilai `false` jika daftar dipotong agar muat dalam respons. Entri yang dikembalikan tetap dapat digunakan; namespace yang tidak ada dianggap tidak diketahui. Lihat [membaca berkas cache dalam mode generasi](OPERATIONS.id.md#reading-cache-files).

`pending.full` menghitung operasi seluruh cache; wildcard dengan kunci `*` adalah jenis berbeda. `active` mencakup penunggu pool, di luar `queue.size` dan `pending.total` tetapi tetap menggunakan kapasitas.

Untuk penyapuan namespace gabungan, `pending.full` mengikuti klasifikasi tugas pertama. Pemindaian/probe REFRESH tidak masuk kedalaman antrean atau error penghapusan. Memutar tugas percobaan tertunda dapat membuat `wait_time` menjadi usia kepala yang disampel, bukan usia tunggu maksimum.

Satu penulis event loop per slot proses memperbarui counter dengan awal/jarak selaras cache line. Thread mengembalikan hasil lewat event penyelesaian. Pembaca status tidak memakai mutex maupun atomic fetch-add, termasuk kenaikan nol. Build 64-bit memakai baca/tulis volatile selaras biasa; 32-bit memakai pemeriksaan urutan terbatas untuk melindungi baca 64-bit, sehingga shard sibuk dapat dilewati dalam satu sampel. Kolom bersifat perkiraan, bukan transaksi lintas worker. Penerimaan memakai status kontrol terkunci, bukan metrik.

Counter kumulatif jenuh pada maksimum unsigned 64-bit, bertahan saat reload dan reset saat restart. Indikator tunggu mengikuti pembersihan permintaan. Operasi sinkron dan penghapusan cache manager tidak termasuk total latar. Byte memperkirakan panjang file, bukan blok atau bukti ruang kembali; pembaca terbuka dan penulisan bersamaan memengaruhi hasil. Hasil parsial dihitung; hasil worker crash dapat hilang. Bukan log audit.

HIT/MISS hanya menghitung permintaan utama dengan keputusan cache native. Upstream hilang, status nol, BYPASS, EXPIRED, STALE, UPDATING dan REVALIDATED tidak masuk rasio. Permintaan biasa tanpa cache tidak menjadi MISS. Pemantauan saja dapat membuat SHM meski purge latar mati. Pada 32-bit, usia antrean mencakup kurang dari sekitar 24,9 hari; selisih waktu bertanda negatif dilaporkan nol.

<a id="cache_purge_legacy_status"></a>

## `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

Direktif usang yang diterima hanya untuk migrasi konfigurasi. Kedua nilai menghasilkan peringatan dan diabaikan; hapus direktif ini. Nilai tidak valid tetap ditolak. Operasi sinkron tanpa kecocokan menghasilkan `200 no_match`, pembersihan fisik yang diterima `202 accepted`, dan invalidasi generasi yang dipublikasikan `200 invalidated`. Pemilihan `412`/`404` telah dihapus. Lihat [kontrak HTTP](../HTTP_RESPONSES.md).

<a id="cache_purge_vary_aware"></a>

## `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

Dengan `on`, purge tepat yang berhasil menghapus file utama lalu menelusuri cache untuk menghapus sisa file dengan string `KEY:` identik. Mencakup `Vary` dan `gzip_vary` pada jalur berbeda tetapi satu kunci logis. Byte setelah kunci wajib `\n`, mencegah kecocokan palsu karena prefiks sama.

Nonaktif secara default karena setiap purge tepat menambahkan penelusuran seluruh direktori. Aktifkan untuk beberapa representasi `Vary`/`gzip_vary`. Penelusuran tetap dilakukan jika entri utama sudah tidak ada. Wildcard dan `purge_all` sudah mencakup varian. Kunci Vary tepat peka huruf besar/kecil dan dibatasi 511 byte; kunci yang lebih panjang menghasilkan `400 invalid_selector` sebelum penghapusan apa pun.

Invalidasi generasi mencakup seluruh representasi kunci tanpa opsi ini.

<a id="cache_purge_namespace"></a>

## `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

Mendeklarasikan namespace sebelum location diikat. Nama berisi 1–32 huruf ASCII, digit, `_` atau `-`; SHM minimal delapan halaman native. Gunakan direktori status persisten khusus di luar akar cache dan temporer. Inti membuat penanda persisten secara eksklusif untuk mengalokasikan epoch restart. Simpan file kecil ini selama cache terkait mungkin masih ada; menghapus status sambil mempertahankan cache dapat memakai ulang epoch usang. Tidak menjamin durabilitas mati listrik/fsync. Reload biasa menjaga epoch/aturan; perubahan ukuran/jalur atau penghapusan namespace memerlukan restart penuh. Ukuran SHM namespace dan antrean terpisah.

<a id="cache_purge_key"></a>

## `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

Mengikat location ke namespace yang ada dan menyediakan `$cache_purge_key`. Gunakan variabel ini sebagai seluruh `proxy_cache_key`, `fastcgi_cache_key`, `scgi_cache_key`, atau `uwsgi_cache_key`, termasuk location PURGE terpisah terkait. Bila kunci native hasil ekspansi berbeda dari kunci generasi, PURGE gagal dengan `500` alih-alih mengklaim invalidasi ketat. Jangan merujuk `$cache_purge_key` dalam ekspresinya sendiri. Batas kunci logis 8096 byte. Ikat hanya location peserta bila lainnya perlu perilaku lama; tidak ada `off` untuk membatalkan ikatan warisan.

<a id="proxy_cache_refresh"></a>

## `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

Membuat endpoint REFRESH eksplisit khusus permintaan utama; metode lain mendapat `405`. Memerlukan ikatan `cache_purge_key`, ekspresi native `$cache_purge_key` dan profil refresh. Kunci logis memilih kunci tepat, prefiks berakhiran `*`, atau `*` untuk namespace pada jalur ini. Lindungi dengan akses/autentikasi inti. REFRESH hanya untuk proxy; PURGE tetap untuk keempat protokol.

<a id="cache_purge_refresh_profile"></a>

## `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

Memetakan representasi yang diketahui ke location proxy internal literal. Menghapus prefiks tepat yang peka huruf harus menyisakan URI origin berawalan `/`; query string dan escape dipertahankan. Modul tidak menebak host atau mencari `://` dalam kunci. Target wajib `internal`, cache mati, penerusan header/body mati, metode HEAD, Host literal tidak kosong dan header representasi literal. Header bersyarat harus persis `$cache_purge_refresh_etag` dan `$cache_purge_refresh_last_modified`; `$cache_purge_refresh_uri` memasok URI untuk `proxy_pass`. Ketiga variabel murni ini hanya bermakna dalam probe. Target diperiksa di preaccess, termasuk pengalihan internal. Administrator wajib memastikan representasi origin sama dengan kunci; Vary dilewati karena metadata native tidak menyimpan header permintaan asli.

Tetapkan `proxy_intercept_errors off` di location probe. Intersepsi warisan ditolak agar error origin tidak ditulis ulang menjadi sukses yang menginvalidasi.

<a id="cache_purge_refresh_timeout"></a>

## `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

Membatasi pengiriman scan/probe baru dalam halaman REFRESH, termasuk menunggu pemindai worker. Kedaluwarsa menghentikan pekerjaan baru dan menandai respons tidak lengkap. HEAD yang aktif selesai menurut `proxy_connect_timeout`, `proxy_send_timeout`, dan `proxy_read_timeout`; atur eksplisit. Bukan tenggat respons keras, dan tidak memutus syscall. Putus koneksi/penghentian anggun menghentikan pengiriman sambil mempertahankan memori thread aktif sampai penyelesaian native.
