<a id="ngx_cache_purge"></a>

# ngx_cache_purge

Модуль NGINX для видалення записів із кешів `FastCGI`, `proxy`, `SCGI` та `uWSGI`. У v4 покоління ключів дають змогу припинити видавання старих записів новим запитам ще до видалення файлів. REFRESH перевіряє на сервері-джерелі, чи змінився вміст проксі-кешу. Обидві можливості потрібно налаштувати окремо. Синтаксис PURGE збережено, проте HTTP-відповіді змінилися: див. [перехід на v4](README.uk.md#migration).

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

<a id="features"></a>

## Можливості

- **Очищення в основній секції location** — окремий HTTP-метод (`PURGE`) з контролем доступу за IP
- **Окрема секція location для очищення** — синтаксис зони й ключа `proxy_cache_purge zone key` для ключів, захоплених регулярним виразом, без `proxy_pass`
- **Маска / часткове очищення** — кінцева `*` запускає обхід каталогу кешу та видалення відповідних записів
- **`purge_all`** — видалення всіх записів зони кешу одним запитом
- **Фонова черга** — відкладений обхід обмеженими кроками циклу подій або через необов’язковий пул потоків NGINX; налаштовувані пакети завдань і паузи
- **Урахування Vary** — після очищення точного ключа видаляються всі файлові варіанти з тим самим ключем, зокрема gzip і Vary
- **Формати відповіді** — `json` (типово), `html`, `xml`, `text`, `none`
- **Іменовані пули** — успадковуваний вибір пулу з покроковим виконанням у збірках без потоків і на Windows
- **Дедуплікація активних завдань** — одна індексована операція та щонайбільше один запланований повторний прохід для запитів, що надходять під час обходу
- **Стан** — показники черги, видалення й HIT/MISS для кожного кешу без захоплення спільного м’ютекса черги під час читання
- **Ключі поколінь** — інвалідація точного ключа, префікса чи простору імен без дискового введення-виведення в обробнику запиту; пізні записи старого покоління залишаються недоступними
- [Перевірка кешу в джерелі (REFRESH)](#conditional-refresh)

<a id="choosing-a-mode"></a>

## Як вибрати режим

| Завдання | Налаштування | Що означає відповідь |
|---|---|---|
| Видалити явно застарілі записи й дочекатися спроби видалення | PURGE з вимкненою фоновою чергою | `200 purged` або `200 no_match`; у разі помилки — 4xx/5xx |
| Запланувати очищення великої групи ключів за префіксом або всього кешу | PURGE з `cache_purge_background_queue on` | `202 accepted`; до очищення файли ще можуть бути доступними |
| Не видавати старий вміст новим запитам, зокрема після запізнілого записування файла | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`; нові запити використовують нове покоління ключа, файли видаляються згодом |
| Перевірити, чи змінився вміст проксі-кешу в джерелі | Покоління ключів + `proxy_cache_refresh` + внутрішній профіль | `200` із результатом для кожного запису та продовженням; порожній кеш не заповнюється |

Ці налаштування поєднуються: зміна покоління також ставить очищення в чергу. Саме лише ввімкнення фонової черги не захищає від запізнілого записування файлів. Почніть із [прикладу v4](README.uk.md#generation-and-refresh-example) або [звичайного PURGE](#sample-configurations). Перш ніж писати клієнт, перегляньте [правила HTTP-відповідей](../HTTP_RESPONSES.md) (англійською).

<a id="compatibility"></a>

## Сумісність

Для робочого середовища вибирайте підтримуваний випуск NGINX. Основна платформа — POSIX/UNIX; підтримка Windows експериментальна. Підтримка потоків необов’язкова: без `--with-threads` фоновий обхід виконується невеликими кроками в циклі подій. Див. [перевірювані версії та платформи](../../t/TESTING.md#compatibility-matrix).

<a id="installation"></a>

## Встановлення

```bash
cd /path/to/nginx-source
./configure --add-module=/path/to/ngx_cache_purge
make
make install
```

Динамічний модуль:

```bash
./configure --with-compat --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

Використовуйте вихідний код тієї самої версії NGINX, що й на сервері. `--with-compat` не гарантує сумісності між довільними версіями NGINX чи бібліотеками C. Докладніше: [динамічні модулі та релізні архіви](../BINARY_MODULES.md) (англійською).

<a id="generation-and-refresh-example"></a>

## Приклад: каталог товарів

У цьому прикладі NGINX приймає запити на локальному порту 8080 й передає їх застосунку на порту 8081. Застосунок має обслуговувати `/products/42` із Host `shop.example.com`. Замініть адресу застосунку, домен і доступні для запису каталоги кешу та стану.

`shop.example.com` — домен у заголовку HTTP Host, а не каталог. `/products/42` — URI картки товару. `catalog:` — довільний сталий префікс ключа кешу, а не ім’я сервера. Логічний ключ складається з префікса й повного URI запиту, включно з рядком параметрів: `catalog:/products/42`. Профіль REFRESH вилучає саме `catalog:` і перевіряє `/products/42` у тому самому застосунку.

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

Запитайте наявний товар, відповідь для якого можна кешувати. Повторюйте GET до появи `X-Cache-Status: HIT`, а потім перевірте його через REFRESH:

```sh
curl -i 'http://127.0.0.1:8080/products/42'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/42' \
  'http://127.0.0.1:8080/admin/refresh'
```

Якщо товар змінився й запис потрібно одразу вилучити з доступного кешу, надішліть PURGE. Тут `200 invalidated` означає, що нові запити не отримають старого покоління; наступний GET звернеться до застосунку.

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/products/42'
```

Щоб перевірити всі кешовані картки, виберіть `catalog:/products/*`. Для продовження замініть `<next_cursor>` отриманим значенням, не змінюючи селектора. У разі HTTP-помилки спершу усуньте її причину.

```sh
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

<a id="partial-key-purge"></a>

## Очищення за частиною ключа

Додайте `*` до **розгорнутого ключа кешу**, щоб вибрати префікс без урахування регістру. Точні ключі й Vary залишаються чутливими до регістру. Префікс фізичного очищення може містити до 511 байтів без кінцевої `*`. Довший префікс спричиняє один запис у журналі та відповідь `400 invalid_selector` без обходу файлів.

За `cache_purge_key` зіставляється логічний вираз, а ліміт становить 8096 байтів разом із кінцевою `*`.

Якщо вираз ключа закінчується на `$uri`, цей запит вибирає префікс URI:

```http
PURGE /images/header*
```

Зірочка має бути останнім байтом обчисленого ключа, а не лише шляху URI. Враховуйте рядок запиту чи змінні наприкінці `proxy_cache_key`. Ключ із самої `*` охоплює всі придатні записи.

<a id="generation-invalidation"></a>

## Як припинити видавання старих записів: покоління ключів

Використовуйте покоління ключів, якщо після завершення PURGE нові запити вже не повинні отримувати застарілу відповідь. Запит до джерела, розпочатий раніше, може завершитися пізніше, але його запис зі старим поколінням не стане HIT для нових запитів. Файли видаляються окремо.

Простір імен об’єднує ключі зі спільними правилами інвалідації. Зберігайте каталог його стану разом із кешем між перезапусками. Спільний простір імен поширює інвалідацію на кілька шляхів кешу; фізичне очищення стосується лише вказаного шляху. Див. [експлуатацію](OPERATIONS.uk.md) (англійською).

<a id="conditional-refresh"></a>

## Перевірка кешу в джерелі (REFRESH)

REFRESH потрібен, коли слід перевірити, чи змінився вміст у джерелі. Він надсилає HEAD для того самого подання зі збереженими ETag та Last-Modified. Якщо картка товару точно застаріла, використовуйте PURGE.

REFRESH не завантажує нове тіло відповіді, не заповнює порожній кеш і не подовжує TTL. Перевіряйте результат для кожного запису. За `complete: false` повторіть той самий селектор, передавши `next_cursor` у `X-Cache-Purge-Cursor`. Див. [контракт та обмеження](../REFRESH.md) (англійською).

| Відповідь джерела або стан запису | Дія модуля |
|---|---|
| `304` | Зберігає запис: `kept`. Строк його дії не подовжується. |
| `200`, `404`, `410` | Скидає поточне покоління: `invalidated`. Наступний звичайний GET завантажить об’єкт знову, якщо відповідь джерела можна кешувати. |
| Інший статус | Зберігає запис: `upstream_status`. Код джерела міститься в полі `upstream_status`. |
| Відповідь джерела не отримано | Зберігає запис: `transport_error`. |
| Запис містить Vary | Не надсилає HEAD: `skipped_vary`; початкові заголовки запиту невідомі. |
| Некоректний профіль або перетворення ключа на URI | Зберігає запис: `skipped_profile`. Помилки налаштування профілю записуються в журнал; також перевірте правило перетворення URI. |
| Інший PURGE уже змінив покоління | Не зачіпає нову версію: `raced_replaced`. |
| Помилка зміни покоління або виділення пам’яті | Зберігає запис: `invalidation_error`. |
| Вибрану роботу не завершено | Враховує її в `undispatched`; продовження не пропускає цей запис. |

<a id="sample-configurations"></a>

## Приклади конфігурації

Перші п’ять блоків — окремі повні приклади `nginx.conf`. Останній блок додає оброблення Vary до наявної секції `http`. NGINX приймає локальні запити на порту 8080; `catalog_backend` — назва групи із застосунком за адресою `127.0.0.1:8081`. `shop.example.com` — його домен у заголовку HTTP Host. Замініть адреси й каталог кешу на власні; каталог має бути доступним для запису.

<a id="basic--inline-purge-method"></a>

### Базова — метод очищення в основній секції location

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

### Базова — окрема секція location для очищення

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

### Увімкнена фонова черга

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

### Очищення всіх записів

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

### Відповіді JSON із перевизначенням у location

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

### Очищення з урахуванням Vary

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

Після очищення ключа автоматично видаляються всі його варіанти gzip і Vary, збережені за іншими файловими шляхами.

<a id="migration"></a>

## Міграція

У v4 типовий формат — JSON, а відсутність запису повертає `200 no_match`. `cache_purge_legacy_status` лише попереджає й не змінює відповіді. Оновіть клієнтів; якщо змінилася структура спільної пам’яті, повністю перезапустіть NGINX. Покоління ключів вмикаються явно та змінюють імена файлів кешу. Див. [перехід на v4](MIGRATION.uk.md) (англійською).

<a id="security"></a>

## Безпека

- Обмежуйте доступ до кінцевих точок очищення директивами `allow` / `deny` або попереднім рівнем автентифікації. Не відкривайте їх для загального доступу.
- Добирайте `cache_purge_queue_size` відповідно до трафіку, щоб обмежити споживання спільної пам’яті за надмірного потоку запитів.
- Розгляньте `limit_req` у секціях очищення, щоб обмежувати частоту PURGE незалежно від звичайного трафіку.

<a id="documentation"></a>

## Документація

- [Довідник директив](DIRECTIVES.uk.md)
- [Інші приклади конфігурації](README.uk.md#sample-configurations)
- [Експлуатація й усунення несправностей](OPERATIONS.uk.md)
- [HTTP-відповіді та обмеження REFRESH](../REFRESH.md)
- [Перехід на v4](MIGRATION.uk.md)
- [Архітектура реалізації](../ARCHITECTURE.md)
- [Тестування](OPERATIONS.uk.md#testing)
- [Розробка й тести](../../t/TESTING.md)

<a id="see-also"></a>

## Дивіться також

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [Документація nginx](https://nginx.org/en/docs/)

<a id="license"></a>

## Ліцензія

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
