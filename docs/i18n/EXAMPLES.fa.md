[ngx_cache_purge](README.fa.md)

<div dir="rtl">

<a id="sample-configurations"></a>

# نمونه‌های پیکربندی

پنج بلوک نخست نمونه‌های کامل و مستقل `nginx.conf` هستند. بلوک آخر، پردازش Vary را به بخش `http` موجود اضافه می‌کند. NGINX اتصال‌های محلی را روی پورت 8080 می‌پذیرد؛ `catalog_backend` نام گروه upstream برای برنامه در نشانی `127.0.0.1:8081` است. `shop.example.com` مقدار HTTP Host ارسالی به برنامه است. نشانی‌ها را متناسب با محیط خود تغییر دهید و پوشه‌ای برای کش انتخاب کنید که NGINX اجازهٔ نوشتن در آن را داشته باشد.

<a id="basic--inline-purge-method"></a>

### پایه — روش پاک‌سازی در همان location

<div dir="ltr">

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

</div>

<div dir="ltr">

```bash
curl -i -X PURGE -H 'Host: shop.example.com' \
  'http://127.0.0.1:8080/images/logo.png'
# 200 — entry purged
# 200 + Cache-Purge-Code: no_match
# 403 — client IP not in the allowed list
```

</div>

<a id="basic--separate-purge-location"></a>

### پایه — location جداگانهٔ پاک‌سازی

<div dir="ltr">

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

</div>

<a id="background-queue-enabled"></a>

### فعال‌کردن صف پس‌زمینه

<div dir="ltr">

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

</div>

<a id="purge-all-entries"></a>

### پاک‌سازی همهٔ ورودی‌ها

<div dir="ltr">

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

</div>

<div dir="ltr">

```bash
curl -i -X PURGE -H 'Host: shop.example.com' \
  'http://127.0.0.1:8080/content/anything'
# 200 — walk completed; inspect error_log for file errors
```

</div>

<a id="json-responses-with-per-location-override"></a>

### پاسخ‌های JSON با بازنویسی تنظیم برای هر location

<div dir="ltr">

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

</div>

<a id="vary-aware-purge"></a>

### پاک‌سازی با درنظرگرفتن Vary

<div dir="ltr">

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

</div>

پس از پاک‌سازی یک کلید، همهٔ گونه‌های gzip و Vary ذخیره‌شده در مسیرهای فایل‌سیستمی متفاوت نیز به‌طور خودکار حذف می‌شوند.

</div>
