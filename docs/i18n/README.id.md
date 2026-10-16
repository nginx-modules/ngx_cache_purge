<a id="ngx_cache_purge"></a>

# ngx_cache_purge

Modul NGINX untuk menghapus entri cache `FastCGI`, `proxy`, `SCGI`, dan `uWSGI`. Pada v4, generasi kunci mencegah permintaan baru memakai entri lama sebelum berkasnya dihapus. REFRESH memeriksa ke server asal apakah isi cache proxy telah berubah. Kedua fitur perlu dikonfigurasi secara eksplisit. Sintaks PURGE tetap tersedia, tetapi respons HTTP berubah; lihat [migrasi](README.id.md#migration).

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

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
- [Memeriksa cache ke server asal (REFRESH)](#conditional-refresh)

<a id="choosing-a-mode"></a>

## Pilih perilaku yang diperlukan

| Tujuan | Konfigurasi | Arti respons |
|---|---|---|
| Menghapus entri usang dan menunggu upaya penghapusan | PURGE tanpa antrean latar belakang | `200 purged` atau `200 no_match`; 4xx/5xx jika gagal |
| Menjadwalkan pembersihan berdasarkan prefiks atau seluruh cache | PURGE dengan `cache_purge_background_queue on` | `202 accepted`; entri lama masih dapat disajikan selama menunggu |
| Mencegah permintaan baru memakai konten lama, termasuk hasil penulisan terlambat | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`; generasi kunci baru, berkas dihapus kemudian |
| Memeriksa perubahan di server asal | Generasi + `proxy_cache_refresh` + profil internal | `200` dengan hasil per entri dan kelanjutan; tidak mengisi cache lebih dahulu |

Mekanisme ini dapat digabungkan: pergantian generasi juga menjadwalkan pembersihan. Antrean saja tidak melindungi dari penulisan terlambat. Lihat [contoh v4](README.id.md#generation-and-refresh-example), [contoh PURGE](#sample-configurations), dan [respons HTTP](../HTTP_RESPONSES.md) (bahasa Inggris).

<a id="compatibility"></a>

## Kompatibilitas

Untuk produksi, gunakan rilis NGINX yang masih dipelihara. POSIX/UNIX merupakan target utama; dukungan Windows masih eksperimental. Thread bersifat opsional. [NGINX / CI](../../t/TESTING.md#compatibility-matrix).

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
./configure --with-compat --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

Gunakan kode sumber yang sesuai dengan versi NGINX di server. `--with-compat` tidak menjamin kompatibilitas antarsemua versi NGINX atau pustaka C. Lihat [modul dinamis dan arsip rilis](../BINARY_MODULES.md) (bahasa Inggris).

<a id="generation-and-refresh-example"></a>

## Contoh: katalog produk

Contoh lengkap ini mendengarkan port lokal 8080 dan meneruskan permintaan ke aplikasi pada port 8081. Aplikasi harus melayani `/products/42` dengan Host `shop.example.com`. Sesuaikan alamat, domain, serta direktori cache dan status yang dapat ditulis.

`shop.example.com` adalah domain pada header HTTP Host, bukan direktori. `/products/42` adalah URI produk. `catalog:` adalah prefiks kunci tetap, bukan nama host. Prefiks ditambah URI lengkap beserta query string membentuk kunci logis, misalnya `catalog:/products/42`. Profil REFRESH menghapus `catalog:` dan memeriksa `/products/42` pada aplikasi yang sama.

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx/catalog levels=1:2
                     keys_zone=catalog:10m;
    cache_purge_namespace catalog 1m /var/lib/nginx/cache-purge;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        location /products/ {
            cache_purge_key catalog "catalog:$request_uri";
            proxy_cache catalog;
            proxy_cache_key $cache_purge_key;
            proxy_cache_lock on;
            proxy_cache_valid 200 10m;
            add_header X-Cache-Status $upstream_cache_status always;
            proxy_set_header Host shop.example.com;
            proxy_pass http://catalog_backend;
            proxy_cache_purge PURGE from 127.0.0.1;
        }

        location = /admin/refresh {
            allow 127.0.0.1;
            deny all;
            cache_purge_key catalog $http_x_cache_key;
            proxy_cache_refresh catalog $cache_purge_key;
            cache_purge_refresh_profile /internal/cache-probe catalog:;
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
            proxy_set_header Host shop.example.com;
            proxy_set_header Authorization "";
            proxy_set_header Cookie "";
            proxy_set_header If-None-Match $cache_purge_refresh_etag;
            proxy_set_header If-Modified-Since
                             $cache_purge_refresh_last_modified;
            proxy_pass http://catalog_backend$cache_purge_refresh_uri;
        }
    }
}
```

GET produk yang ada dan dapat disimpan di cache hingga muncul `X-Cache-Status: HIT`, lalu kirim REFRESH:

```sh
curl -i 'http://127.0.0.1:8080/products/42'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/42' \
  'http://127.0.0.1:8080/admin/refresh'
```

Jika produk berubah, kirim PURGE. Di sini `200 invalidated` mencegah permintaan baru memakai generasi lama; GET berikutnya menuju aplikasi.

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/products/42'
```

Untuk semua produk, gunakan `catalog:/products/*`. Jika `complete: false`, pertahankan selektor dan ganti `<next_cursor>` dengan nilai hasil. Atasi kesalahan HTTP sebelum melanjutkan.

```sh
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

<a id="partial-key-purge"></a>

## Penghapusan berdasarkan sebagian kunci

Tambahkan `*` pada **kunci cache yang sudah dievaluasi** untuk mencocokkan prefiks tanpa membedakan huruf besar/kecil. Kunci tepat dan Vary tetap peka huruf besar/kecil. Prefiks pembersihan fisik maksimal 511 byte, tidak termasuk `*` terakhir. Prefiks yang lebih panjang dicatat sekali dan menghasilkan `400 invalid_selector` tanpa menelusuri berkas.

Dengan `cache_purge_key`, pencocokan memakai ekspresi logis dan menerima hingga 8096 byte, termasuk `*` akhir.

Untuk ekspresi yang berakhir `$uri`, permintaan ini memilih prefiks URI:

```http
PURGE /images/header*
```

Bintang harus menjadi byte terakhir kunci yang dievaluasi, bukan sekadar jalur URI. Perhitungkan query string atau akhiran variabel di `proxy_cache_key`. Kunci `*` saja menelusuri semua entri yang memenuhi syarat.

<a id="generation-invalidation"></a>

## Invalidasi generasi

Generasi kunci mencegah permintaan baru memakai konten lama setelah PURGE, termasuk respons lama yang baru selesai ditulis. Berkas dihapus secara terpisah. Namespace berbagi aturan invalidasi; pertahankan direktori statusnya saat memulai ulang.

<a id="conditional-refresh"></a>

## Memeriksa cache ke server asal (REFRESH)

REFRESH mengirim HEAD ke representasi sumber yang sama dengan ETag dan Last-Modified tersimpan. Jika halaman produk sudah pasti usang, gunakan PURGE. REFRESH tidak mengunduh isi baru, mengisi cache kosong, atau memperpanjang TTL. [HTTP / REFRESH](../REFRESH.md).

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

<a id="sample-configurations"></a>

## Contoh konfigurasi

Lima blok pertama merupakan contoh `nginx.conf` lengkap yang berdiri sendiri. Blok terakhir menambahkan penanganan Vary ke bagian `http` yang sudah ada. NGINX menerima koneksi lokal pada port 8080; `catalog_backend` adalah nama grup upstream untuk aplikasi di `127.0.0.1:8081`. `shop.example.com` adalah nilai HTTP Host yang dikirim ke aplikasi. Sesuaikan alamat dan gunakan direktori cache yang dapat ditulis oleh NGINX.

<a id="basic--inline-purge-method"></a>

### Dasar — metode purge inline

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        location / {
            proxy_pass        http://catalog_backend;
            proxy_cache       main;
            proxy_cache_valid 200 10m;
            proxy_set_header Host shop.example.com;
            add_header X-Cache-Status $upstream_cache_status always;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }
    }
}
```

```bash
curl -i -X PURGE -H 'Host: shop.example.com' \
  'http://127.0.0.1:8080/images/logo.png'
# 200 — entry purged
# 200 + Cache-Purge-Code: no_match
# 403 — client IP not in the allowed list
```

<a id="basic--separate-purge-location"></a>

### Dasar — location purge terpisah

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        location / {
            proxy_pass      http://catalog_backend;
            proxy_cache     main;
            proxy_cache_valid 200 10m;
            proxy_set_header Host shop.example.com;
            add_header X-Cache-Status $upstream_cache_status always;
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
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;
    cache_purge_queue_size        2048;
    cache_purge_batch_size        20;
    cache_purge_throttle_ms       10ms;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        location / {
            proxy_pass        http://catalog_backend;
            proxy_cache       main;
            proxy_cache_valid 200 10m;
            proxy_set_header Host shop.example.com;
            add_header X-Cache-Status $upstream_cache_status always;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }
    }
}
```

<a id="purge-all-entries"></a>

### Menghapus semua entri

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        location /content {
            proxy_pass        http://catalog_backend;
            proxy_cache       main;
            proxy_cache_valid 200 10m;
            proxy_set_header Host shop.example.com;
            add_header X-Cache-Status $upstream_cache_status always;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE purge_all from 127.0.0.1;
        }
    }
}
```

```bash
curl -i -X PURGE -H 'Host: shop.example.com' \
  'http://127.0.0.1:8080/content/anything'
# 200 — walk completed; inspect error_log for file errors
```

<a id="json-responses-with-per-location-override"></a>

### Respons JSON dengan pengaturan per location

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        # Default for all locations in this server.
        cache_purge_response_type json;
        location / {
            proxy_pass        http://catalog_backend;
            proxy_cache       main;
            proxy_cache_valid 200 10m;
            proxy_set_header Host shop.example.com;
            add_header X-Cache-Status $upstream_cache_status always;
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

<a id="migration"></a>

## Migrasi

Perubahan struktur SHM memerlukan restart penuh. Sintaks metode/ACL dan handler protokol tetap tersedia. v4 memakai JSON secara default, kode hasil yang sama pada header dan isi, serta `200 no_match` untuk entri yang tidak ada. `cache_purge_legacy_status` hanya menghasilkan peringatan. Perbarui klien sesuai [kontrak HTTP](../HTTP_RESPONSES.md). Kapasitas mencakup tugas yang sudah ditetapkan; kelebihan beban menunggu penerimaan. Duplikat aktif dapat menjadwalkan penelusuran ulang.

<a id="security"></a>

## Keamanan

- Batasi endpoint purge dengan `allow` / `deny` atau lapisan autentikasi sebelumnya. Jangan buka untuk publik.
- Atur `cache_purge_queue_size` sesuai lalu lintas untuk membatasi SHM saat banjir permintaan.
- Pertimbangkan `limit_req` agar laju purge dibatasi terpisah dari lalu lintas biasa.

<a id="documentation"></a>

## Dokumentasi

- [Referensi direktif](DIRECTIVES.id.md)
- [Contoh konfigurasi lainnya](README.id.md#sample-configurations)
- [Operasi dan pemecahan masalah](OPERATIONS.id.md)
- [HTTP dan batas REFRESH (Inggris)](../REFRESH.md)
- [Migrasi ke v4](MIGRATION.id.md)
- [Arsitektur implementasi (Inggris)](../ARCHITECTURE.md)
- [Pengujian](OPERATIONS.id.md#testing)
- [Pengembangan dan pengujian (Inggris)](../../t/TESTING.md)

<a id="see-also"></a>

## Lihat juga

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [Dokumentasi nginx](https://nginx.org/en/docs/)

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
