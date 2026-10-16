

<a id="contents"></a>

## Содержание

- [Возможности](#features)
- [Совместимость](#compatibility)
- [Пакеты дистрибутивов](#distribution-packages)
- [Установка](#installation)
- [Использование](#usage)
  - [Очистка по части ключа](#partial-key-purge)
  - [Как перестать выдавать старые записи: поколения ключей](#generation-invalidation)
  - **REFRESH** — [Проверка кэша у источника (REFRESH)](#conditional-refresh)
- [Примеры конфигурации](#localized-examples)
- [Безопасность](#security)
- [Миграция](#migration)
- [Документация](#documentation)
- [См. также](#see-also)
- [Спонсоры](#sponsors)
- [Лицензия](#license)

<a id="features"></a>

## Возможности

- **Очистка в обслуживающем location** — отдельный HTTP-метод (`PURGE`) с контролем доступа по IP
- **Отдельный location для очистки** — синтаксис зоны и ключа `proxy_cache_purge zone key` для очистки по ключу из захватов регулярного выражения без `proxy_pass`
- **Маска / частичная очистка** — завершающий `*` запускает обход каталога кэша и удаление совпадающих записей
- **`purge_all`** — удаление всех записей зоны кэша одним запросом
- **Фоновая очередь** — отложенный обход ограниченными шагами цикла событий или через необязательный пул потоков NGINX; настраиваемые группы задач и паузы
- **Учёт Vary** — после очистки точного ключа удаляются все файловые варианты с тем же ключом, включая gzip и варианты заголовка Vary
- **Форматы ответа** — `json` (по умолчанию), `html`, `xml`, `text`, `none`
- **Именованные пулы** — наследуемый выбор пула с инкрементальным выполнением в сборках без потоков и на Windows
- **Дедупликация активных задач** — одна индексируемая операция и не более одного ожидающего повторного прохода для PURGE, поступивших во время обхода
- **Статус** — метрики очереди, удаления и HIT/MISS для каждого кэша без захвата общего mutex очереди при чтении
- **Ключи поколений** — инвалидация точного ключа, префикса или пространства имён без дискового I/O в пути обработки запроса; поздние старые записи остаются недоступными
- **[Проверка кэша у источника (REFRESH)](#conditional-refresh)**

<a id="compatibility"></a>

## Совместимость

Для рабочей среды выбирайте поддерживаемый выпуск NGINX. Основная платформа — POSIX/UNIX; Windows остаётся экспериментальной. Потоки необязательны: без `--with-threads` фоновый обход выполняется небольшими шагами в цикле событий. См. [проверяемые версии и платформы](../../t/TESTING.md#compatibility-matrix).

<a id="distribution-packages"></a>

## Пакеты дистрибутивов

Некоторые дистрибутивы и проекты портов распространяют этот форк, но не обязательно v4. Перед установкой проверьте версию пакета и совместимость с ABI NGINX. Подробности — в [списке пакетов и их источников](../DISTRIBUTIONS.md).

<a id="installation"></a>

## Установка

```bash
cd /path/to/nginx-source
./configure --add-module=/path/to/ngx_cache_purge
make
make install
```

Динамический модуль:

```bash
./configure --with-compat --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

Используйте исходники той же версии NGINX, что установлена на сервере. `--with-compat` не гарантирует совместимость произвольных версий NGINX и библиотек C. Подробнее: [динамические модули и релизные архивы](../BINARY_MODULES.md) (на английском).

<a id="usage"></a>

## Использование

[→ Примеры конфигурации](#localized-examples)

<a id="partial-key-purge"></a>

### Очистка по части ключа

Добавьте `*` к **вычисленному ключу кеша** для поиска по префиксу без учёта регистра. Точные ключи и Vary остаются чувствительными к регистру. Префикс физической очистки ограничен 511 байтами без завершающей `*`. Более длинный префикс записывается в журнал один раз и возвращает `400 invalid_selector` без обхода файлов.

С `cache_purge_key` сравнивается логическое выражение; допускается до 8096 байт вместе с конечной `*`.

Если выражение оканчивается на `$uri`, следующий запрос выбирает префикс URI:

```http
PURGE /images/header*
```

Звёздочка должна завершать вычисленный ключ, а не только путь URI. Учитывайте query string и переменные в конце `proxy_cache_key`. Ключ из одной `*` обходит все подходящие записи.

<a id="generation-invalidation"></a>

### Как перестать выдавать старые записи: поколения ключей

Используйте поколения ключей, если после завершения PURGE новые запросы уже не должны получать устаревший ответ. Запрос к источнику, начавшийся раньше, может завершиться позже, но его запись со старым поколением не станет HIT для новых запросов. Файлы удаляются отдельно.

Пространство имён объединяет ключи с общими правилами инвалидации. Сохраняйте каталог его состояния вместе с кешем между перезапусками. Общее пространство имён распространяет инвалидацию на несколько путей кеша; физическая очистка относится только к указанному пути. См. [эксплуатацию](OPERATIONS.ru.md).

<a id="conditional-refresh"></a>

### Проверка кэша у источника (REFRESH)

REFRESH нужен, когда следует проверить, изменилось ли содержимое у источника. Он отправляет HEAD для того же варианта ответа с сохранёнными ETag и Last-Modified. Если карточка товара заведомо устарела, используйте PURGE.

REFRESH не загружает новое тело, не прогревает пустой кеш и не продлевает TTL. Проверяйте результат каждой записи. При `complete: false` повторите тот же селектор, передав `next_cursor` в `X-Cache-Purge-Cursor`. См. [контракт и ограничения](../REFRESH.md) (на английском).

| Ответ источника или состояние записи | Что делает модуль |
|---|---|
| `304` | Сохраняет запись: `kept`. Срок её действия не продлевается. |
| `200`, `404`, `410` | Сбрасывает текущее поколение: `invalidated`. Следующий обычный GET загрузит объект заново, если ответ источника можно кэшировать. |
| Другой статус | Сохраняет запись: `upstream_status`. Код источника находится в поле `upstream_status`. |
| Ответ источника не получен | Сохраняет запись: `transport_error`. |
| В записи есть Vary | Не отправляет HEAD: `skipped_vary`; исходные заголовки запроса неизвестны. |
| Некорректный профиль или преобразование ключа в URI | Сохраняет запись: `skipped_profile`. Причина записана в error log. |
| Другой PURGE уже сменил поколение | Не затрагивает новую версию: `raced_replaced`. |
| Ошибка смены поколения или выделения памяти | Сохраняет запись: `invalidation_error`. |
| Выбранная работа не завершена | Учитывает её в `undispatched`; продолжение не пропускает эту запись. |

<a id="localized-examples"></a>

## Примеры конфигурации

Полные примеры включают каталог товаров с REFRESH и стандартные конфигурации PURGE. [Английские примеры конфигурации](../EXAMPLES.md).


<a id="choosing-a-mode"></a>

### Как выбрать режим

| Задача | Настройка | Что означает ответ |
|---|---|---|
| Удалить заведомо устаревшие записи и дождаться попытки удаления | PURGE с выключенной фоновой очередью | `200 purged` или `200 no_match`; при ошибке — 4xx/5xx |
| Запланировать очистку большого префикса или всего кэша | PURGE с `cache_purge_background_queue on` | `202 accepted`; до очистки файлы могут оставаться доступными |
| Не выдавать старое содержимое новым запросам, включая позднюю запись файла | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`; новые запросы используют новое поколение ключа, файлы удаляются позже |
| Проверить, изменилось ли содержимое proxy-кэша у источника | Поколения ключей + `proxy_cache_refresh` + внутренний профиль | `200` с результатом по каждой записи и продолжением; пустой кэш не заполняется |

Эти настройки сочетаются: смена поколения тоже ставит очистку в очередь. Одного включения фоновой очереди недостаточно для защиты от поздних записей. Начните с [примера v4](#localized-examples) или [обычного PURGE](#localized-examples). Перед реализацией клиента изучите [контракт HTTP-ответов](../HTTP_RESPONSES.md) (на английском).

<details>
<summary>Примеры конфигурации — 7 примеров</summary>

<a id="generation-and-refresh-example"></a>

### Пример: каталог товаров

Полный пример слушает локальный порт 8080 и направляет запросы товаров приложению на порту 8081. Приложение должно обслуживать `/products/42` с Host `shop.example.com`. Замените адрес приложения, домен и доступные для записи каталоги кеша и состояния.

`shop.example.com` — домен в HTTP-заголовке Host, а не каталог. `/products/42` — URI карточки товара. `catalog:` — произвольный постоянный префикс ключа кеша, а не имя сервера. Логический ключ складывается из префикса и полного URI запроса, включая query string: `catalog:/products/42`. Профиль REFRESH удаляет ровно `catalog:` и проверяет `/products/42` в том же приложении.

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

Запросите существующий товар, ответ для которого разрешено кешировать. Повторяйте GET до `X-Cache-Status: HIT`, затем проверьте его через REFRESH:

```sh
curl -i 'http://127.0.0.1:8080/products/42'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/42' \
  'http://127.0.0.1:8080/admin/refresh'
```

Если товар изменился и запись нужно сразу исключить из кеша, отправьте PURGE. Здесь `200 invalidated` означает, что новые запросы не получат старое поколение; следующий GET обратится к приложению.

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/products/42'
```

Для проверки всех закешированных карточек выберите `catalog:/products/*`. При продолжении замените `<next_cursor>` полученным значением, сохранив селектор. При HTTP-ошибке сначала устраните её причину.

```sh
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

<a id="sample-configurations"></a>

### Примеры конфигурации

Первые пять блоков — отдельные полные примеры `nginx.conf`. Последний блок добавляет обработку Vary в существующую секцию `http`. NGINX принимает локальные запросы на порту 8080; `catalog_backend` — имя группы с приложением по адресу `127.0.0.1:8081`. `shop.example.com` — его домен в заголовке HTTP Host. Замените адреса и каталог кеша своими; каталог должен быть доступен для записи.

<a id="basic--inline-purge-method"></a>

#### Базовая — встроенный метод очистки

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
# 200 — запись удалена
# 200 + Cache-Purge-Code: no_match — подходящая запись в кеше не найдена
# 403 — IP-адрес клиента не разрешён
```

<a id="basic--separate-purge-location"></a>

#### Базовая — отдельный location очистки

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

        # Извлечь ключ кеша из URI и передать в форме зона/ключ.
        # В этом location не нужны proxy_pass и proxy_cache.
        location ~ ^/purge(/.*) {
            allow             127.0.0.1;
            deny              all;
            proxy_cache_purge main "$host$1$is_args$args";
        }
    }
}
```

<a id="background-queue-enabled"></a>

#### Включённая фоновая очередь

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

#### Очистка всех записей

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
# 200 — обход завершён; ошибки файлов см. в error_log
```

<a id="json-responses-with-per-location-override"></a>

#### Ответы JSON с переопределением в location

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        # Значение по умолчанию для всех location сервера.
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
            # Выбрать обычный текст для скриптов
            cache_purge_response_type text;
            proxy_cache_purge         main "$arg_key";
        }
    }
}
```

<a id="vary-aware-purge"></a>

#### Очистка с учётом Vary

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

После очистки ключа автоматически удаляются все его gzip- и Vary-варианты по другим файловым путям.

</details>

<a id="security"></a>

## Безопасность

- Ограничивайте endpoint очистки через `allow` / `deny` или внешний слой аутентификации. Не открывайте публично.
- Выбирайте `cache_purge_queue_size` по трафику, ограничивая SHM при потоке запросов.
- Рассмотрите `limit_req` для ограничения PURGE независимо от обычного трафика.

<a id="migration"></a>

## Миграция

В v4 JSON используется по умолчанию, а отсутствие записи возвращает `200 no_match`. `cache_purge_legacy_status` только предупреждает и не меняет ответы. Обновите клиентов; при изменении структуры общей памяти выполните полный перезапуск. Поколения ключей включаются явно и меняют имена файлов кеша. См. [переход на v4](MIGRATION.ru.md).

<a id="documentation"></a>

## Документация

- [Справочник директив](DIRECTIVES.ru.md)
- [Другие примеры конфигурации](#localized-examples)
- [Эксплуатация и устранение неполадок](OPERATIONS.ru.md)
- [HTTP-ответы и ограничения REFRESH](../REFRESH.md)
- [Переход на v4](MIGRATION.ru.md)
- [Архитектура реализации](../ARCHITECTURE.md)
- [Тестирование](OPERATIONS.ru.md#testing)
- [Разработка и тесты](../../t/TESTING.md)

<a id="see-also"></a>

## См. также

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [Документация nginx](https://nginx.org/en/docs/)

<a id="sponsors"></a>

## Спонсоры

Благодарим следующие организации за бесплатное предоставление инструментов и сервисов для разработки и сопровождения `ngx_cache_purge` как проекта с открытым исходным кодом:
- [PVS-Studio](https://pvs-studio.com/en/pvs-studio/?utm_source=website&utm_medium=github&utm_campaign=open_source) - статический анализатор кода для разработки корпоративного ПО (C, C++, C#, Go и Java) и веб-приложений (JS и TS).

<a id="license"></a>

## Лицензия

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
