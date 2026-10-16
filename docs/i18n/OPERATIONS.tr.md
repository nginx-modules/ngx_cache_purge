# ngx_cache_purge

[README](README.tr.md) · [Yönergeler](DIRECTIVES.tr.md)

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

[Durum uç noktası](DIRECTIVES.tr.md#cache_purge_queue_status) bekleyen/etkin görev, kabul bekleyen, tekrar, silme ve HIT/MISS gösterir. `202 Accepted` kabul/birleşmedir; silme veya anında mantıksal geçersizleştirme kanıtı değildir. Gecikmeyi kuyruk yaşı ve depolama çekişmesiyle birlikte izleyin.

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

<a id="temporary-directories"></a>

## Geçici dizinler

Tarama kök altındaki yapılandırılmış geçici dizinleri tam yol bileşenleriyle karşılaştırıp atlar. `proxy_temp` gibi adlar geçici yol olarak tanımlı değilse ayrılmış değildir. Yalnız adı tam 32 hex karakter olan normal dosyalar adaydır. Sayısal/son ekli geçici yazmalar `use_temp_path=off` dahil atlanır.

Her tarama yerel başlık sürümü, ofsetler, tam `KEY: ` işareti, CRC32 ve anahtar/çeşit dosya adı hash’ini, 8192 bayt sabit anahtar sınırıyla doğrular. Bozuk, kesik, başka sürüm veya uyuşmayan dosya tam temizlemede bile atlanır. Çekirdek üst sınırına eşit ETag/Vary kabul edilir. Biçim doğrulaması kötü niyetli dosyanın gerçekliğini doğrulamaz.

Kök symlink veya Windows junction olabilir; alt symlink/reparse point’ler atlanır. Tam descriptor-relative özellik denetimli POSIX’te çocuklar çekirdek `ngx_openat_file`/`ngx_file_at_info` ve no-follow kullanır; silme açık tutulan ebeveyne göredir. Değiştirilen alt symlink yönlendirmesini engeller, açılmış dizinin taşınmasına karşı sandbox değildir. Windows/eski Unix tam yollar ve link denetimleri kullanır; yol değiştirme yarışı kalır. Eski tam anahtar arama/silme de tam yolludur. Kökleri özel tutun, güvenilmeyen yazıcıya izin vermeyin.

Tutarlı kanonik yol yazımı kullanın; karşılaştırmalar symlink, bağlama veya Unicode takma adlarını çözmez. Kuyruktaki görev korumasını tutar; yeni tekrar bekleyeni veya sonraki geçişi günceller. Çalışan geçiş eski kümeyi korur; tüm canlı worker nesillerinin birleşimi değildir. İlgili görevler beklerken/çalışırken yolları başka amaçla kullanmayın.

---

<a id="testing"></a>

## Testler

[Test kapsamı](../TEST_COVERAGE.md) (İngilizce), test edilen davranışları ve kalan eksikleri gösterir.

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

Sıkı derleme, sanitizer, Valgrind, platform işleri ve raporlar için [`t/TESTING.md`](../../t/TESTING.md). Tüm GitHub işleri `build-test-release.yml` içindedir.
