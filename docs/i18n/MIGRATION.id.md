# ngx_cache_purge

[README](README.id.md) · [Direktif](DIRECTIVES.id.md)

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
