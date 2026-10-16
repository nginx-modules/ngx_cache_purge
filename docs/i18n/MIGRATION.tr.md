# ngx_cache_purge

[README](README.tr.md) · [Yönergeler](DIRECTIVES.tr.md)

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
