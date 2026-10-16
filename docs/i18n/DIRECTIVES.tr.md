[ngx_cache_purge](README.tr.md)

<a id="directives"></a>

# Yönergeler

<a id="proxy_cache_purge"></a>

## `proxy_cache_purge`

```
Syntax:  proxy_cache_purge on | off;
         proxy_cache_purge <method> [purge_all] from all | <cidr> ...;
         proxy_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

**Yerinde biçim** (`from …`) — proxy location’da belirtilen HTTP yöntemini yakalayıp kaydı temizler. `on`, `PURGE` kısaltması; `off` kapatır. CIDR listesiyle sınırlayın veya `from all` kullanın. `from` öncesi `purge_all`, URI’den bağımsız bütün bölgeyi boşaltır.

**Ayrı location biçimi** (bölge/anahtar) — yalnızca temizleme location’ında, genellikle anahtarı yakalayan regex ile kullanılabilir. Bölgeyi adıyla bulur, derlenmiş anahtar ifadesini temizler. Aynı location’da `proxy_cache` ve `proxy_pass` ile uyumsuzdur.

<a id="fastcgi_cache_purge"></a>

## `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

`fastcgi_cache` / `fastcgi_cache_path` bölgeleri için `proxy_cache_purge` eşdeğeridir.

<a id="scgi_cache_purge"></a>

## `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

`scgi_cache` / `scgi_cache_path` bölgeleri için `proxy_cache_purge` eşdeğeridir.

<a id="uwsgi_cache_purge"></a>

## `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

`uwsgi_cache` / `uwsgi_cache_path` bölgeleri için `proxy_cache_purge` eşdeğeridir.

<a id="cache_purge_response_type"></a>

## `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

Başarılı PURGE yanıtlarının gövde biçimini seçer; varsayılan JSON’dur. `Cache-Purge-Code` ile gövdedeki sonuç kodu aynıdır. Modül yanıtları `Cache-Control: no-store` içerir; hatalar RFC 9457 uyarınca `application/problem+json` kullanır. Metin anahtarları uygun biçimde kaçışlanır, ikili anahtarlar kodlama bilgisiyle birlikte Base64 olarak sunulur.

`none`, PURGE gövdesini kaldırır: tamamlanan başarı `204`, kabul edilen arka plan işi `202`, hatalar 4xx/5xx döndürür. Durum ve REFRESH uç noktaları yapılandırılmış JSON kullanmayı sürdürür. Kodlar, alanlar ve sınırlar için [HTTP sözleşmesine](../HTTP_RESPONSES.md) bakın.

<a id="cache_purge_background_queue"></a>

## `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

Joker ve `purge_all` için ertelenmiş işlem açar. Kuyruğa alındığında veya aynı bekleyen/etkin görevle birleşince `202 Accepted` döner. Tam anahtar temizliği değişmez.

Eski joker/tam temizleme davranışını kontrol eder. `cache_purge_key` bağlıysa bu kapalı olsa bile ertelenmiş fiziksel temizleme ve mantıksal `200` kullanılır.

Kuyruk dolu veya ayırma başarısızsa istek kabule kadar açık kalır, beklerken tarama yapmaz. Bekleyen/etkin görevin tekrarı kapasitede kabul edilebilir. Küçük bekleme durumu ayrılamazsa `500` döner. Bağlantı temizliği zamanlayıcıyı iptal edip bekleme sayacını bırakır.

Bu seçenek ve `cache_purge_vary_aware` için son bildirilen değer geçerlidir. Geçersiz değerler reddedilir. `cache_purge_legacy_status` eskimiştir ve yok sayılır. İş parçacıkları ve yaşam döngüsü: [yürütme modeli](../ARCHITECTURE.md#execution-model).

<a id="cache_purge_queue_size"></a>

## `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

Tüm yolların en fazla bitmemiş görev sayısı. Bekleyen ve atanmış görevler kapasite tüketir. Tekrarlar aynı ayırmayı paylaşır; yeniden geçiş belleği ve kabul hakkını korur. Farklı görev boş yer bekler.

Arka plan veya durum uç noktası yapılandırılınca ortak bölge oluşturulur. Yuvarlanmış boyut aynı kalsa da kapasite değişimi yeniden başlatma gerektirir. Reload, işi korumak için yeniden boyutlandırmayı reddeder. Kök eklemek de bellek ve yeniden başlatma gerektirebilir; bitmesi gereken bekleyen/etkin işleri önce boşaltın.

Ad alanı tanımlamak da görev kuyruğunu oluşturur.

Bölge görevleri, yol metaverisini ve hizalı worker ölçümlerini içerir; derleme ve yol sayısına bağlıdır. [Bellek hesabına](../OPERATIONS.md#memory-sizing) bakın.

<a id="cache_purge_thread_pool"></a>

## `cache_purge_thread_pool`

```
Syntax:  cache_purge_thread_pool <name>;
Default: default
Context: http, server, location
```

Kabul edilen arka plan işinin yürütücüsünü normal miras kurallarıyla seçer. Örnek:

```nginx
thread_pool purge_io threads=2 max_queue=128;
http {
    cache_purge_background_queue on;
    cache_purge_thread_pool purge_io;
}
```

NGINX havuzları doğrular, tamamlanmayı yerel görev olayıyla iletir; özel havuz yapısı/tamamlanma kuyruğu kopyalanmaz. İş parçacığı varsa özel havuz mevcut olmalıdır. Yoksa veya Windows’ta yönerge kabul edilip sınırlı olay dilimleri kullanılır. Dolu havuz yürütmeyi erteler, G/Ç’yi olay döngüsüne geri taşımaz. Ayrı havuzlar yığın ve bağlam geçişi maliyeti ekler.

<a id="cache_purge_batch_size"></a>

## `cache_purge_batch_size`

```
Syntax:  cache_purge_batch_size <number>;
Default: 10
Minimum: 1
Maximum: 64
Context: http
```

Worker’ın beklemeler arasındaki en fazla görevi. İlerleme dilimler ve iş parçacığı tamamlanmaları arasında korunur. Boş kuyruk grubu bitirir. 64 üzeri uyarıyla sınırlanır; sıfır reddedilir.

Dosya veya callback süresini değil görevleri kontrol eder. Küçük değer aralara daha çok bekleme koyar, çalışan iş parçacığı taramasını kesmez. Disk yükü için `cache_purge_throttle_ms` ile birlikte ayarlayın.

<a id="cache_purge_throttle_ms"></a>

## `cache_purge_throttle_ms`

```
Syntax:  cache_purge_throttle_ms <time>;
Default: 10ms
Minimum: 1ms
Context: http
```

Gruplar arası gecikme. `10ms`, `500ms`, `1s` ve `"2s 500ms"` gibi tırnaklı bileşikler kabul edilir. Birimsiz tamsayı milisaniyedir: `10`, `10ms` ile eşittir. Birimsiz değer uyarı üretir; açık birim bunu önler.

Boş worker bu sürenin on katı sonra kontrol eder. Bitmemiş dilim ve meşgul mutex denemeleri 1 ms kullanır. Bunlar asgari zamanlama aralıklarıdır, son süre garantisi değildir. Sıfır ve platform güvenli zamanlayıcı aralığını aşan değerler reddedilir.

Kabul, havuz baskısı ve ertelenmiş temizleme tekrarları 10 ms yoklama kullanır.

<a id="cache_purge_queue_status"></a>

## `cache_purge_queue_status`

```
Syntax:  cache_purge_queue_status <cache_path>;
Default: not configured
Context: location
```

GET/HEAD için `Cache-Control: no-store` ile JSON görüntüsü sunar. Yol yapılandırılmış kökü göstermelidir; bilinmeyen yol, arka plan kapalıyken bile yapılandırmayı reddettirir. Erişimi kısıtlayın:

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

`queue`, bütün yolların ortak kuyruğudur:

| Alan | Tür / birim | Anlam |
|---|---|---|
| `size` | Tamsayı / görev | Worker bekleyenler |
| `running` | Tamsayı / görev | Havuz bekleyenler dahil atanmışlar |
| `max_size` | Tamsayı / görev | Bekleyen ve atanmış toplam kapasite |
| `wait_time` | Tamsayı / milisaniye | En eski bekleyenin yaşı; boşsa sıfır |
| `full` | Boolean | Örneklenen bekleyen/atanmış toplamı kapasiteye ulaştı |

`cache`, bu location’ın seçtiği kökü tanımlar:

| Alan | Tür / birim | Anlam |
|---|---|---|
| `pending.total` | Tamsayı / görev | Bu önbellekte bekleyenler |
| `pending.full` | Tamsayı / görev | `pending.total` içindeki bekleyen `purge_all` |
| `active` | Tamsayı / görev | Tamamlanma hesabı bitene kadar atanmışlar |
| `requests.overflow` | Tamsayı / istek | Kilit/bellek baskısı dahil kabul beklemesine girenler |
| `requests.deduplicated` | Tamsayı / istek | Bekleyen veya çalışan işle birleşenler |
| `requests.waiting` | Tamsayı / istek | Bu önbelleğin mevcut kabul bekleyenleri |
| `files.deleted` | Tamsayı / dosya | Geçiş sonunda bildirilen başarılı arka plan silmeleri |
| `files.errors` | Tamsayı / hata | Zaten eksik dosya dışındaki silme hataları |
| `files.bytes` | Tamsayı / bayt | Silinen dosyaların tahmini mantıksal boyutları |
| `responses.hit`, `responses.miss` | Tamsayı / ana istek | Log aşamasında gözlenen çekirdek HIT/MISS |
| `responses.hit_ratio` | Sayı / 0..1 | HIT / (HIT + MISS); gözlem yoksa sıfır |
| `logical.invalidations` | Tamsayı / karar | Tekrarlar ve REFRESH dahil yayımlanmış nesil geçersizleştirmeleri |
| `cleanup.scan_errors` | Tamsayı / hata | Arka plan metaveri/açma/okuma/kapama/ayırma hataları |
| `cleanup.retries` | Tamsayı / geçiş | Temizleme hatası nedeniyle ek geçişler |
| `cleanup.exhausted` | Tamsayı / görev | Üç başarısız yeniden denemeden sonra kaldırılanlar |

`pending.full` tam önbellek işlemlerini sayar; `*` anahtarlı joker farklı türdür. `active` havuz bekleyenleri içerir; `queue.size` ve `pending.total` dışında olsa da kabul kapasitesini tüketir.

Birleşik ad alanı taramasında `pending.full`, ilk kabul edilenin sınıfıdır. REFRESH tarama/yoklamaları kuyruk derinliğine veya silme hatalarına dahil değildir. Ertelenen tekrarın döndürülmesi, `wait_time` değerini en büyük yaş yerine örneklenen başın yaşına dönüştürebilir.

Her süreç slotunda olay döngüsünden tek sayaç yazıcısı vardır; başlangıç/adımlar önbellek satırına hizalıdır. İş parçacıkları sonuçları tamamlanma olaylarıyla verir. Durum okuyucusu mutex veya sıfır artış dahil atomic fetch-add kullanmaz. 64 bitte normal hizalı volatile okuma/yazma; 32 bitte sınırlı sıra denetimleri 64 bit okumayı korur, meşgul bölüm bir örnekte atlanabilir. Alanlar yaklaşık, worker’lar arası işlem değildir. Kabul, ölçüm yerine kilitli kontrol durumunu kullanır.

Birikimli sayaçlar unsigned 64 bit üst sınırında doyar, reload’da korunur, yeniden başlatmada sıfırlanır. Bekleme göstergesi istek temizliğini izler. Eşzamanlı işlemler ve çekirdek cache manager silmeleri arka plan toplamına dahil değildir. Baytlar dosya uzunluğu tahminidir; blok veya boşalan alan kanıtı değildir. Açık okuyucular/eşzamanlı yazmalar sonucu değiştirir. Kısmi sonuçlar sayılır; çöken worker sonuçları eksik olabilir. Denetim günlüğü değildir.

HIT/MISS yalnızca yerel önbellek kararlı ana istekleri sayar. Eksik upstream, sıfır durum, BYPASS, EXPIRED, STALE, UPDATING, REVALIDATED orana girmez. Normalde önbelleksiz istek MISS olmaz. İzleme tek başına, arka plan kapalıyken SHM oluşturabilir. 32 bitte kuyruk yaşı yaklaşık 24,9 günden kısa aralıkları kapsar; negatif işaretli saat farkı sıfır bildirilir.

<a id="cache_purge_legacy_status"></a>

## `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

Yalnızca yapılandırma geçişi için kabul edilen eski yönergedir. Her iki değer de uyarı üretir ve yok sayılır; yönergeyi kaldırın. Geçersiz değerler reddedilir. Eşleşme bulunmayan eşzamanlı işlem `200 no_match`, kabul edilen fiziksel temizlik `202 accepted`, yayımlanan nesil geçersizleştirmesi `200 invalidated` döndürür. `412`/`404` seçimi kaldırılmıştır. [HTTP sözleşmesine](../HTTP_RESPONSES.md) bakın.

<a id="cache_purge_vary_aware"></a>

## `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

`on` iken başarılı tam anahtar temizliği ana dosyayı siler, sonra aynı `KEY:` dizeli diğer dosyaları tarayıp kaldırır. Aynı mantıksal anahtarlı farklı yollardaki `Vary` ve `gzip_vary` çeşitlerini kapsar. Anahtardan hemen sonraki baytın `\n` olması denetlenir; yalnızca ortak önek yüzünden yanlış eşleşme önlenir.

Her tam anahtar temizliği bütün dizini taradığı için varsayılan olarak kapalıdır. Birden fazla `Vary`/`gzip_vary` gösterimi için açın. Ana kayıt yoksa da tarama yapılır. Jokerler ve `purge_all` zaten varyantları kapsar. Tam Vary anahtarları büyük/küçük harfe duyarlıdır ve en fazla 511 bayttır; daha uzun anahtarlar herhangi bir silme yapılmadan `400 invalid_selector` döndürür.

Nesil geçersizleştirmesi bu seçenek olmadan mantıksal anahtarın bütün gösterimlerini kapsar.

<a id="cache_purge_namespace"></a>

## `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

Location bağlanmadan nesil ad alanını tanımlar. Ad 1–32 ASCII harf, rakam, `_` veya `-` içerir; SHM en az sekiz yerel sayfa olmalıdır. Önbellek/geçici kökler dışında ayrılmış kalıcı durum dizini kullanın. Çekirdek yeniden başlatma dönemi ayırmak için kalıcı, özel oluşturulan işaret dosyası yaratır. İlgili önbellek var olabildiği sürece bu küçük dosyaları tutun; önbelleği koruyup durumu silmek eski dönem kullanımına yol açabilir. Elektrik kesintisi/fsync dayanıklılığı garantisi yoktur. Normal reload dönemi/kuralları korur; boyut/yol değişimi veya ad alanının kaldırılması tam yeniden başlatma gerektirir. Ad alanı ve görev kuyruğunun SHM boyutları ayrıdır.

<a id="cache_purge_key"></a>

## `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

Location’ı mevcut ad alanına bağlayıp `$cache_purge_key` sunar. İlgili ayrı PURGE location’ı dahil `proxy_cache_key`, `fastcgi_cache_key`, `scgi_cache_key` veya `uwsgi_cache_key` değerinin tamamını bu değişkene ayarlayın. Açılmış yerel anahtar üretilenden farklıysa PURGE, sıkı geçersizleştirme iddiası yerine `500` verir. `$cache_purge_key` kendi mantıksal ifadesinde kullanılmamalıdır. Mantıksal üst sınır 8096 bayt. Eski davranış gereken diğer location’ları bağlamayın; miras alınan bağı iptal eden `off` yoktur.

<a id="proxy_cache_refresh"></a>

## `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

Yalnızca ana istekler için açık REFRESH uç noktası oluşturur; diğer yöntemler `405` alır. `cache_purge_key` bağı, yerel ifade olarak `$cache_purge_key` ve refresh profili gerekir. Açılmış mantıksal anahtar tam eşleşme, sonu `*` önek veya bu yoldaki ad alanı için `*` seçer. Çekirdek erişim/kimlik doğrulamasıyla koruyun. REFRESH yalnızca proxy; PURGE dört protokolde de vardır.

<a id="cache_purge_refresh_profile"></a>

## `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

Bilinen bir gösterimi sabit iç proxy location’ına eşler. Harfe duyarlı tam önek çıkarılınca `/` ile başlayan kaynak URI kalmalı; sorgu/kaçışlar korunur. Modül host tahmin etmez, anahtarda `://` aramaz. Hedefte `internal`, kapalı önbellek, kapalı istek başlığı/gövdesi iletimi, HEAD, boş olmayan sabit Host ve sabit gösterim başlıkları gerekir. Koşullu başlıklar tam olarak `$cache_purge_refresh_etag` ve `$cache_purge_refresh_last_modified` kullanır; `$cache_purge_refresh_uri`, `proxy_pass` URI’sini sağlar. Bu üç saf değişken yalnızca yoklamada anlamlıdır. Hedef, iç yönlendirmeler dahil preaccess aşamasında denetlenir. Yönetici, anahtarla aynı kaynak gösterimini garanti etmelidir; özgün istek başlıkları yerel metaveride bulunmadığından Vary atlanır.

Yoklama location’ında `proxy_intercept_errors off` kullanın; kaynak hatasının geçersizleştiren başarıya çevrilmesini önlemek için miras alınan interception reddedilir.

<a id="cache_purge_refresh_timeout"></a>

## `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

Worker tarayıcısını bekleme dahil REFRESH sayfasında yeni tarama/yoklama gönderimini sınırlar. Süre dolunca yeni iş durur, yanıt eksik işaretlenir. Etkin HEAD, açıkça ayarlamanız gereken `proxy_connect_timeout`, `proxy_send_timeout`, `proxy_read_timeout` sınırlarıyla biter. Kesin yanıt son süresi değildir, syscall kesemez. Kopma/zarif kapanış yeni gönderimi keser; etkin thread belleği yerel tamamlanmaya kadar tutulur.
