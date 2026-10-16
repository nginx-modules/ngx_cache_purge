

<a id="contents"></a>

## Inhalt

- [Funktionen](#features)
- [Kompatibilität](#compatibility)
- [Pakete der Distributionen](#distribution-packages)
- [Installation](#installation)
- [Das passende Verhalten wählen](#choosing-a-mode)
- [Verwendung](#usage)
- [Teilweises Löschen nach Schlüssel](#partial-key-purge)
- [Generationeninvalidierung](#generation-invalidation)
- **REFRESH** — [Cache-Inhalte beim Ursprungsserver prüfen (REFRESH)](#conditional-refresh)
- [Sicherheit](#security)
- [Migration](#migration)
- [Übersetzte Konfigurationsbeispiele](#localized-examples)
- [Dokumentation](#documentation)
- [Weitere Informationen](#see-also)
- [Lizenz](#license)

<a id="features"></a>

## Funktionen

- **Inline-Purge** — eigene HTTP-Methode (`PURGE`) mit IP-Zugriffskontrolle
- **Eigene Purge-Location** — Syntax `proxy_cache_purge zone key` für per regulärem Ausdruck erfasste Schlüssel, ohne `proxy_pass`
- **Platzhalter / Teilabgleich** — ein abschließendes `*` durchsucht das Cache-Verzeichnis und entfernt passende Einträge
- **`purge_all`** — entfernt mit einer Anfrage alle Einträge einer Cache-Zone
- **Hintergrundwarteschlange** — verzögerter Durchlauf in begrenzten Event-Loop-Schritten oder einem optionalen NGINX-Thread-Pool; konfigurierbare Auftragsgruppen und Pausen
- **Vary-Unterstützung** — entfernt nach einem exakten Purge auch Dateivarianten mit demselben Schlüssel, etwa gzip- und Vary-Varianten
- **Antwortformate** — `json` (Standard), `html`, `xml`, `text`, `none`
- **Benannte Pools** — vererbbare Pool-Auswahl; inkrementelle Ausführung ohne Thread-Unterstützung und unter Windows
- **Deduplizierung aktiver Aufträge** — eine indexierte Operation mit höchstens einem vorgemerkten Wiederholungsdurchlauf für während des Durchlaufs eintreffende Purges
- **Status** — Warteschlangen-, Lösch- und HIT/MISS-Metriken je Cache, ohne den gemeinsamen Warteschlangen-Mutex beim Lesen zu sperren
- **Generationsschlüssel** — Invalidierung einzelner Schlüssel, von Präfixen und Namespaces ohne Datenträger-I/O im PURGE-Request; späte alte Schreibvorgänge bleiben unerreichbar
- [Cache-Inhalte beim Ursprungsserver prüfen (REFRESH)](#conditional-refresh)

<a id="compatibility"></a>

## Kompatibilität

Verwenden Sie im Produktivbetrieb eine gepflegte NGINX-Version. POSIX/UNIX ist die Hauptplattform; Windows wird experimentell unterstützt. Threads sind optional. [NGINX / CI](../../t/TESTING.md#compatibility-matrix).

<a id="distribution-packages"></a>

## Pakete der Distributionen

Mehrere Distributionen und Ports-Projekte paketieren diesen Fork, stellen jedoch nicht unbedingt v4 bereit. Prüfen Sie vor der Installation die Paketversion und die NGINX-ABI. Einzelheiten: [Distributionspakete und Paketquellen](../DISTRIBUTIONS.md).

<a id="installation"></a>

## Installation

```bash
cd /path/to/nginx-source
./configure --add-module=/path/to/ngx_cache_purge
make
make install
```

Dynamisches Modul:

```bash
./configure --with-compat --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

Verwenden Sie den Quellcode der auf dem Server installierten NGINX-Version. `--with-compat` garantiert keine Kompatibilität zwischen beliebigen NGINX-Versionen oder C-Bibliotheken. Siehe [dynamische Module und Release-Archive](../BINARY_MODULES.md) (Englisch).

<a id="choosing-a-mode"></a>

## Das passende Verhalten wählen

| Ziel | Konfiguration | Bedeutung der Antwort |
|---|---|---|
| Veraltete Einträge löschen und den Löschversuch abwarten | PURGE ohne Hintergrundwarteschlange | `200 purged` oder `200 no_match`; bei Fehlern 4xx/5xx |
| Bereinigung eines Präfixes oder des ganzen Caches einplanen | PURGE mit `cache_purge_background_queue on` | `202 accepted`; alte Einträge können bis zur Bereinigung noch ausgeliefert werden |
| Alte Inhalte für neue Anfragen sperren, auch bei verspäteten Schreibvorgängen | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`; neue Schlüsselgeneration, Dateilöschung folgt |
| Änderungen beim Ursprungsserver prüfen | Generationen + `proxy_cache_refresh` + internes Profil | `200` mit Ergebnissen pro Eintrag und Fortsetzung; kein Vorbefüllen |

Die Verfahren lassen sich kombinieren: Ein Generationswechsel plant ebenfalls eine Bereinigung ein. Die Warteschlange allein schützt nicht vor verspäteten Schreibvorgängen. Siehe [v4-Beispiel](#localized-examples), [PURGE-Beispiele](#localized-examples) und [HTTP-Antworten](../HTTP_RESPONSES.md) (Englisch).

<a id="usage"></a>

## Verwendung

[→ Übersetzte Konfigurationsbeispiele](#localized-examples)


<a id="partial-key-purge"></a>

## Teilweises Löschen nach Schlüssel

Hängen Sie `*` an den **ausgewerteten Cache-Schlüssel** an, um Präfixe ohne Beachtung der Groß-/Kleinschreibung abzugleichen. Exakte Schlüssel und Vary bleiben groß-/kleinschreibungssensitiv. Physische Präfixe dürfen ohne abschließendes `*` bis zu 511 Byte umfassen. Längere Präfixe werden einmal protokolliert und liefern ohne Dateidurchlauf `400 invalid_selector`.

Mit `cache_purge_key` wird der logische Ausdruck abgeglichen; erlaubt sind höchstens 8096 Bytes einschließlich eines abschließenden `*`.

Bei einem auf `$uri` endenden Schlüsselausdruck wählt diese Anfrage ein URI-Präfix:

```http
PURGE /images/header*
```

Der Stern muss das letzte Byte des ausgewerteten Schlüssels sein, nicht nur des URI-Pfads. Query-String- und Variablensuffixe in `proxy_cache_key` berücksichtigen. Der alleinige Schlüssel `*` durchsucht alle zulässigen Einträge.

<a id="generation-invalidation"></a>

## Generationeninvalidierung

Schlüsselgenerationen verhindern, dass neue Anfragen nach PURGE alte Inhalte treffen, auch bei verspäteten Schreibvorgängen. Dateien werden separat gelöscht. Ein Namensraum teilt Invalidierungsregeln; sein Zustandsverzeichnis muss Neustarts überdauern.

<a id="conditional-refresh"></a>

## Cache-Inhalte beim Ursprungsserver prüfen (REFRESH)

REFRESH sendet HEAD für dieselbe Darstellung am Ursprung und nutzt gespeicherte ETag- und Last-Modified-Werte. Ist die Produktseite bereits als veraltet bekannt, verwenden Sie PURGE. REFRESH lädt keinen neuen Inhalt, füllt keinen leeren Cache und verlängert keine TTL. [HTTP / REFRESH](../REFRESH.md).

| Tatsächliches Origin-Ergebnis | Aktion / Ergebnis |
|---|---|
| `304` | Behalten; `kept` |
| `200`, `404`, `410` | Bedingte Generationsinvalidierung und verzögerte Bereinigung; `invalidated` |
| Anderer Status | Behalten; `upstream_status` |
| Kein Origin-Status / Transportfehler | Behalten; `transport_error` |
| Vary-Metadaten | Kein HEAD; `skipped_vary` |
| Ungültige Zuordnung oder Profil | Behalten; `skipped_profile` |
| Generation inzwischen ersetzt | Keine neue Invalidierung; `raced_replaced` |
| Interner Invalidierungs-/Speicherfehler | Behalten; `invalidation_error` |
| Vor Seitenende nicht gestartet/abgeschlossen | `undispatched`; Cursor bleibt davor |

<a id="security"></a>

## Sicherheit

- Purge-Endpunkte mit `allow` / `deny` oder vorgelagerter Authentifizierung begrenzen. Niemals öffentlich freigeben.
- `cache_purge_queue_size` passend zur Last wählen, um SHM-Verbrauch bei Anfragefluten zu begrenzen.
- Purge-Locations gegebenenfalls mit `limit_req` unabhängig vom normalen Verkehr drosseln.

<a id="migration"></a>

## Migration

Änderungen am SHM-Layout erfordern einen vollständigen Neustart. Methoden-/ACL-Syntax und Protokollhandler bleiben erhalten. v4 verwendet standardmäßig JSON, übereinstimmende Ergebniskennungen in Header und Antwortkörper sowie `200 no_match` bei fehlendem Eintrag. `cache_purge_legacy_status` erzeugt nur eine Warnung. Passen Sie Clients an den [HTTP-Vertrag](../HTTP_RESPONSES.md) an. Die Kapazität umfasst zugewiesene Aufgaben; Überlast wartet auf Zulassung. Aktive Duplikate können einen weiteren Durchlauf vormerken.

<a id="localized-examples"></a>

## Übersetzte Konfigurationsbeispiele

Die vollständigen lokalen Beispiele enthalten den Produktkatalog mit REFRESH sowie die klassischen PURGE-Konfigurationen. [Englische Konfigurationsbeispiele](../EXAMPLES.md).

<details>
<summary>Übersetzte Konfigurationsbeispiele — 7 Beispiele</summary>

<a id="generation-and-refresh-example"></a>

### Beispiel: Produktkatalog

Diese vollständige Konfiguration nimmt Anfragen lokal auf Port 8080 an und leitet sie an die Anwendung auf Port 8081 weiter. Die Anwendung muss `/products/42` mit Host `shop.example.com` bereitstellen. Passen Sie Adresse, Domain und beschreibbare Cache- und Zustandsverzeichnisse an.

`shop.example.com` ist die Domain im HTTP-Host-Header, kein Verzeichnis. `/products/42` ist die Produkt-URI. `catalog:` ist ein festes Schlüsselpräfix, kein Hostname. Präfix und vollständige URI einschließlich Query-String bilden den logischen Schlüssel, etwa `catalog:/products/42`. Das REFRESH-Profil entfernt `catalog:` und prüft `/products/42` in derselben Anwendung.

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

Rufen Sie ein vorhandenes, cachefähiges Produkt per GET ab, bis `X-Cache-Status: HIT` erscheint. Senden Sie dann REFRESH:

```sh
curl -i 'http://127.0.0.1:8080/products/42'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/42' \
  'http://127.0.0.1:8080/admin/refresh'
```

Hat sich das Produkt geändert, senden Sie PURGE. Hier sperrt `200 invalidated` die alte Generation für neue Anfragen; der nächste GET ruft die Anwendung auf.

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/products/42'
```

Für alle Produktseiten verwenden Sie `catalog:/products/*`. Bei `complete: false` behalten Sie den Selektor bei und ersetzen `<next_cursor>` durch den erhaltenen Wert. Beheben Sie HTTP-Fehler vor dem Fortsetzen.

```sh
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

<a id="sample-configurations"></a>

### Beispielkonfigurationen

Die ersten fünf Blöcke sind jeweils vollständige, alternative `nginx.conf`-Beispiele. Der letzte Block ergänzt die Vary-Behandlung in einem vorhandenen `http`-Abschnitt. NGINX nimmt lokale Anfragen auf Port 8080 an; `catalog_backend` bezeichnet die Upstream-Gruppe für die Anwendung unter `127.0.0.1:8081`. `shop.example.com` ist der an die Anwendung gesendete HTTP-Host. Passen Sie die Adressen und das für NGINX beschreibbare Cache-Verzeichnis an Ihre Umgebung an.

<a id="basic--inline-purge-method"></a>

#### Basis — Inline-Purge-Methode

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

#### Basis — separate Purge-Location

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

#### Aktivierte Hintergrundwarteschlange

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

#### Alle Einträge löschen

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

#### JSON-Antworten mit Überschreibung je Location

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

#### Purge mit Vary-Unterstützung

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

Nach dem Purge eines Schlüssels werden auch seine gzip- und Vary-Varianten an anderen Dateipfaden automatisch entfernt.

</details>

<a id="documentation"></a>

## Dokumentation

- [Direktivenreferenz](DIRECTIVES.de.md)
- [Weitere Konfigurationsbeispiele](#localized-examples)
- [Betrieb und Fehlerbehebung](OPERATIONS.de.md)
- [HTTP und REFRESH-Grenzen (Englisch)](../REFRESH.md)
- [Migration auf v4](MIGRATION.de.md)
- [Implementierungsarchitektur (Englisch)](../ARCHITECTURE.md)
- [Tests](OPERATIONS.de.md#testing)
- [Entwicklung und Tests (Englisch)](../../t/TESTING.md)

<a id="see-also"></a>

## Weitere Informationen

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [nginx documentation](https://nginx.org/en/docs/)

<a id="license"></a>

## Lizenz

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
