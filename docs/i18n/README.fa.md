<div dir="rtl">

<a id="contents"></a>

## فهرست

- [قابلیت‌ها](#features)
- [سازگاری](#compatibility)
- [بسته‌های توزیع‌ها](#distribution-packages)
- [نصب](#installation)
- [نحوهٔ استفاده](#usage)
  - [پاک‌سازی با بخشی از کلید](#partial-key-purge)
  - [ابطال مبتنی بر نسل](#generation-invalidation)
  - **REFRESH** — [بررسی محتوای کش در مبدأ (REFRESH)](#conditional-refresh)
- [نمونه‌های پیکربندی](#localized-examples)
- [امنیت](#security)
- [مهاجرت](#migration)
- [مستندات](#documentation)
- [پیوندهای مرتبط](#see-also)
- [مجوز](#license)

<a id="features"></a>

## قابلیت‌ها

- **پاک‌سازی درون همان location** — روش اختصاصی HTTP با نام `PURGE`، همراه با کنترل دسترسی بر اساس IP
- **location جداگانه برای پاک‌سازی** — نحو `proxy_cache_purge zone key` با آرگومان‌های ناحیه و کلید، برای پاک‌سازی کلیدهای استخراج‌شده با عبارت منظم، بدون نیاز به `proxy_pass`
- **پاک‌سازی با wildcard یا بخشی از کلید** — وجود `*` در انتهای کلید باعث پیمایش دایرکتوری کش و حذف همهٔ ورودی‌های منطبق می‌شود
- **`purge_all`** — حذف همهٔ ورودی‌های ناحیهٔ کش با یک درخواست
- **صف پس‌زمینه** — پیمایش به‌تعویق‌افتاده در گام‌های محدودِ حلقهٔ رویداد، یا با استفادهٔ اختیاری از مخزن نخ NGINX؛ اندازهٔ دسته‌های کار و مکث‌ها قابل تنظیم است
- **پاک‌سازی با درنظرگرفتن Vary** — پس از پاک‌سازی یک کلید دقیق، همهٔ گونه‌های فایل‌سیستمی آن با همان کلید کش، از جمله گونه‌های gzip و سرآیند Vary، حذف می‌شوند
- **قالب پاسخ** — `json` (پیش‌فرض)، `html`، `xml`، `text` و `none`
- **مخزن‌های اجرای نام‌دار** — انتخاب مخزن با ارث‌بری تنظیمات و بازگشت به اجرای گام‌به‌گام در ساخت‌های بدون پشتیبانی نخ و در Windows
- **ادغام کارهای فعالِ تکراری** — برای پاک‌سازی‌های تکراری که هنگام پیمایش دریافت می‌شوند، یک عملیات نمایه‌شده و حداکثر یک اجرای مجددِ در انتظار نگه داشته می‌شود
- **وضعیت** — سنجه‌های صف، حذف و HIT/MISS برای هر کش؛ خواندن آن‌ها نیازی به گرفتن قفل متقابل صف مشترک ندارد
- **کلیدهای نسل** — ابطال دقیق، پیشوندی و فضای نام، بدون ورودی/خروجی دیسک در مسیر پردازش درخواست؛ داده‌های قدیمی که دیرتر نوشته می‌شوند همچنان دسترس‌ناپذیر می‌مانند
- **[بررسی محتوای کش در مبدأ (REFRESH)](#conditional-refresh)**

<a id="compatibility"></a>

## سازگاری

در محیط عملیاتی از نسخهٔ NGINX که همچنان نگهداری می‌شود استفاده کنید. بستر اصلی POSIX/UNIX است؛ پشتیبانی Windows آزمایشی است. استفاده از رشته‌ها اختیاری است. [NGINX / CI](../../t/TESTING.md#compatibility-matrix).

<a id="distribution-packages"></a>

## بسته‌های توزیع‌ها

برخی توزیع‌ها و پروژه‌های پورت این فورک را بسته‌بندی می‌کنند، اما بسته‌ها لزوماً نسخهٔ v4 نیستند. پیش از نصب، نسخهٔ بسته و سازگاری ABI با NGINX را بررسی کنید. جزئیات در [بسته‌ها و منابع آن‌ها](../DISTRIBUTIONS.md) آمده است.

<a id="installation"></a>

## نصب

<div dir="ltr">

```bash
cd /path/to/nginx-source
./configure --add-module=/path/to/ngx_cache_purge
make
make install
```

</div>

ماژول پویا:

<div dir="ltr">

```bash
./configure --with-compat --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

از کد منبع همان نسخهٔ NGINX نصب‌شده روی سرور استفاده کنید. گزینهٔ `--with-compat` سازگاری میان هر نسخه‌ای از NGINX یا کتابخانه‌های C را تضمین نمی‌کند. برای جزئیات، [ماژول‌های پویا و بسته‌های انتشار](../BINARY_MODULES.md) را ببینید (به انگلیسی).

</div>

<a id="usage"></a>

## نحوهٔ استفاده

[→ نمونه‌های پیکربندی](#localized-examples)

<a id="partial-key-purge"></a>

### پاک‌سازی با بخشی از کلید

به **کلید ارزیابی‌شدهٔ کش** یک `*` بیفزایید تا پیشوند بدون حساسیت به بزرگی حروف تطبیق داده شود. کلید دقیق و Vary همچنان به بزرگی حروف حساس‌اند. پیشوند پاک‌سازی فیزیکی بدون `*` پایانی حداکثر 511 بایت است. پیشوند بلندتر یک بار در گزارش ثبت می‌شود و بدون پیمایش فایل‌ها `400 invalid_selector` برمی‌گرداند.

با `cache_purge_key`، تطبیق از عبارت منطقی استفاده می‌کند و حداکثر 8096 بایت، شامل `*` انتهایی، می‌پذیرد.

برای عبارت کلیدی که به `$uri` ختم می‌شود، این درخواست یک پیشوند URI را انتخاب می‌کند:

<div dir="ltr">

```http
PURGE /images/header*
```

</div>

ستاره باید آخرین بایت کلیدِ ارزیابی‌شده باشد، نه صرفاً آخرین بایت مسیر URI. پسوندهای رشتهٔ پرس‌وجو یا متغیرها در `proxy_cache_key` را در نظر بگیرید. کلیدی که فقط `*` باشد، همهٔ ورودی‌های واجد شرایط را پیمایش می‌کند.

<a id="generation-invalidation"></a>

### ابطال مبتنی بر نسل

نسل‌های کلید مانع استفادهٔ درخواست‌های جدید از محتوای قدیمی پس از PURGE می‌شوند، حتی اگر پاسخ قدیمی دیرتر نوشته شود. فایل‌ها جداگانه حذف می‌شوند. فضای نام قواعد ابطال را مشترک می‌کند؛ دایرکتوری وضعیت آن را بین راه‌اندازی‌های مجدد نگه دارید.

<a id="conditional-refresh"></a>

### بررسی محتوای کش در مبدأ (REFRESH)

REFRESH با ETag و Last-Modified ذخیره‌شده، HEAD را برای همان بازنمایی در مبدأ می‌فرستد. اگر صفحهٔ محصول قطعاً قدیمی است، از PURGE استفاده کنید. REFRESH بدنهٔ تازه نمی‌گیرد، کش خالی را پر نمی‌کند و TTL را افزایش نمی‌دهد. [HTTP / REFRESH](../REFRESH.md).

| نتیجهٔ خام مبدأ | اقدام / نتیجه |
|---|---|
| `304` | حفظ؛ `kept` |
| `200`, `404`, `410` | ابطال شرطی نسل و پاک‌سازی به‌تعویق‌افتاده؛ `invalidated` |
| وضعیت دیگر | حفظ؛ `upstream_status` |
| نبودن وضعیت مبدأ / شکست انتقال | حفظ؛ `transport_error` |
| فرادادهٔ Vary | بدون HEAD؛ `skipped_vary` |
| نگاشت یا پروفایل نامعتبر | حفظ؛ `skipped_profile` |
| جایگزین‌شدن نسل در این فاصله | بدون ابطال جدید؛ `raced_replaced` |
| شکست داخلی ابطال یا تخصیص حافظه | حفظ؛ `invalidation_error` |
| کاری که پیش از توقف صفحه ارسال یا تکمیل نشده است | `undispatched`؛ مکان‌نما از آن عبور نمی‌کند |

<div dir="rtl">

<a id="localized-examples"></a>

## نمونه‌های پیکربندی

نمونه‌های کامل، کاتالوگ محصولات با REFRESH و پیکربندی‌های معمول PURGE را در بر می‌گیرند. [نمونه‌های پیکربندی انگلیسی](../EXAMPLES.md).


<a id="choosing-a-mode"></a>

### انتخاب رفتار مناسب

| هدف | پیکربندی | معنای پاسخ |
|---|---|---|
| حذف ورودی‌های منقضی و انتظار برای تلاش حذف | PURGE با صف پس‌زمینهٔ خاموش | `200 purged` یا `200 no_match`؛ در صورت خطا 4xx/5xx |
| زمان‌بندی پاک‌سازی بر اساس پیشوند یا کل کش | PURGE همراه `cache_purge_background_queue on` | `202 accepted`؛ تا زمان پاک‌سازی ممکن است ورودی قدیمی همچنان ارائه شود |
| جلوگیری از دسترسی درخواست جدید به محتوای قدیمی، حتی پس از نوشتن دیرهنگام | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`؛ نسل جدید کلید استفاده می‌شود و فایل‌ها بعداً حذف می‌شوند |
| بررسی تغییر محتوا در مبدأ | نسل‌های کلید + `proxy_cache_refresh` + پروفایل داخلی | `200` همراه نتیجهٔ هر ورودی و اطلاعات ادامه؛ کش را از پیش پر نمی‌کند |

این سازوکارها قابل ترکیب‌اند: تغییر نسل نیز پاک‌سازی را زمان‌بندی می‌کند. صف به‌تنهایی جلوی اثر نوشتن دیرهنگام را نمی‌گیرد. [نمونهٔ v4](#localized-examples)، [نمونه‌های PURGE](#localized-examples) و [پاسخ‌های HTTP](../HTTP_RESPONSES.md) (انگلیسی) را ببینید.

<details>
<summary>نمونه‌های پیکربندی — ۷ نمونه</summary>

<a id="generation-and-refresh-example"></a>

### نمونه: کاتالوگ محصولات

این نمونهٔ کامل روی پورت محلی 8080 گوش می‌دهد و درخواست‌ها را به برنامهٔ پورت 8081 می‌فرستد. برنامه باید `/products/42` را با Host برابر `shop.example.com` ارائه کند. آدرس، دامنه و دایرکتوری‌های قابل‌نوشتن کش و وضعیت را تغییر دهید.

`shop.example.com` دامنهٔ سربرگ HTTP Host است، نه دایرکتوری. `/products/42` نشانی URI محصول است. `catalog:` پیشوند ثابت کلید است، نه نام میزبان. پیشوند همراه URI کامل، شامل رشتهٔ پرس‌وجو، کلید منطقی مانند `catalog:/products/42` را می‌سازد. پروفایل REFRESH دقیقاً `catalog:` را حذف و `/products/42` را در همان برنامه بررسی می‌کند.

<div dir="ltr">

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

</div>

محصول موجود و قابل کش را با GET بخوانید تا `X-Cache-Status: HIT` دیده شود، سپس REFRESH بفرستید:

<div dir="ltr">

```sh
curl -i 'http://127.0.0.1:8080/products/42'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/42' \
  'http://127.0.0.1:8080/admin/refresh'
```

</div>

اگر محصول تغییر کرده، PURGE بفرستید. در اینجا `200 invalidated` مانع استفادهٔ درخواست‌های جدید از نسل قدیمی می‌شود؛ GET بعدی به برنامه می‌رود.

<div dir="ltr">

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/products/42'
```

</div>

برای همهٔ محصولات از `catalog:/products/*` استفاده کنید. اگر `complete: false` بود، انتخابگر را نگه دارید و `<next_cursor>` را با مقدار برگشتی جایگزین کنید. پیش از ادامه خطاهای HTTP را رفع کنید.

<div dir="ltr">

```sh
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

</div>

<a id="sample-configurations"></a>

### نمونه‌های پیکربندی

پنج بلوک نخست نمونه‌های کامل و مستقل `nginx.conf` هستند. بلوک آخر، پردازش Vary را به بخش `http` موجود اضافه می‌کند. NGINX اتصال‌های محلی را روی پورت 8080 می‌پذیرد؛ `catalog_backend` نام گروه upstream برای برنامه در نشانی `127.0.0.1:8081` است. `shop.example.com` مقدار HTTP Host ارسالی به برنامه است. نشانی‌ها را متناسب با محیط خود تغییر دهید و پوشه‌ای برای کش انتخاب کنید که NGINX اجازهٔ نوشتن در آن را داشته باشد.

<a id="basic--inline-purge-method"></a>

#### پایه — روش پاک‌سازی در همان location

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
# 200 — ورودی پاک شد
# 200 + Cache-Purge-Code: no_match — ورودی منطبق در کش یافت نشد
# 403 — نشانی IP کلاینت مجاز نیست
```

</div>

<a id="basic--separate-purge-location"></a>

#### پایه — location جداگانهٔ پاک‌سازی

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

        # کلید کش را از URI گرفته و به صورت منطقه/کلید ارسال کنید.
        # در این location نیازی به proxy_pass یا proxy_cache نیست.
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

#### فعال‌کردن صف پس‌زمینه

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

#### پاک‌سازی همهٔ ورودی‌ها

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
# 200 — پیمایش کامل شد؛ خطاهای فایل را در error_log بررسی کنید
```

</div>

<a id="json-responses-with-per-location-override"></a>

#### پاسخ‌های JSON با بازنویسی تنظیم برای هر location

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

        # پیش‌فرض همهٔ locationهای این سرور.
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
            # تغییر به متن ساده برای اسکریپت‌ها
            cache_purge_response_type text;
            proxy_cache_purge         main "$arg_key";
        }
    }
}
```

</div>

<a id="vary-aware-purge"></a>

#### پاک‌سازی با درنظرگرفتن Vary

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

</details>

<a id="security"></a>

## امنیت

- نقاط پایانی پاک‌سازی را با دستورهای `allow` / `deny` یا لایهٔ احراز هویت پیشین محدود کنید. هرگز آن‌ها را عمومی نکنید.
- `cache_purge_queue_size` را متناسب با ترافیک خود تنظیم کنید تا مصرف حافظهٔ مشترک هنگام سیل درخواست‌ها محدود شود.
- استفاده از `limit_req` در locationهای پاک‌سازی را در نظر بگیرید تا نرخ درخواست‌های پاک‌سازی مستقل از ترافیک عادی محدود شود.

<a id="migration"></a>

## مهاجرت

تغییر ساختار SHM نیازمند راه‌اندازی مجدد کامل است. نحو روش/ACL و رسیدگی‌کننده‌های پروتکل حفظ می‌شوند. v4 به‌طور پیش‌فرض JSON، کد نتیجهٔ یکسان در سرآیند و بدنه، و `200 no_match` برای نبود ورودی به کار می‌برد. `cache_purge_legacy_status` فقط هشدار می‌دهد. کارخواه‌ها را مطابق [قرارداد HTTP](../HTTP_RESPONSES.md) به‌روز کنید. ظرفیت شامل کارهای واگذارشده است؛ درخواست در ازدحام منتظر پذیرش می‌ماند. موارد تکراریِ فعال می‌توانند پیمایش دوباره را زمان‌بندی کنند.

<a id="documentation"></a>

## مستندات

- [مرجع دستورها](DIRECTIVES.fa.md)
- [نمونه‌های پیکربندی بیشتر](#localized-examples)
- [بهره‌برداری و رفع اشکال](OPERATIONS.fa.md)
- [HTTP و محدودیت‌های REFRESH (انگلیسی)](../REFRESH.md)
- [مهاجرت به v4](MIGRATION.fa.md)
- [معماری پیاده‌سازی (انگلیسی)](../ARCHITECTURE.md)
- [آزمون‌ها](OPERATIONS.fa.md#testing)
- [توسعه و آزمون‌ها (انگلیسی)](../../t/TESTING.md)

<a id="see-also"></a>

## پیوندهای مرتبط

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [مستندات nginx](https://nginx.org/en/docs/)

<a id="license"></a>

## مجوز

<div dir="ltr">

```
Copyright (c) 2009-2014, FRiCKLE <info@frickle.com>
Copyright (c) 2009-2014, Piotr Sikora <piotr.sikora@frickle.com>
Copyright (C) 2016-2026, Denys Denysov

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

</div>

</div>
