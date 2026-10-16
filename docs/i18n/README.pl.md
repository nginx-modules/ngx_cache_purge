<a id="ngx_cache_purge"></a>

# ngx_cache_purge

Moduł NGINX do usuwania wpisów z pamięci podręcznej `FastCGI`, `proxy`, `SCGI` i `uWSGI`. W v4 generacje kluczy uniemożliwiają nowym żądaniom użycie starych wpisów jeszcze przed usunięciem plików. REFRESH sprawdza na serwerze źródłowym, czy zawartość pamięci proxy się zmieniła. Obie funkcje wymagają jawnej konfiguracji. Składnia PURGE pozostaje dostępna, lecz odpowiedzi HTTP się zmieniają: zobacz [migrację](README.pl.md#migration).

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

<a id="features"></a>

## Możliwości

- **PURGE w obsługującej lokalizacji** — dedykowana metoda HTTP (`PURGE`) z kontrolą dostępu IP
- **Oddzielna lokalizacja PURGE** — składnia strefa/klucz `proxy_cache_purge zone key`, z kluczem przechwyconym przez wyrażenie regularne, bez `proxy_pass`
- **Wzorzec / usuwanie częściowe** — końcowa `*` uruchamia przejście katalogu i usuwanie pasujących wpisów
- **`purge_all`** — usuwa całą strefę jednym żądaniem
- **Kolejka w tle** — odroczone przejście ograniczonymi krokami pętli zdarzeń lub opcjonalną pulą wątków NGINX; konfigurowalne partie i przerwy
- **Obsługa Vary** — po usunięciu dokładnego klucza usuwa warianty plikowe gzip/Vary o tym samym kluczu
- **Formaty odpowiedzi** — `json` (domyślny), `html`, `xml`, `text`, `none`
- **Nazwane pule** — dziedziczony wybór, tryb przyrostowy bez wątków i na Windows
- **Deduplikacja aktywnych zadań** — jedna indeksowana operacja i najwyżej jedno oczekujące powtórzenie dla żądań otrzymanych w czasie przejścia
- **Status** — metryki kolejki, usuwania i HIT/MISS na cache, odczytywane bez mutexu kolejki
- **Klucze generacji** — invalidacja dokładna, prefiksowa i przestrzeni nazw bez dyskowego I/O w ścieżce żądania; późne stare zapisy pozostają niedostępne
- [Sprawdzanie pamięci podręcznej na serwerze źródłowym (REFRESH)](#conditional-refresh)

<a id="choosing-a-mode"></a>

## Wybór sposobu działania

| Cel | Konfiguracja | Znaczenie odpowiedzi |
|---|---|---|
| Usunąć nieaktualne wpisy i zaczekać na próbę usunięcia | PURGE bez kolejki w tle | `200 purged` lub `200 no_match`; przy błędzie 4xx/5xx |
| Zaplanować czyszczenie prefiksu lub całej pamięci podręcznej | PURGE z `cache_purge_background_queue on` | `202 accepted`; stare wpisy mogą być nadal dostępne do czasu czyszczenia |
| Wykluczyć stare dane z nowych żądań, także po opóźnionym zapisie | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`; nowa generacja klucza, pliki usuwane później |
| Sprawdzić zmiany na serwerze źródłowym | Generacje + `proxy_cache_refresh` + profil wewnętrzny | `200` z wynikami dla wpisów i kontynuacją; bez wstępnego zapełniania |

Mechanizmy można łączyć: zmiana generacji także planuje czyszczenie. Sama kolejka nie chroni przed opóźnionym zapisem. Zobacz [przykład v4](README.pl.md#generation-and-refresh-example), [przykłady PURGE](#sample-configurations) i [odpowiedzi HTTP](../HTTP_RESPONSES.md) (po angielsku).

<a id="compatibility"></a>

## Zgodność

W środowisku produkcyjnym używaj nadal utrzymywanej wersji NGINX. Główną platformą jest POSIX/UNIX; obsługa Windows jest eksperymentalna. Wątki są opcjonalne. [NGINX / CI](../../t/TESTING.md#compatibility-matrix).

<a id="installation"></a>

## Instalacja

```bash
cd /path/to/nginx-source
./configure --add-module=/path/to/ngx_cache_purge
make
make install
```

Moduł dynamiczny:

```bash
./configure --with-compat --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

Użyj kodu źródłowego tej samej wersji NGINX, która jest zainstalowana na serwerze. `--with-compat` nie gwarantuje zgodności między dowolnymi wersjami NGINX ani bibliotekami C. Zobacz [moduły dynamiczne i archiwa wydań](../BINARY_MODULES.md) (po angielsku).

<a id="generation-and-refresh-example"></a>

## Przykład: katalog produktów

Ten kompletny przykład nasłuchuje lokalnie na porcie 8080 i przekazuje żądania do aplikacji na porcie 8081. Aplikacja musi udostępniać `/products/42` z Host `shop.example.com`. Zmień adres, domenę oraz zapisywalne katalogi pamięci podręcznej i stanu.

`shop.example.com` to domena nagłówka HTTP Host, nie katalog. `/products/42` to URI produktu. `catalog:` jest stałym prefiksem klucza, nie nazwą hosta. Prefiks i pełny URI wraz z parametrami tworzą klucz logiczny, np. `catalog:/products/42`. Profil REFRESH usuwa `catalog:` i sprawdza `/products/42` w tej samej aplikacji.

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

Pobieraj istniejący produkt możliwy do zapisania w pamięci podręcznej, aż GET zwróci `X-Cache-Status: HIT`, a następnie wyślij REFRESH:

```sh
curl -i 'http://127.0.0.1:8080/products/42'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/42' \
  'http://127.0.0.1:8080/admin/refresh'
```

Jeśli produkt się zmienił, wyślij PURGE. Tutaj `200 invalidated` blokuje nowym żądaniom starą generację; kolejny GET trafia do aplikacji.

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/products/42'
```

Dla wszystkich produktów użyj `catalog:/products/*`. Przy `complete: false` zachowaj selektor i zastąp `<next_cursor>` otrzymaną wartością. Przed kontynuacją usuń przyczynę błędu HTTP.

```sh
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

<a id="partial-key-purge"></a>

## Usuwanie według części klucza

Dodaj `*` do **obliczonego klucza pamięci podręcznej**, aby dopasować prefiks bez rozróżniania wielkości liter. Klucze dokładne i Vary nadal ją rozróżniają. Prefiks fizycznego czyszczenia może mieć do 511 bajtów bez końcowej `*`. Dłuższy prefiks powoduje jeden wpis w dzienniku i `400 invalid_selector` bez przeglądania plików.

Z `cache_purge_key` dopasowanie dotyczy wyrażenia logicznego, maksymalnie 8096 bajtów z końcową `*`.

Dla wyrażenia kończącego się `$uri` to żądanie wybiera prefiks URI:

```http
PURGE /images/header*
```

Gwiazdka musi kończyć obliczony klucz, nie tylko ścieżkę URI. Uwzględnij parametry lub zmienne na końcu `proxy_cache_key`. Samo `*` przechodzi wszystkie kwalifikujące się wpisy.

<a id="generation-invalidation"></a>

## Invalidacja generacji

Generacje kluczy blokują nowym żądaniom dostęp do starej treści po PURGE, także gdy wcześniejsza odpowiedź zostanie zapisana później. Pliki są usuwane osobno. Przestrzeń nazw współdzieli reguły unieważniania; zachowaj jej katalog stanu między restartami.

<a id="conditional-refresh"></a>

## Sprawdzanie pamięci podręcznej na serwerze źródłowym (REFRESH)

REFRESH wysyła HEAD dla tej samej reprezentacji na serwerze źródłowym, używając zapisanych ETag i Last-Modified. Jeśli karta produktu jest już nieaktualna, użyj PURGE. REFRESH nie pobiera nowej treści, nie wypełnia pustej pamięci podręcznej i nie przedłuża TTL. [HTTP / REFRESH](../REFRESH.md).

| Surowy wynik źródła | Działanie / wynik |
|---|---|
| `304` | Zachowaj; `kept` |
| `200`, `404`, `410` | Warunkowa invalidacja generacji i odroczone czyszczenie; `invalidated` |
| Inny status | Zachowaj; `upstream_status` |
| Brak statusu / błąd transportu | Zachowaj; `transport_error` |
| Metadane Vary | Bez HEAD; `skipped_vary` |
| Błędne mapowanie lub profil | Zachowaj; `skipped_profile` |
| Generacja zmieniona w międzyczasie | Bez nowej invalidacji; `raced_replaced` |
| Wewnętrzny błąd invalidacji/alokacji | Zachowaj; `invalidation_error` |
| Praca niezgłoszona/nieukończona do zatrzymania strony | `undispatched`; kursor jej nie przekracza |

<a id="sample-configurations"></a>

## Przykładowe konfiguracje

Pierwszych pięć bloków to odrębne, kompletne przykłady `nginx.conf`. Ostatni dodaje obsługę Vary do istniejącej sekcji `http`. NGINX przyjmuje lokalne połączenia na porcie 8080; `catalog_backend` to nazwa grupy upstream z aplikacją pod adresem `127.0.0.1:8081`. `shop.example.com` jest wartością nagłówka HTTP Host wysyłanego do aplikacji. Dostosuj adresy i wybierz katalog pamięci podręcznej, w którym NGINX może zapisywać pliki.

<a id="basic--inline-purge-method"></a>

### Podstawowa — metoda PURGE w tej samej lokalizacji

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

### Podstawowa — oddzielna lokalizacja PURGE

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

### Włączona kolejka w tle

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

### Usuwanie wszystkich wpisów

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

### JSON z nadpisaniem na poziomie location

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

### Usuwanie z obsługą Vary

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

Po usunięciu klucza automatycznie usuwa się też wszystkie warianty gzip/Vary z innych ścieżek.

<a id="migration"></a>

## Migracja

Zmiany struktury SHM wymagają pełnego restartu. Składnia metoda/ACL i obsługa protokołów pozostają dostępne. v4 domyślnie używa JSON, zgodnych kodów w nagłówku i treści oraz `200 no_match` przy braku wpisu. `cache_purge_legacy_status` powoduje tylko ostrzeżenie. Dostosuj klientów do [kontraktu HTTP](../HTTP_RESPONSES.md). Pojemność obejmuje przydzielone zadania; przeciążone żądanie czeka na przyjęcie. Aktywne duplikaty mogą zaplanować kolejny przebieg.

<a id="security"></a>

## Bezpieczeństwo

- Ogranicz endpointy PURGE przez `allow` / `deny` lub wcześniejsze uwierzytelnianie. Nigdy nie wystawiaj publicznie.
- Dobierz `cache_purge_queue_size` do ruchu, ograniczając SHM przy zalewie żądań.
- Rozważ `limit_req` dla purgi niezależnie od zwykłego ruchu.

<a id="documentation"></a>

## Dokumentacja

- [Opis dyrektyw](DIRECTIVES.pl.md)
- [Dalsze przykłady konfiguracji](README.pl.md#sample-configurations)
- [Eksploatacja i diagnostyka](OPERATIONS.pl.md)
- [HTTP i ograniczenia REFRESH (angielski)](../REFRESH.md)
- [Migracja do v4](MIGRATION.pl.md)
- [Architektura implementacji (angielski)](../ARCHITECTURE.md)
- [Testowanie](OPERATIONS.pl.md#testing)
- [Rozwój i testy (angielski)](../../t/TESTING.md)

<a id="see-also"></a>

## Zobacz też

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [Dokumentacja nginx](https://nginx.org/en/docs/)

<a id="license"></a>

## Licencja

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
