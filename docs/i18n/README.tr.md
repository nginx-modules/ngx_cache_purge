<a id="ngx_cache_purge"></a>

# ngx_cache_purge

`FastCGI`, `proxy`, `SCGI` ve `uWSGI` önbelleklerinden kayıt silen NGINX modülü. v4 sürümünde anahtar nesilleri, dosyalar silinmeden önce yeni isteklerin eski kayıtları kullanmasını engeller. REFRESH, proxy önbelleğindeki içeriğin değişip değişmediğini kaynak sunucuya sorar. İki özellik de açıkça yapılandırılmalıdır. PURGE sözdizimi korunur, ancak HTTP yanıtları değişir; [geçiş notlarına](README.tr.md#migration) bakın.

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

<a id="choosing-a-mode"></a>

## Gereken davranışı seçin

| Amaç | Yapılandırma | Yanıtın anlamı |
|---|---|---|
| Eski kayıtları silmek ve silme girişimini beklemek | Arka plan kuyruğu kapalı PURGE | `200 purged` veya `200 no_match`; hata durumunda 4xx/5xx |
| Öneke göre veya tüm önbellek için temizlik planlamak | PURGE ve `cache_purge_background_queue on` | `202 accepted`; temizlik beklerken eski kayıtlar sunulabilir |
| Geç yazılan dosyalar dahil eski içeriği yeni isteklere kapatmak | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`; yeni anahtar nesli kullanılır, dosyalar sonra silinir |
| Kaynak sunucudaki değişiklikleri denetlemek | Nesiller + `proxy_cache_refresh` + dahili profil | `200`, kayıt başına sonuç ve devam bilgisi; önbelleği önceden doldurmaz |

Bu mekanizmalar birlikte çalışır: nesil değişimi de temizlik planlar. Kuyruk tek başına geç yazmalara karşı koruma sağlamaz. [v4 örneğine](README.tr.md#generation-and-refresh-example), [PURGE örneklerine](EXAMPLES.tr.md#sample-configurations) ve [HTTP yanıtlarına](../HTTP_RESPONSES.md) (İngilizce) bakın.

<a id="compatibility"></a>

## Uyumluluk

Üretimde bakımı sürdürülen bir NGINX sürümü kullanın. Ana hedef POSIX/UNIX’tir; Windows desteği deneyseldir. İş parçacıkları isteğe bağlıdır. [NGINX / CI](../../t/TESTING.md#compatibility-matrix).

<a id="installation"></a>

## Kurulum

```bash
cd /path/to/nginx-source
./configure --add-module=/path/to/ngx_cache_purge
make
make install
```

Dinamik modül:

```bash
./configure --with-compat --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

Sunucuda kurulu NGINX sürümüyle aynı sürümün kaynak kodunu kullanın. `--with-compat`, farklı NGINX sürümleri veya C kütüphaneleri arasında koşulsuz uyumluluk sağlamaz. Bkz. [dinamik modüller ve sürüm arşivleri](../BINARY_MODULES.md) (İngilizce).

<a id="generation-and-refresh-example"></a>

## Örnek: ürün kataloğu

Bu tam örnek yerel 8080 portunu dinler ve istekleri 8081 portundaki uygulamaya iletir. Uygulama, Host `shop.example.com` için `/products/42` sunmalıdır. Adresi, alan adını ve yazılabilir önbellek/durum dizinlerini uyarlayın.

`shop.example.com`, HTTP Host başlığındaki alan adıdır; dizin değildir. `/products/42` ürün URI’sidir. `catalog:` sabit bir anahtar önekidir, sunucu adı değildir. Önek ve sorgu dizesini içeren tam URI, `catalog:/products/42` gibi mantıksal anahtarı oluşturur. REFRESH profili `catalog:` kısmını kaldırıp aynı uygulamadaki `/products/42` yolunu denetler.

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

Var olan, önbelleğe alınabilir bir ürünü `X-Cache-Status: HIT` görülene kadar GET ile isteyin; sonra REFRESH gönderin:

```sh
curl -i 'http://127.0.0.1:8080/products/42'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/42' \
  'http://127.0.0.1:8080/admin/refresh'
```

Ürün değiştiyse PURGE gönderin. Burada `200 invalidated`, yeni isteklerin eski nesli kullanmasını engeller; sonraki GET uygulamaya gider.

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/products/42'
```

Tüm ürünler için `catalog:/products/*` kullanın. `complete: false` ise seçiciyi koruyun ve `<next_cursor>` yerine dönen değeri koyun. Devam etmeden önce HTTP hatalarını giderin.

```sh
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

<a id="partial-key-purge"></a>

## Kısmi anahtarla temizleme

**Değerlendirilmiş önbellek anahtarına** `*` ekleyerek büyük/küçük harfe duyarsız önek eşleşmesi isteyin. Tam anahtar ve Vary eşleşmeleri harf boyutuna duyarlıdır. Fiziksel temizleme öneki, sondaki `*` hariç 511 bayt olabilir. Daha uzun önek bir kez günlüğe yazılır ve dosyalar taranmadan `400 invalid_selector` döndürür.

`cache_purge_key` ile mantıksal ifade eşleştirilir; sondaki `*` dahil 8096 bayt kabul edilir.

`$uri` ile biten ifade için bu istek URI öneki seçer:

```http
PURGE /images/header*
```

Yıldız yalnızca URI yolunun değil, değerlendirilmiş anahtarın son baytı olmalıdır. `proxy_cache_key` sorgu veya değişken eklerini hesaba katın. Tek başına `*`, tüm uygun girdileri tarar.

<a id="generation-invalidation"></a>

## Nesil geçersizleştirmesi

Anahtar nesilleri, PURGE sonrasındaki yeni isteklerin eski içeriği kullanmasını engeller; geç yazılan eski yanıtlar da buna dahildir. Dosyalar ayrıca silinir. Ad alanı geçersizleştirme kurallarını paylaşır; durum dizinini yeniden başlatmalar arasında koruyun.

<a id="conditional-refresh"></a>

## Önbelleği kaynak sunucuyla karşılaştırma (REFRESH)

REFRESH, önbellekteki ETag ve Last-Modified değerleriyle aynı kaynak gösterimine HEAD gönderir. Ürün sayfasının eski olduğunu biliyorsanız PURGE kullanın. REFRESH yeni gövde indirmez, boş önbelleği doldurmaz ve TTL’yi uzatmaz. [HTTP / REFRESH](../REFRESH.md).

| Ham kaynak sonucu | İşlem / sonuç |
|---|---|
| `304` | Koru; `kept` |
| `200`, `404`, `410` | Koşullu nesil geçersizleştirmesi ve ertelenmiş temizleme; `invalidated` |
| Diğer durum | Koru; `upstream_status` |
| Kaynak durumu yok / taşıma hatası | Koru; `transport_error` |
| Vary metaverisi | HEAD yok; `skipped_vary` |
| Geçersiz eşleme/profil | Koru; `skipped_profile` |
| Bu sırada nesil değişti | Yeni geçersizleştirme yok; `raced_replaced` |
| İç geçersizleştirme/ayırma hatası | Koru; `invalidation_error` |
| Sayfa durmadan gönderilemeyen/bitmeyen iş | `undispatched`; imleç üzerinden geçmez |

<a id="migration"></a>

## Geçiş

SHM yapısı değişiklikleri tam yeniden başlatma gerektirir. Yöntem/ACL sözdizimi ve protokol işleyicileri korunur. v4 varsayılan JSON, başlık ve gövdede aynı sonuç kodları ve bulunamayan kayıtlar için `200 no_match` kullanır. `cache_purge_legacy_status` yalnızca uyarı üretir. İstemcileri [HTTP sözleşmesine](../HTTP_RESPONSES.md) göre güncelleyin. Kapasite atanmış görevleri de içerir; aşırı yükte istek kabul bekler. Etkin yinelenen görevler yeni bir geçiş planlayabilir.

<a id="security"></a>

## Güvenlik

- Purge uç noktalarını `allow` / `deny` veya öndeki kimlik doğrulamayla sınırlayın. Herkese açmayın.
- İstek selinde ortak belleği sınırlamak için trafiğe uygun `cache_purge_queue_size` belirleyin.
- Normal trafikten ayrı purge hız sınırlaması için `limit_req` kullanmayı değerlendirin.

<a id="documentation"></a>

## Belgeler

- [Yönerge başvurusu](DIRECTIVES.tr.md)
- [Diğer yapılandırma örnekleri](EXAMPLES.tr.md)
- [İşletim ve sorun giderme (İngilizce)](../OPERATIONS.md)
- [HTTP ve REFRESH sınırları (İngilizce)](../REFRESH.md)
- [v4 geçişi (İngilizce)](../MIGRATION.md)
- [Uygulama mimarisi (İngilizce)](../ARCHITECTURE.md)
- [Geliştirme ve testler (İngilizce)](../../t/TESTING.md)

<a id="see-also"></a>

## Ayrıca bakınız

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [nginx belgeleri](https://nginx.org/en/docs/)

<a id="license"></a>

## Lisans

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
