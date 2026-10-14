<a id="ngx_cache_purge"></a>

# ngx_cache_purge

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

`FastCGI`, `proxy`, `SCGI` ve `uWSGI` önbelleklerini temizleyen NGINX modülü. 4.0 sürümü eski arayüzü korur; ertelenmiş dosya silmeden önce isteğe bağlı nesil geçersizleştirmesi ve proxy önbellekleri için sınırlandırılmış koşullu REFRESH ekler.

---

<a id="contents"></a>

## İçindekiler

- [Özellikler](#features)
- [Uyumluluk](#compatibility)
- [Kurulum](#installation)
- [Yürütme modeli](#execution-model)
- [Yönergeler](#directives)
- [Kısmi anahtarla temizleme](#partial-key-purge)
- [Nesil geçersizleştirmesi](#generation-invalidation)
- [Koşullu REFRESH](#conditional-refresh)
- [Yapılandırma örnekleri](#sample-configurations)
- [Performans ayarı](#performance-tuning)
- [İzleme ve hata ayıklama](#monitoring-and-debugging)
- [Sorun giderme](#troubleshooting)
- [Testler](#testing)
- [Geçiş](#migration)
- [Geçici dizinler](#temporary-directories)
- [Güvenlik](#security)
- [Lisans](#license)

---

<a id="features"></a>

## Özellikler

- **Yerinde temizleme** — IP erişim denetimli özel HTTP yöntemi (`PURGE`)
- **Ayrı temizleme location’ı** — `proxy_pass` olmadan, düzenli ifadeyle yakalanan anahtarlar için bölge/anahtar sözdizimi `proxy_cache_purge zone key`
- **Joker / kısmi temizleme** — sondaki `*`, önbellek dizinini gezerek eşleşenleri siler
- **`purge_all`** — bir istekte bölgedeki tüm kayıtları siler
- **Arka plan kuyruğu** — sınırlı olay döngüsü dilimleri veya isteğe bağlı NGINX iş parçacığı havuzunda ertelenmiş tarama; ayarlanabilir görev grupları ve beklemeler
- **Vary desteği** — tam anahtar temizliğinden sonra aynı anahtarlı gzip/Vary dosya çeşitlerini de siler
- **Yanıt biçimleri** — `json` (varsayılan), `html`, `xml`, `text`, `none`
- **Adlandırılmış havuzlar** — miras alınan seçim; iş parçacıksız derlemelerde ve Windows’ta artımlı yürütme
- **Etkin görev tekilleştirmesi** — bir indeksli işlem ve tarama sırasında gelen tekrarlar için en fazla bir bekleyen yeniden tarama
- **Durum** — ortak kuyruk mutex’ini almadan okunan, önbellek bazında kuyruk, silme ve HIT/MISS ölçümleri
- **Nesil anahtarları** — isteğin işlenme yolunda disk G/Ç’si olmadan tam anahtar, önek ve ad alanı geçersizleştirmesi; geç tamamlanan eski yazmalar erişilemez kalır
- **Koşullu REFRESH** — açık HEAD profili, ortak yoklama sınırı ve 32 kayıtlık devam sayfaları

---

<a id="compatibility"></a>

## Uyumluluk

Tablo CI’da yapılandırılmış hedefleri gösterir. Başarılı iş, o kaynak/derleme yapılandırmasını doğrular; serinin bütün sürümlerini kapsamaz.

| NGINX | CI: push / PR | CI: + full |
|---|---|---|
| 1.30.5, 1.31.6 | `test-matrix` | — |
| 1.31.6 | `build-compatibility` (GCC/Clang), `sanitizer`, `native-macos`, `native-windows` (x64/x86) | `memory-leak-test` (Valgrind) |
| 1.7.9 | `build-compatibility` (GCC) | — |
| 1.7.11, 1.7.12 | `build-compatibility` (GCC) | — |
| 1.20.2, 1.26.3, 1.28.3 | — | `test-matrix` |
| 1.11.4, 1.11.5 | — | `build-compatibility` (GCC) |

Linux işleri, yerel çalıştırmalarla aynı Docker görüntülerini ve test girişini kullanır. Yerel macOS/Windows işleri kendi dosya ve süreç API’lerini sınar; Windows MSVC ile derlenir, `nginx.exe` üzerinde HTTP, yol ve reload denetimleri yapar. POSIX süreç/sinyal senaryoları Linux ve macOS’ta çalışır; Windows bunları veya isteğe bağlı NGINX havuz yolunu çalıştırmaz.

`--with-threads` yoksa artımlı tarama kullanılır. NGINX 1.7.12 öncesindeki epoll derlemelerinde çekirdeğin bildirim yarışını önlemek için de bu kip kullanılır. BSD için yerel CI yoktur. Normal çalıştırmalar güncel stable/mainline sürümlerini ve API sınırlarını; genişletilmiş çalıştırmalar ayrıca eski sürümleri ve Valgrind’i denetler. Yalnızca belge değişiklikleri derlemeleri atlar. Takvim ve komutlar: [testler](../../t/TESTING.md).

Kod; yapılandırma, istek işleme, kuyruk, tarama, geçersizleştirme, REFRESH ve yanıt bileşenlerine ayrılmıştır. Ortak yol işlevleri `ngx_cache_purge.h`, platform işlemleri POSIX/Win32 başlıklarındadır. `generation.*`, `invalidation.*` olarak yeniden adlandırılmıştır; nesil kavramı hâlâ anahtar sürümünü belirtir. [Mimariye](../ARCHITECTURE.md) bakın.

Modül NGINX’in C89 uyumlu tarzını ve çekirdek olay, slab, rbtree, atomik API’lerini kullanır; C11 atomikleri veya yeni dosya API’si gerektirmez. Sıkı GNU89 derlemeleri bildirim sırası ve uyarıları denetler. Gerçekte sınanan derleyici/platform sonuçlarla kaydedilir.

---

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
./configure --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

---

<a id="execution-model"></a>

## Yürütme modeli

**Görev**, bir joker veya `purge_all` işlemidir. **Grup**, worker’ın bekleme öncesi işlediği görev sayısıdır. **Dilim**, artımlı dizin gezisinin sınırlı adımıdır; bir görev birçok dilim gerektirebilir.

Nesil anahtarlarında her önbellek yolu/ad alanı için tek fiziksel tarama kabul edilen geçersizleştirmeleri birleştirir. REFRESH sayfaları kuyruk görevi değil, ayrı HTTP işlemleridir.

| İstek ve yapılandırma | Dosya taraması | HTTP tamamlanması |
|---|---|---|
| Eski tam anahtar, Vary ile veya olmadan | İstek worker’ı; Vary dizin taraması ekler | İşlem sonrası |
| Joker veya `purge_all`, kuyruk kapalı | İstek worker’ında eşzamanlı tarama | Tarama sonrası |
| Arka plan görevi, seçili havuz | Worker başına aynı anda bir havuz görevi | Kabul sonrası `202` |
| Arka plan görevi, kullanılabilir havuz yok | Worker olay döngüsünde artımlı dilimler | Kuyruğa alma sonrası `202` |
| Kuyruk kapasitesi/bellek tükendi | İstek tarama yapmadan kabul bekler | Kabul sonrası `202` |
| Nesil PURGE, her anahtar biçimi | Ertelenmiş ad alanı taraması; istekte önbellek açma/silme yok | Yayımlama ve kabul sonrası `200 invalidated` |
| REFRESH uç noktası | Sınırlı tarama ve en fazla 32 ardışık HEAD | Tamamlanma/devam alanlarıyla `200` JSON |

Nesil anahtarı olmayan satırlar eski davranıştır. Kapasite bekleyen nesil isteği kabul edilince `202` değil `200` döner.

Bir dilim en fazla 64 dizin yinelemesinde veya 5 ms geçtiği gözlenince durur; atlanan girdiler ve dizin sonları dahildir. Tek bir sistem çağrısının süresini sınırlamaz.

Arka plan dosya G/Ç’sini iş parçacıklarına taşımak için `--with-threads` ile derleyip `http` dışında **main** bağlamında çekirdek havuzu tanımlayın:

```nginx
thread_pool default threads=2;
```

`cache_purge_thread_pool <name>`, `http`, `server` veya `location` içinde mevcut havuzu seçer. Varsayılan `default`, tanımlanmadan kullanılırsa NGINX standart ayarlarıyla oluşturur. Özel adlar çekirdek `thread_pool` bildirimi gerektirir. Seçim miras alınır; ilk kabul edilen istek birleşik işin yürütücüsünü belirler. Her worker’ın havuzları ayrıdır. Havuz doluysa görev kabul hakkını koruyarak 10 ms sonra yeniden gönderilir, eşzamanlı tarama başlamaz. İş parçacıksız derleme, Windows, tek süreç modu veya eski görevin havuzunun reload ile kaldırılması durumunda artımlı imleç kullanılır.

REFRESH, worker başına ek bir tarayıcı için seçili havuzu kullanır. Yerel HTTP alt istekleri olay döngüsündedir; G/Ç iş parçacığı onların durumuna erişmez.

Kuyruk/indeks yapısal değişiklikleri slab mutex’iyle korunur. Kabul ve arka plan callback’leri trylock ve zamanlayıcıyla yeniden deneme kullanır. Bekleyen **ve çalışan** görevler yol başına çekirdek dize rbtree’sinde kalır; tam anahtar karşılaştırması hash çakışmalarını çözer. `Q` bitmemiş görev için indeks `O(log Q)`; yol arama, kayıtlı yol sayısına göre doğrusaldır. `ngx_queue_t` listeleri bekleyen/etkin işi izler. Çalışan görevin ilk tekrarı yeniden geçiş ister, diğerleri birleşir. Mevcut geçişten sonra tekrar kuyruğun sonuna gider; sonraki PURGE kaybolmaz. Sürekli yeni purgeler daha fazla geçiş doğurabilir. Kabul hakkı yalnızca kesin bitişte bırakılır. Dosya G/Ç’si ve yanıt biçimleme kilit dışındadır.

Rbtree yalnızca fiziksel temizlemeyi tekilleştirir. Nesil kuralları ayrı SHM bölgesindeki sınırlı bayt trie’larında tutulur. Sıkı ad alanı taramasının indeks kimliği eski anahtar görevinden farklıdır; REFRESH kararları aynı temizleme yoluna katılır.

Kuyruk dolması veya görev ayırma hatasında istek, 10 ms zamanlayıcı ve çekirdek bağlantı kesilmesi yönetimiyle bekler. İstekler/temizleme handler’ları kendi worker’ında kalır; SHM istek işaretçisi içermez. HTTP/2 ve HTTP/3 bağlantısının tüm okuması kapatılmaz. Kuyruk baskısı 429/503 üretmez; okunmuş kısa PURGE, TCP Zero Window garanti etmez. Bekleyen istekler bellek/bağlantı tüketir; çekirdek zaman aşımı ve kabul politikalarını bağlantı bütçesine göre ayarlayın.

Kabul edilen görevler normal reload boyunca anahtar, havuz ve geçici yol korumalarını saklar. Yeni tekrar, bekleyen görevi veya sonraki geçişi günceller. Yeni kabulleri kapatmak mevcut işi korur. Zarif kapanan worker mevcut geçişi bitirir, yeni görev almaz. Hızlı çıkış, bitmemiş artımlı görevi çıkış hook’unda taramayı tamamlamadan kuyruğa döndürür. Gönderilmiş iş parçacığı görevlerinin yaşamını çekirdek havuz kapanışı yönetir.

PID/slot dönemleri atanmış görevleri tanımlar. Worker’lar saniyede bir, geçiş başına en fazla 64 etkin kaydı kontrol edip sahibi çıkmış işi yeniden kuyruğa alır. Bu sahipsiz işi kurtarır; işlemsel günlük değildir. Ortak liste/slab güncellemesinde çökme durumu bozup yeniden başlatma gerektirebilir. Çökme öncesi silmeler ölçümlerde eksik kalabilir. Bekleyen işler/tekrarlar çalışan worker gerektirir; bitmeleri önemliyse durmadan önce kuyruğu boşaltın. Kuyruk kalıcı değildir.

Kuyruk iç yapısı yükseltme ABI’si değildir. Farklı yapıya geçerken, bitmesi gereken işleri boşaltıp tam yeniden başlatın. Aynı derlemenin normal reload’u bölgeyi yeniden kullanır.

Silme best effort’dur. `202`, tamamlanan silmeyi değil kabulü doğrular; biten tarama atomik görüntü değildir. Eşzamanlı istekler tarama sırasında/sonrasında kayıt oluşturabilir; hatalar loglanır. Joker/tam tarama her yerel düğümü eşzamanlı güncellemeden dosyaları siler. Tam anahtar ve Vary eşleşen düğümleri geçersizleştirir.

Başarısız arka plan geçişi 100 ms, 1 s ve 10 s sonra, en fazla üç kez tekrarlanır. Ertelenen görev kabul/tekilleştirme yerini tutar, diğerlerine izin verir. Yeni PURGE pencereyi sıfırlar. Denemeler tükenince görev kaldırılıp hata loglanır; depoyu düzelttikten sonra `cleanup.exhausted` değerini inceleyip yeniden deneyin. Fiziksel hata nesil geçersizleştirmesini bozmaz. Eksik dosya beklenen yarıştır; bozuk dosyalar sonsuza kadar denenmez, atlanır.

---

<a id="directives"></a>

## Yönergeler

<a id="proxy_cache_purge"></a>

### `proxy_cache_purge`

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

### `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

`fastcgi_cache` / `fastcgi_cache_path` bölgeleri için `proxy_cache_purge` eşdeğeridir.

<a id="scgi_cache_purge"></a>

### `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

`scgi_cache` / `scgi_cache_path` bölgeleri için `proxy_cache_purge` eşdeğeridir.

<a id="uwsgi_cache_purge"></a>

### `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

`uwsgi_cache` / `uwsgi_cache_path` bölgeleri için `proxy_cache_purge` eşdeğeridir.

<a id="cache_purge_response_type"></a>

### `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

Başarılı PURGE yanıtlarının gövde biçimini seçer; varsayılan JSON’dur. `Cache-Purge-Code` ile gövdedeki sonuç kodu aynıdır. Modül yanıtları `Cache-Control: no-store` içerir; hatalar RFC 9457 uyarınca `application/problem+json` kullanır. Metin anahtarları uygun biçimde kaçışlanır, ikili anahtarlar kodlama bilgisiyle birlikte Base64 olarak sunulur.

`none`, PURGE gövdesini kaldırır: tamamlanan başarı `204`, kabul edilen arka plan işi `202`, hatalar 4xx/5xx döndürür. Durum ve REFRESH uç noktaları yapılandırılmış JSON kullanmayı sürdürür. Kodlar, alanlar ve sınırlar için [HTTP sözleşmesine](../HTTP_RESPONSES.md) bakın.

<a id="cache_purge_background_queue"></a>

### `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

Joker ve `purge_all` için ertelenmiş işlem açar. Kuyruğa alındığında veya aynı bekleyen/etkin görevle birleşince `202 Accepted` döner. Tam anahtar temizliği değişmez.

Eski joker/tam temizleme davranışını kontrol eder. `cache_purge_key` bağlıysa bu kapalı olsa bile ertelenmiş fiziksel temizleme ve mantıksal `200` kullanılır.

Kuyruk dolu veya ayırma başarısızsa istek kabule kadar açık kalır, beklerken tarama yapmaz. Bekleyen/etkin görevin tekrarı kapasitede kabul edilebilir. Küçük bekleme durumu ayrılamazsa `500` döner. Bağlantı temizliği zamanlayıcıyı iptal edip bekleme sayacını bırakır.

Bu seçenek ve `cache_purge_vary_aware` için son bildirilen değer geçerlidir. Geçersiz değerler reddedilir. `cache_purge_legacy_status` eskimiştir ve yok sayılır. İş parçacıkları ve yaşam döngüsü: [yürütme modeli](#execution-model).

<a id="cache_purge_queue_size"></a>

### `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

Tüm yolların en fazla bitmemiş görev sayısı. Bekleyen ve atanmış görevler kapasite tüketir. Tekrarlar aynı ayırmayı paylaşır; yeniden geçiş belleği ve kabul hakkını korur. Farklı görev boş yer bekler.

Arka plan veya durum uç noktası yapılandırılınca ortak bölge oluşturulur. Yuvarlanmış boyut aynı kalsa da kapasite değişimi yeniden başlatma gerektirir. Reload, işi korumak için yeniden boyutlandırmayı reddeder. Kök eklemek de bellek ve yeniden başlatma gerektirebilir; bitmesi gereken bekleyen/etkin işleri önce boşaltın.

Ad alanı tanımlamak da görev kuyruğunu oluşturur.

Bölge görevleri, yol metaverisini ve hizalı worker ölçümlerini içerir; derleme ve yol sayısına bağlıdır. [Bellek hesabına](#memory-sizing) bakın.

<a id="cache_purge_thread_pool"></a>

### `cache_purge_thread_pool`

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

### `cache_purge_batch_size`

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

### `cache_purge_throttle_ms`

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

### `cache_purge_queue_status`

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

### `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

Yalnızca yapılandırma geçişi için kabul edilen eski yönergedir. Her iki değer de uyarı üretir ve yok sayılır; yönergeyi kaldırın. Geçersiz değerler reddedilir. Eşleşme bulunmayan eşzamanlı işlem `200 no_match`, kabul edilen fiziksel temizlik `202 accepted`, yayımlanan nesil geçersizleştirmesi `200 invalidated` döndürür. `412`/`404` seçimi kaldırılmıştır. [HTTP sözleşmesine](../HTTP_RESPONSES.md) bakın.

<a id="cache_purge_vary_aware"></a>

### `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

`on` iken başarılı tam anahtar temizliği ana dosyayı siler, sonra aynı `KEY:` dizeli diğer dosyaları tarayıp kaldırır. Aynı mantıksal anahtarlı farklı yollardaki `Vary` ve `gzip_vary` çeşitlerini kapsar. Anahtardan hemen sonraki baytın `\n` olması denetlenir; yalnızca ortak önek yüzünden yanlış eşleşme önlenir.

Her tam anahtar temizliği bütün dizini taradığı için varsayılan olarak kapalıdır. Birden fazla `Vary`/`gzip_vary` gösterimi için açın. Ana kayıt yoksa da tarama yapılır. Jokerler ve `purge_all` zaten varyantları kapsar. Tam Vary anahtarları büyük/küçük harfe duyarlıdır ve en fazla 511 bayttır; daha uzun anahtarlar herhangi bir silme yapılmadan `400 invalid_selector` döndürür.

Nesil geçersizleştirmesi bu seçenek olmadan mantıksal anahtarın bütün gösterimlerini kapsar.

<a id="cache_purge_namespace"></a>

### `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

Location bağlanmadan nesil ad alanını tanımlar. Ad 1–32 ASCII harf, rakam, `_` veya `-` içerir; SHM en az sekiz yerel sayfa olmalıdır. Önbellek/geçici kökler dışında ayrılmış kalıcı durum dizini kullanın. Çekirdek yeniden başlatma dönemi ayırmak için kalıcı, özel oluşturulan işaret dosyası yaratır. İlgili önbellek var olabildiği sürece bu küçük dosyaları tutun; önbelleği koruyup durumu silmek eski dönem kullanımına yol açabilir. Elektrik kesintisi/fsync dayanıklılığı garantisi yoktur. Normal reload dönemi/kuralları korur; boyut/yol değişimi veya ad alanının kaldırılması tam yeniden başlatma gerektirir. Ad alanı ve görev kuyruğunun SHM boyutları ayrıdır.

<a id="cache_purge_key"></a>

### `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

Location’ı mevcut ad alanına bağlayıp `$cache_purge_key` sunar. İlgili ayrı PURGE location’ı dahil `proxy_cache_key`, `fastcgi_cache_key`, `scgi_cache_key` veya `uwsgi_cache_key` değerinin tamamını bu değişkene ayarlayın. Açılmış yerel anahtar üretilenden farklıysa PURGE, sıkı geçersizleştirme iddiası yerine `500` verir. `$cache_purge_key` kendi mantıksal ifadesinde kullanılmamalıdır. Mantıksal üst sınır 8096 bayt. Eski davranış gereken diğer location’ları bağlamayın; miras alınan bağı iptal eden `off` yoktur.

<a id="proxy_cache_refresh"></a>

### `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

Yalnızca ana istekler için açık REFRESH uç noktası oluşturur; diğer yöntemler `405` alır. `cache_purge_key` bağı, yerel ifade olarak `$cache_purge_key` ve refresh profili gerekir. Açılmış mantıksal anahtar tam eşleşme, sonu `*` önek veya bu yoldaki ad alanı için `*` seçer. Çekirdek erişim/kimlik doğrulamasıyla koruyun. REFRESH yalnızca proxy; PURGE dört protokolde de vardır.

<a id="cache_purge_refresh_profile"></a>

### `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

Bilinen bir gösterimi sabit iç proxy location’ına eşler. Harfe duyarlı tam önek çıkarılınca `/` ile başlayan kaynak URI kalmalı; sorgu/kaçışlar korunur. Modül host tahmin etmez, anahtarda `://` aramaz. Hedefte `internal`, kapalı önbellek, kapalı istek başlığı/gövdesi iletimi, HEAD, boş olmayan sabit Host ve sabit gösterim başlıkları gerekir. Koşullu başlıklar tam olarak `$cache_purge_refresh_etag` ve `$cache_purge_refresh_last_modified` kullanır; `$cache_purge_refresh_uri`, `proxy_pass` URI’sini sağlar. Bu üç saf değişken yalnızca yoklamada anlamlıdır. Hedef, iç yönlendirmeler dahil preaccess aşamasında denetlenir. Yönetici, anahtarla aynı kaynak gösterimini garanti etmelidir; özgün istek başlıkları yerel metaveride bulunmadığından Vary atlanır.

Yoklama location’ında `proxy_intercept_errors off` kullanın; kaynak hatasının geçersizleştiren başarıya çevrilmesini önlemek için miras alınan interception reddedilir.

<a id="cache_purge_refresh_timeout"></a>

### `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

Worker tarayıcısını bekleme dahil REFRESH sayfasında yeni tarama/yoklama gönderimini sınırlar. Süre dolunca yeni iş durur, yanıt eksik işaretlenir. Etkin HEAD, açıkça ayarlamanız gereken `proxy_connect_timeout`, `proxy_send_timeout`, `proxy_read_timeout` sınırlarıyla biter. Kesin yanıt son süresi değildir, syscall kesemez. Kopma/zarif kapanış yeni gönderimi keser; etkin thread belleği yerel tamamlanmaya kadar tutulur.

---

<a id="partial-key-purge"></a>

## Kısmi anahtarla temizleme

**Değerlendirilmiş önbellek anahtarına** `*` ekleyerek büyük/küçük harfe duyarsız önek eşleşmesi isteyin. Tam anahtar ve Vary eşleşmeleri harf boyutuna duyarlıdır. Fiziksel temizleme öneki, sondaki `*` hariç 511 bayt olabilir. Daha uzun önek bir kez günlüğe yazılır ve dosyalar taranmadan `400 invalid_selector` döndürür.

`cache_purge_key` ile mantıksal ifade eşleştirilir; sondaki `*` dahil 8096 bayt kabul edilir.

`$uri` ile biten ifade için bu istek URI öneki seçer:

```http
PURGE /images/header*
```

Yıldız yalnızca URI yolunun değil, değerlendirilmiş anahtarın son baytı olmalıdır. `proxy_cache_key` sorgu veya değişken eklerini hesaba katın. Tek başına `*`, tüm uygun girdileri tarar.

---

<a id="generation-invalidation"></a>

## Nesil geçersizleştirmesi

Bağlı ad alanında PURGE yeni nesli yayımlar, eski nesiller için tekilleştirilmiş tarama planlar ve `invalidated` durumuyla `200` döner. Kabul yolunda önbellek dosyası açılmaz/silinmez. Yayımdan sonra anahtarı ilk değerlendiren yeni istek eski nesle HIT yapamaz. İstek, iç yönlendirmeler dahil ad alanı/mantıksal anahtar başına nesli sabitler; başlamış yanıt bitebilir ama geç yazması yeni istekler için HIT olamaz. Tam eşleşme harfe duyarlı; önek ASCII harfe duyarsızdır. `purge_all`, bilinçli paylaşılan diğer yollar dahil tüm ad alanını değiştirir. Fiziksel temizleme hedef yola alınır; diğerleri çekirdek sona ermesi veya sonraki purge ile temizlenir.

Kabul, hemen disk alanı kazanımı değildir. Tarama sonrası geç eski yazma erişilemez kalır; sonraki temizleme veya çekirdek inactive süresi kaldırır. Ad alanı SHM baskısında kurallar yeni ortak tabana daraltılır: geçersiz anahtar geri gelmez, ek anahtarlar soğur. Sonraki her geçersizleştirmede en fazla 64 ayrılmış trie düğümü geri kazanılır. Sık aynı anahtar purgesi ağacı sonsuz büyütmez. Karmaşıklık dosya sayısına değil sınırlı anahtar uzunluğuna/görev indeksine bağlıdır; aşırı yükte gerçek zamanlı O(1) veya aç kalmama garantisi yoktur. Kabul bekleyebilir. GET yığılmalarını `proxy_cache_lock` ve eşdeğerlerle yönetin.

<a id="conditional-refresh"></a>

## Koşullu REFRESH

REFRESH, TTL dolmadan hâlâ önbellekteki nesneyi doğrular; önceden doldurmaz, `304` ile TTL uzatmaz. Süresi dolmuş normal trafiği işleyen `proxy_cache_revalidate` ve `proxy_cache_background_update` tamamlayıcısıdır. Doğrulayıcıyı yok sayıp hep `200` veren kaynakta, geçersizleştirme ve ek HEAD olur. Yalnızca güncel, geçerli, kanonik hash yolundakiler adaydır. Aynı nesilde fiziksel değişim temkinli biçimde geçersizleştirilebilir; nesil karşılaştırması eski HEAD’in sonraki PURGE neslini bozmasını engeller.

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

Worker aynı anda en fazla bir etkin sayfa ve bir yoklama tutar; ortak sınır worker’lar arasında 16 yoklamadır. Diğer sayfalar aday dizisi ayırmadan bekler. Sayfa ortak imleçle tarar, en fazla 32 aday tutar, yerel HEAD alt isteklerini sırayla gönderir. Böylece bellek ve ana istek havuzunu paylaşan alt istekler sınırlanır. Aynı REFRESH istekleri tekilleştirilmez, eşzamanlılık sınırını paylaşır. Geçersizleştirmeleri ortak tekilleştirilmiş PURGE kuyruğunu kullanır.

İmleçsiz başlayın, sonra aynı uç noktaya aynı mantıksal seçici/profille `complete` doğru olana kadar `X-Cache-Purge-Cursor: <next_cursor>` gönderin. İmleç son tamamlanan 32 haneli hex dosya adıdır; geçersiz/tekrarlı başlık `400` alır. `scan_complete` başarılı dizin sayımını; `complete` ayrıca sonraki sayfa kalmadığını ve kararların bittiğini belirtir. `timed_out`, `scan_error` kesinti/hata ayrımıdır. `undispatched` seçilmiş bitmeyenleri sayar; eksik tarama toplam değil yalnızca alt sınırdır. Tarama eksikse yoklama çalışmaz, sonuç boş ve giriş imleci aynı kalır. Sonuçlar anahtar/kimlik bilgisi değil hash ve ham upstream durumlarını gösterir.

Her sayfa sonraki hash’ler için yeniden tarar: sınırlı bellek karşılığı tekrarlı G/Ç, toplam `O(files × pages)`. Eşzamanlı ekleme/silmede sayfalama zayıf tutarlıdır; daha sonra tam görüntü gerekiyorsa baştan başlayın. Seçici/süreleri buna göre ayarlayın. Büyük canlı önbellekleri sık taramak için uygun değildir.

<a id="generation-and-refresh-example"></a>

### Nesil ve REFRESH örneği

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

## Yapılandırma örnekleri

<a id="basic--inline-purge-method"></a>

### Temel — yerinde temizleme yöntemi

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

### Temel — ayrı temizleme location’ı

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

### Arka plan kuyruğu açık

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

### Tüm girdileri temizleme

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

### Location bazında değiştirilebilen JSON yanıtları

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

### Vary destekli temizleme

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

Anahtar temizlenince farklı yollardaki tüm gzip ve Vary çeşitleri otomatik silinir.

---

<a id="performance-tuning"></a>

## Performans ayarı

`queue_size` bitmemiş işi sınırlar, `batch_size` worker başına gruplar, `throttle_ms` grupları aralar. Değişiklik öncesi/sonrası gecikme, disk çekişmesi ve kuyruk yaşını ölçün. Daha büyük kapasite boşaltma hızını artırmaz.

Bunlar başlangıç ayarlarıdır, donanım performans iddiası değildir. Sütunlarda ortak `cache_purge_` öneki yoktur.

| İş yükü / depolama bütçesi | `queue_size` | `batch_size` | `throttle_ms` |
|---|---:|---:|---:|
| Kısıtlı makine/depolama | 512 | 5 | 25ms |
| Genel başlangıç | 2048 | 20 | 10ms |
| Boş kapasitesi ölçülmüş hızlı depolama | 8192 | 50 | 5ms |
| Temkinli boşaltmayla kısa patlamalar | 8192 | 5 | 50ms |

Genel başlangıç örneği:

```nginx
cache_purge_background_queue on;
cache_purge_queue_size        2048;
cache_purge_batch_size        20;
cache_purge_throttle_ms       10ms;
```

Büyük kuyruk kısa artışı emer; sürekli geliş tamamlanmadan fazlaysa yine dolar. Ek payı, tekilleştirme sonrası görev/s oranlarıyla `max(0, arrival_rate - completion_rate) * burst_seconds` görev olarak hesaplayın. Dengesiz süreler/mevcut birikim için pay bırakın. Bekleme açık istek/bağlantı ekler; depo dolu yükteyken bellek ve FD bütçesini hesaba katın.

<a id="memory-sizing"></a>

### Bellek hesabı

Bölge boyutu yerel yapı boyutlarından hesaplanır:

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

| Bileşen | Ayrılan/kullanılan bellek |
|---|---|
| Görev rezervi | `Q * S`; slab yuvarlaması ve gömülü anahtar/havuz adları dahil |
| Tek yol ölçümleri | `R * NGX_MAX_PROCESSES` ve hizalama; 192 bayt adım/1024 slot ile 192 KiB |
| Kuyruk kontrolü | `H`; süreç sahipliği ve dönem dizileri dahil |
| Yol rezervi | `M`; adlar, rbtree kökleri, hizalama, ölçüm dizileri dahil |
| Asgari bölge | Sekiz yerel sayfa |

Ölçülen LP64 düzeni (`I=184`, `H=20576`, yol kaydı 104, istatistik kaydı 144 bayt), 64 bayt önbellek satırı, 4 KiB sayfa ve tek 20 bayt yol için:

| Kapasite | Bölge baytı | Bölge MiB |
|---:|---:|---:|
| 512 | 1654784 | 1.58 |
| 2048 | 5373952 | 5.12 |
| 8192 | 20242432 | 19.30 |

Gerçek görev belleği slab öncesi `I + index_bytes + 1 + pool_name_bytes`. Gömülü indeks bir bayt işlem etiketi ve eski anahtar veya nesil taramasının ad alanıdır; eski `purge_all` yalnızca etiketi tutar. Kapasite bekleyen/etkin ayırmaları birlikte sınırlar. Yerel boyut, önbellek satırı, `NGX_MAX_PROCESSES` ve yollar sonucu belirler; Windows/32 bit farklıdır. Reload koruma kayıtları ek SHM tüketir; görev sınırından önce ayırma başarısız olabilir.

Süreç belleği ayrıdır. Etkin taramada sabit anahtar tamponu, yeniden kullanılan yol tamponu ve açık dizin başına sabit çerçeve vardır. Çerçeveler ata yollarını kopyalamadan önek uzunluklarını tutar: dosya sayısından bağımsız `O(depth + longest_path)`. Dizin akışlarının platform tamponları vardır. Bekleyen istek küçük zamanlayıcı/temizleme bağlamı tutar, imleç tutmaz. Thread yığınları ve çekirdek havuzları ayrı maliyettir.

Ad alanı kendi SHM boyutunu kullanır. Bir REFRESH tarayıcısı 283864 bayt sabit LP64 tampon/bağlam (32 kayıt), tarama çerçeveleri ve HTTP havuzunda en fazla 32 alt istek kullanır. Sayfa bitince istek bırakılır; devamı yeni istektir. Ek bekleyenler aday dizisi ayırmaz.

<a id="throughput-and-latency"></a>

### İş hacmi ve gecikme

Sürekli meşgul worker için yaklaşık hesap:

```text
B = tasks completed in a batch
T = throttle delay in milliseconds
D = elapsed batch processing time in milliseconds, including slice waits
rate_per_worker ~= 1000 * B / (D + T) tasks/s
```

`D` yok sayılırsa salt zamanlama üst sınırı `1000 * B / T`, varsayılanda worker başına 1000 görev/s olur. Dosya silme hızı veya ölçüm değildir. On görev toplam 200 ms ve bekleme 10 ms ise yaklaşık 47,6 görev/s. Depolama ortaktır; worker sayısıyla çarpmak garanti değildir. Boş worker yeni işi kontrol için `10 * T` ve olay gecikmesi kadar bekleyebilir.

Dinlenmeler arasındaki işi azaltmak için grubu küçültün veya beklemeyi uzatın. Diskte düşük worker gecikmesi için isteğe bağlı havuzu kullanın. Hiçbiri tek syscall’ı sınırlamaz veya thread görevini gruplara bölmez.

---

<a id="monitoring-and-debugging"></a>

## İzleme ve hata ayıklama

[Durum uç noktası](#cache_purge_queue_status) bekleyen/etkin görev, kabul bekleyen, tekrar, silme ve HIT/MISS gösterir. `202 Accepted` kabul/birleşmedir; silme veya anında mantıksal geçersizleştirme kanıtı değildir. Gecikmeyi kuyruk yaşı ve depolama çekişmesiyle birlikte izleyin.

| Seviye | Koşul |
|---|---|
| `warn` | Uzun anahtar, kaldırılmış havuz veya sınırlandırılmış ayar |
| `error` | Erişim reddi, önbellek arama/REFRESH profili hatası, tükenen tekrarlar; çekirdek havuz/Windows okuma hatası |
| `crit` | Açma/stat/silme veya dizin hatası; POSIX çekirdek okuma hatası |
| `alert` | Dosya kapama hatası |
| `emerg` | Geçersiz yapılandırma, uyumsuz SHM, boyutlandırma veya reload metaverisi ayırma hatası |
| `debug` | Ek istek/tarama bağlamı |

Tanılar yerel NGINX log işlevlerini ve `ngx_cache_purge: ` önekini kullanır. NGINX seviye, zaman, süreç/thread kimliği, errno ve istek/yapılandırma bağlamını ekler. Dosya mesajı gerçek başarısız işlemi ve tırnaklı yolu; ad alanı/REFRESH nesneyi ve ret nedenini belirtir. Standart NGINX error-log ayrıştırıcısıyla öneki filtreleyin.

Debug, `NGX_LOG_DEBUG_HTTP` kullanır; hem `--with-debug` derlemesi hem `error_log /var/log/nginx/error.log debug;` gerekir.

Kuyruk baskısı her tekrarda uyarı yerine bekleme ölçümleriyle gösterilir. Yok olmuş dosya beklenen yarıştır, modül tanısı üretmez. Çekirdek ayrıştırıcı, ayırıcı, G/Ç ve havuz mesajları yerel biçimlerini korur; teşhiste tüm loga bakın.

```bash
tail -f /var/log/nginx/error.log | grep 'ngx_cache_purge:'
```

Sınanan tanılar için [log denetimleri](../../t/TESTING.md#logging-contracts).

---

<a id="troubleshooting"></a>

## Sorun giderme

| Belirti | Denetim/ayar |
|---|---|
| Büyük taramada worker gecikmesi | Kuyruk ve disk için havuzu açın. Tam/Vary ve arka plan kapalı işlemler eşzamanlı kalır. |
| Kabul bekleyenler artıyor | Geliş, kuyruk yaşı, havuz ve diski kontrol edin. Ek kapasite patlamayı emer; boyut değişimi yeniden başlatma ister. |
| Yüksek iowait | `cache_purge_batch_size` azaltın, `cache_purge_throttle_ms` artırın, gelen purgeleri sınırlayın. |
| Kuyruk yavaş boşalıyor | Grubu büyütmeden/beklemeyi azaltmadan süre ve çekişmeyi ölçün. |
| İstemci eski `412`/`404` kodlarını bekliyor | `cache_purge_legacy_status` yönergesini kaldırın; `200` ve `Cache-Purge-Code: no_match` sonucunu işleyin. |
| Vary çeşitleri kalıyor | `cache_purge_vary_aware` açın; ana sonucu ve 511 bayt sınırını denetleyin. |
| Kabul edilen iş eksik görünüyor | Etkin görev, kurtarma ve dosya hatalarını inceleyin. `202` tamamlanma belgesi değildir; SHM bozulması şüphesi yeniden başlatma ister. |

---

<a id="testing"></a>

## Testler

Tüm takımı Docker’da derleyip çalıştırın:

```sh
make test-all
NGINX_VERSION=1.31.6 make test-all
```

`t/` içinden mevcut Compose servisleri kullanılabilir:

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

Compose eklentisinde `docker-compose` yerine `docker compose` kullanın; Makefile’lar ikisini algılar. `nginx-test` ve `make test-all`, yedi `t/*.t` takımını CI ile aynı `prove` girişinden çalıştırır. Entegrasyon görüntüsünde yerel yardımcı/yapı ve havuzlu çalışma testleri bulunur. Sıkı iş parçacıksız derlemeler artımlı yürütmeyi sınar. Her başarısız test komutu başarısız yapar.

Sıkı derleme, sanitizer, Valgrind, platform işleri ve raporlar için [`t/TESTING.md`](../../t/TESTING.md). Tüm GitHub işleri `ci.yml` içindedir.

---

<a id="migration"></a>

## Geçiş

<a id="queue-and-status-changes"></a>

### Kuyruk ve durum değişiklikleri

SHM yapısı değişiklikleri tam yeniden başlatma gerektirir. Yöntem/ACL sözdizimi ve protokol işleyicileri korunur. v4 varsayılan JSON, başlık ve gövdede aynı sonuç kodları ve bulunamayan kayıtlar için `200 no_match` kullanır. `cache_purge_legacy_status` yalnızca uyarı üretir. İstemcileri [HTTP sözleşmesine](../HTTP_RESPONSES.md) göre güncelleyin. Kapasite atanmış görevleri de içerir; aşırı yükte istek kabul bekler. Etkin yinelenen görevler yeni bir geçiş planlayabilir.

Duruma yeni alanlar eklenir, eski adlar korunur. `requests.overflow` artık kabul bekleyenleri sayar. Ölçümler yaklaşık ve birikimli 64 bittir. Thread derlemesi başvurulan `default` havuzunu otomatik kaydeder; G/Ç’yi ayırmak için adlandırılmış havuz kullanın. Yalnız durum yapılandırması SHM ayırır.

v4.0’da nesil ve REFRESH açıkça etkinleştirilir. Katılan tüm location’larda `cache_purge_key` bağlayıp `$cache_purge_key` değerini yerel anahtarın tamamı yapın. Fiziksel adlar değişir; eski dosyalar soğur, çekirdek süresini veya açık eski temizlemeyi bekler. Durum işaretlerini önbellekle koruyun. Eski PURGE location’ları REFRESH kazanmaz. Deneysel PR60 kullanıcıları yeni uç nokta/profil ve sayfalama kurmalıdır; PR60’ın sınırsız yanıtı, tahmini URI/Host eşlemesi ve örtük yöntem genişlemesi uyumluluk sözü değildir.

<a id="from-the-original-frickle-module"></a>

### Özgün FRiCKLE modülünden

Yöntem/erişim listesi ve bölge/anahtar biçimleri kullanılabilir. Eksik kayıtlar `200 no_match` döndürür; `cache_purge_legacy_status` yönergesini kaldırıp istemcileri güncelleyin. Arka plan işlemeyi açmak için ekleyin:

```nginx
http {
    cache_purge_background_queue on;
}
```

<a id="from-nginx-selective-cache-purge-module"></a>

### nginx-selective-cache-purge-module modülünden

Modülü değiştirin; kuyruk artık yerleşiktir. Eski parametreleri `cache_purge_queue_size`, `cache_purge_batch_size`, `cache_purge_throttle_ms` ile eşleyin.

---

<a id="temporary-directories"></a>

## Geçici dizinler

Tarama kök altındaki yapılandırılmış geçici dizinleri tam yol bileşenleriyle karşılaştırıp atlar. `proxy_temp` gibi adlar geçici yol olarak tanımlı değilse ayrılmış değildir. Yalnız adı tam 32 hex karakter olan normal dosyalar adaydır. Sayısal/son ekli geçici yazmalar `use_temp_path=off` dahil atlanır.

Her tarama yerel başlık sürümü, ofsetler, tam `KEY: ` işareti, CRC32 ve anahtar/çeşit dosya adı hash’ini, 8192 bayt sabit anahtar sınırıyla doğrular. Bozuk, kesik, başka sürüm veya uyuşmayan dosya tam temizlemede bile atlanır. Çekirdek üst sınırına eşit ETag/Vary kabul edilir. Biçim doğrulaması kötü niyetli dosyanın gerçekliğini doğrulamaz.

Kök symlink veya Windows junction olabilir; alt symlink/reparse point’ler atlanır. Tam descriptor-relative özellik denetimli POSIX’te çocuklar çekirdek `ngx_openat_file`/`ngx_file_at_info` ve no-follow kullanır; silme açık tutulan ebeveyne göredir. Değiştirilen alt symlink yönlendirmesini engeller, açılmış dizinin taşınmasına karşı sandbox değildir. Windows/eski Unix tam yollar ve link denetimleri kullanır; yol değiştirme yarışı kalır. Eski tam anahtar arama/silme de tam yolludur. Kökleri özel tutun, güvenilmeyen yazıcıya izin vermeyin.

Tutarlı kanonik yol yazımı kullanın; karşılaştırmalar symlink, bağlama veya Unicode takma adlarını çözmez. Kuyruktaki görev korumasını tutar; yeni tekrar bekleyeni veya sonraki geçişi günceller. Çalışan geçiş eski kümeyi korur; tüm canlı worker nesillerinin birleşimi değildir. İlgili görevler beklerken/çalışırken yolları başka amaçla kullanmayın.

---

<a id="security"></a>

## Güvenlik

- Purge uç noktalarını `allow` / `deny` veya öndeki kimlik doğrulamayla sınırlayın. Herkese açmayın.
- İstek selinde ortak belleği sınırlamak için trafiğe uygun `cache_purge_queue_size` belirleyin.
- Normal trafikten ayrı purge hız sınırlaması için `limit_req` kullanmayı değerlendirin.

---

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

---

<a id="see-also"></a>

## Ayrıca bakınız

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [nginx belgeleri](https://nginx.org/en/docs/)
