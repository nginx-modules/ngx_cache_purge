<a id="ngx_cache_purge"></a>

# ngx_cache_purge

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

NGINX-Modul zum Entfernen von Einträgen aus `FastCGI`-, `proxy`-, `SCGI`- und `uWSGI`-Caches. In v4 verhindern Schlüsselgenerationen, dass neue Anfragen alte Einträge verwenden, bevor deren Dateien gelöscht werden. REFRESH prüft beim Ursprungsserver, ob sich der Inhalt eines Proxy-Caches geändert hat. Beide Funktionen müssen ausdrücklich konfiguriert werden. Die PURGE-Syntax bleibt verfügbar; die HTTP-Antworten ändern sich. Siehe [Migration](#migration).

---

<a id="contents"></a>

## Inhalt

- [Funktionen](#features)
- [Das passende Verhalten wählen](#choosing-a-mode)
- [Kompatibilität](#compatibility)
- [Installation](#installation)
- [Teilweises Löschen nach Schlüssel](#partial-key-purge)
- [Generationeninvalidierung](#generation-invalidation)
- [Cache-Inhalte beim Ursprungsserver prüfen (REFRESH)](#conditional-refresh)
- [Beispielkonfigurationen](#sample-configurations)
- [Direktiven](#directives)
- [Ausführungsmodell](#execution-model)
- [Leistungsabstimmung](#performance-tuning)
- [Monitoring und Debugging](#monitoring-and-debugging)
- [Fehlerbehebung](#troubleshooting)
- [Tests](#testing)
- [Migration](#migration)
- [Temporäre Verzeichnisse](#temporary-directories)
- [Sicherheit](#security)
- [Lizenz](#license)

---

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

---

<a id="choosing-a-mode"></a>

## Das passende Verhalten wählen

| Ziel | Konfiguration | Bedeutung der Antwort |
|---|---|---|
| Veraltete Einträge löschen und den Löschversuch abwarten | PURGE ohne Hintergrundwarteschlange | `200 purged` oder `200 no_match`; bei Fehlern 4xx/5xx |
| Bereinigung eines Präfixes oder des ganzen Caches einplanen | PURGE mit `cache_purge_background_queue on` | `202 accepted`; alte Einträge können bis zur Bereinigung noch ausgeliefert werden |
| Alte Inhalte für neue Anfragen sperren, auch bei verspäteten Schreibvorgängen | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`; neue Schlüsselgeneration, Dateilöschung folgt |
| Änderungen beim Ursprungsserver prüfen | Generationen + `proxy_cache_refresh` + internes Profil | `200` mit Ergebnissen pro Eintrag und Fortsetzung; kein Vorbefüllen |

Die Verfahren lassen sich kombinieren: Ein Generationswechsel plant ebenfalls eine Bereinigung ein. Die Warteschlange allein schützt nicht vor verspäteten Schreibvorgängen. Siehe [v4-Beispiel](#generation-and-refresh-example), [PURGE-Beispiele](#sample-configurations) und [HTTP-Antworten](../HTTP_RESPONSES.md) (Englisch).

---

<a id="compatibility"></a>

## Kompatibilität

Die Matrix zeigt die konfigurierten CI-Ziele. Ein erfolgreicher Job bestätigt die geprüfte Quell- und Build-Konfiguration, nicht sämtliche Versionen einer Reihe.

| NGINX | CI: push / PR | CI: + full |
|---|---|---|
| 1.30.5, 1.31.6 | `test-matrix` | — |
| 1.31.6 | `build-compatibility` (GCC/Clang), `sanitizer`, `native-macos`, `native-windows` (x64/x86) | `memory-leak-test` (Valgrind) |
| 1.7.9 | `build-compatibility` (GCC) | — |
| 1.7.11, 1.7.12 | `build-compatibility` (GCC) | — |
| 1.20.2, 1.26.3, 1.28.3 | — | `test-matrix` |
| 1.11.4, 1.11.5 | — | `build-compatibility` (GCC) |

Linux-Jobs verwenden dieselben Docker-Images und denselben Testeinstieg wie lokale Läufe. Native macOS- und Windows-Jobs prüfen die jeweiligen Datei- und Prozess-APIs. Windows wird mit MSVC gebaut; `nginx.exe` durchläuft HTTP-, Pfad- und Reload-Prüfungen. POSIX-Prozess- und Signaltests laufen unter Linux und macOS. Windows führt weder diese Tests noch den optionalen NGINX-Thread-Pool-Pfad aus.

Ohne `--with-threads` erfolgt der Durchlauf schrittweise. Bei epoll vor NGINX 1.7.12 nutzt das Modul ebenfalls diesen Modus, um ein Benachrichtigungsrennen im Kern zu umgehen. BSD hat keinen nativen CI-Job. Normale Läufe prüfen stable/mainline und API-Grenzen; erweiterte Läufe ergänzen ältere Versionen und Valgrind. Reine Dokumentationsänderungen überspringen Builds. Zeitplan und Befehle: [Tests](../../t/TESTING.md).

Konfiguration, Handler, Warteschlange, Durchlauf, Invalidierung, REFRESH und Antworten sind getrennte Komponenten. Gemeinsame Pfadfunktionen stehen in `ngx_cache_purge.h`, Plattformoperationen in POSIX-/Win32-Headern. `generation.*` heißt jetzt `invalidation.*`; Generation bezeichnet weiterhin die Schlüsselversion. Siehe [Architektur](../ARCHITECTURE.md).

Das Modul folgt dem C89-kompatiblen NGINX-Stil und verwendet dessen Event-, Slab-, Rbtree- und Atomic-APIs. C11-Atomics oder neuere Dateisystem-APIs werden nicht vorausgesetzt. Strenge GNU89-Builds prüfen Deklarationsreihenfolge und Warnungen; tatsächlich geprüfte Compiler und Plattformen werden mit den Testergebnissen dokumentiert.

---

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
./configure --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

---

<a id="partial-key-purge"></a>

## Teilweises Löschen nach Schlüssel

Hängen Sie `*` an den **ausgewerteten Cache-Schlüssel** an, um Präfixe ohne Beachtung der Groß-/Kleinschreibung abzugleichen. Exakte Schlüssel und Vary bleiben groß-/kleinschreibungssensitiv. Physische Präfixe dürfen ohne abschließendes `*` bis zu 511 Byte umfassen. Längere Präfixe werden einmal protokolliert und liefern ohne Dateidurchlauf `400 invalid_selector`.

Mit `cache_purge_key` wird der logische Ausdruck abgeglichen; erlaubt sind höchstens 8096 Bytes einschließlich eines abschließenden `*`.

Bei einem auf `$uri` endenden Schlüsselausdruck wählt diese Anfrage ein URI-Präfix:

```http
PURGE /images/header*
```

Der Stern muss das letzte Byte des ausgewerteten Schlüssels sein, nicht nur des URI-Pfads. Query-String- und Variablensuffixe in `proxy_cache_key` berücksichtigen. Der alleinige Schlüssel `*` durchsucht alle zulässigen Einträge.

---

<a id="generation-invalidation"></a>

## Generationeninvalidierung

Ein logischer Schlüssel bezeichnet ein Objekt. Ein Namensraum fasst Schlüssel mit gemeinsamen Invalidierungsregeln zusammen. Eine Generation ist eine Version im NGINX-Cache-Schlüssel. Nach PURGE verwenden neue Anfragen eine neue Version; ein zuvor gestarteter GET darf noch enden, aber seine spät geschriebene Datei wird für diese Anfragen kein HIT.

In einem gebundenen Namespace veröffentlicht PURGE eine neue Generation und plant einen deduplizierten Durchlauf veralteter Generationen, bevor `200` mit `invalidated` zurückkehrt. Der Aufnahmepfad öffnet oder löscht keine Cache-Datei. Neue Requests, die den Schlüssel erstmals nach Veröffentlichung auswerten, können die vorherige Generation nicht treffen. Jeder Request fixiert seine Generation je Namespace/logischem Schlüssel, auch über interne Weiterleitungen; eine bereits begonnene Antwort darf enden, ihr später Cache-Schreibvorgang wird aber kein HIT für nachfolgende neue Requests. Exakte Treffer beachten Groß-/Kleinschreibung, Präfixe ignorieren ASCII-Groß-/Kleinschreibung; `purge_all` ändert den gesamten Namespace, auch bewusst gemeinsam nutzende Cache-Pfade. Physische Bereinigung wird für den adressierten Pfad eingeplant; andere Pfade nutzen Core-Ablauf oder einen weiteren PURGE.

Eine angenommene Invalidierung garantiert keine sofortige Freigabe von Plattenplatz. Ein nach dem Durchlauf spät geschriebener alter Eintrag bleibt unerreichbar und wird später oder durch Core-Inactive-Ablauf gelöscht. Bei Namespace-SHM-Druck werden Regeln auf eine neue Namespace-Basis zusammengefasst: zusätzliche Schlüssel werden kalt, statt ungültige wieder erreichbar zu machen. Je späterer Invalidierung werden höchstens 64 abgetrennte Trie-Knoten freigegeben. Wiederholte Hot-Key-PURGEs lassen den Regelbaum nicht unbegrenzt wachsen. Die Komplexität hängt von begrenzter Schlüssellänge und Aufgabenindex ab, nicht von der Cache-Dateizahl; eine zeitliche O(1)- oder Starvation-Freiheit bei Überlast wird nicht behauptet. Aufnahme kann warten. Gegen anschließende GET-Stampedes Core-`proxy_cache_lock` und entsprechende Cache-Regeln einsetzen.

<a id="conditional-refresh"></a>

## Cache-Inhalte beim Ursprungsserver prüfen (REFRESH)

REFRESH eignet sich, wenn ein Objekt im Cache liegt und unklar ist, ob es sich am Ursprung geändert hat, etwa nach einem Katalogimport. Bei sicher veralteten Inhalten verwenden Sie PURGE. REFRESH sendet HEAD ohne Antwortkörper und übermittelt vorhandene ETags oder Änderungszeiten als HTTP-Prüfbedingungen. Das Profil ist eine interne Proxy-Location, die Ursprung, Host, Header und URI-Zuordnung festlegt.

REFRESH prüft noch gespeicherte Objekte vor TTL-Ablauf; es wärmt sie nicht vor und verlängert bei `304` keine TTL. Es ergänzt Core-`proxy_cache_revalidate` und `proxy_cache_background_update` für normalen Verkehr abgelaufener Objekte. Ignoriert der Origin Validatoren und sendet immer `200`, wird REFRESH zur Invalidierung mit zusätzlichem HEAD. Kandidaten sind nur aktuelle gültige Einträge am kanonischen Hash-Pfad. Ein physischer Ersatz in derselben Generation kann vorsorglich invalidiert werden; der Generationsvergleich verhindert, dass ein alter HEAD eine spätere PURGE-Generation invalidiert.

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

Pro Worker ist höchstens eine REFRESH-Seite mit einem gleichzeitigen Probe aktiv; ein gemeinsames Budget erlaubt höchstens 16 Probes über alle Worker. Weitere Seitenrequests warten ohne Kandidatenarray. Jede Seite scannt mit dem gemeinsamen Cursor, hält maximal 32 Kandidaten und startet danach serielle Core-HEAD-Subrequests. Damit sind Kandidatenspeicher und Subrequest-Zahl im gemeinsamen Hauptrequest-Pool begrenzt. Identische REFRESH-HTTP-Requests werden nicht dedupliziert, teilen aber das Parallelitätslimit. Ihre Invalidierungen nutzen die gemeinsame deduplizierte PURGE-Queue.

Ohne Cursor beginnen und dann `X-Cache-Purge-Cursor: <next_cursor>` mit demselben logischen Selektor/Profil an denselben Endpunkt senden, bis `complete` wahr ist. Der Cursor ist der zuletzt abgeschlossene 32-stellige Hex-Dateiname; ungültige oder doppelte Cursor-Header ergeben `400`. `scan_complete` bedeutet erfolgreiche vollständige Aufzählung; `complete` zusätzlich keine weitere passende Seite und Abschluss aller gewählten Entscheidungen. `timed_out` meldet das Zeitlimit; Scanfehler liefern `500 scan_failed`. `undispatched` zählt ausgewählte, nicht abgeschlossene Einträge; bei unvollständigem Scan ist dies nur eine Untergrenze, keine Cache-Gesamtzahl. Dann starten keine Probes, die Ergebnisliste bleibt leer und der Eingabecursor unverändert. Ergebnisse enthalten Dateihashes und echte Upstream-Statuswerte, keine Schlüssel oder Zugangsdaten.

Jede Seite durchsucht das Verzeichnis erneut für die nächsten Hashes: begrenzter Speicher wird gegen wiederholte I/O getauscht, insgesamt `O(Dateien × Seiten)`. Gleichzeitiges Einfügen/Löschen macht die Seitennavigation schwach konsistent; für einen späteren vollständigen Stand von vorn beginnen. Selektoren und Timeouts entsprechend wählen. Diese Verwaltungsschnittstelle eignet sich nicht für häufiges vollständiges Scannen sehr großer aktiver Caches.

<a id="generation-and-refresh-example"></a>

### Beispiel für Generationen und REFRESH

Dieses Fragment gehört in main/http; behalten Sie Ihren `events`-Block bei. Passen Sie Ursprung und Pfade an. Ohne `--with-threads` entfernen Sie `thread_pool` und `cache_purge_thread_pool`, um inkrementell zu scannen.

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
            add_header X-Cache-Status $upstream_cache_status always;
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

Rufen Sie zuerst ein vorhandenes, cachebares Objekt ab. Wiederholen Sie GET bis `X-Cache-Status: HIT` erscheint. Prüfen Sie dann dasselbe Objekt mit REFRESH. Nach `kept` bleibt der nächste GET ein HIT; nach `invalidated` fragt er den Ursprung erneut ab.

```sh
curl -i 'http://127.0.0.1:8080/objects/item'
curl -i -X REFRESH -H 'X-Cache-Key: origin.test/objects/item' \
  'http://127.0.0.1:8080/admin/refresh'
```

Beginnen Sie bei einem Präfix ohne Cursor. Bei `complete: false` wiederholen Sie die Anfrage mit dem zurückgegebenen Wert in `X-Cache-Purge-Cursor` statt `<next_cursor>`. Ändern Sie den Selektor nicht. Klären Sie HTTP-Fehler vor dem Fortsetzen.

```sh
curl -i -X REFRESH -H 'X-Cache-Key: origin.test/objects/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: origin.test/objects/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

Ist das Objekt sicher veraltet, verwenden Sie PURGE direkt. Mit den Generationen dieses Beispiels bedeutet `200 invalidated`, dass neue Anfragen die alte Generation nicht mehr nutzen, auch wenn deren Dateien noch existieren.

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/objects/item'
```

Siehe [HTTP und Grenzfälle](../REFRESH.md) (Englisch). Ein Scanfehler liefert `500 scan_failed`, keine Fortsetzungsseite mit `scan_error: true`.

<a id="sample-configurations"></a>

## Beispielkonfigurationen

<a id="basic--inline-purge-method"></a>

### Basis — Inline-Purge-Methode

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

### Basis — separate Purge-Location

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

### Aktivierte Hintergrundwarteschlange

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

### Alle Einträge löschen

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

### JSON-Antworten mit Überschreibung je Location

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

### Purge mit Vary-Unterstützung

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

Nach dem Purge eines Schlüssels werden auch seine gzip- und Vary-Varianten an anderen Dateipfaden automatisch entfernt.

---

<a id="directives"></a>

## Direktiven

<a id="proxy_cache_purge"></a>

### `proxy_cache_purge`

```
Syntax:  proxy_cache_purge on | off;
         proxy_cache_purge <method> [purge_all] from all | <cidr> ...;
         proxy_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

**Inline-Form** (`from …`) — fängt die genannte HTTP-Methode in einer Proxy-Location ab und löscht den passenden Eintrag. `on` steht für `PURGE`, `off` deaktiviert Purge. Eine CIDR-Liste begrenzt optional die erlaubten Adressen; `from all` erlaubt jede Adresse. `purge_all` vor `from` leert die gesamte Cache-Zone unabhängig vom Anfrage-URI.

**Separate Location** (Zone und Schlüssel als Argumente) — nur innerhalb einer Purge-Location erlaubt, typischerweise einer Regex-Location zur Schlüsselerfassung. Sucht die Zone nach Namen und löscht anhand des kompilierten Schlüsselausdrucks. Diese Form darf nicht zusammen mit `proxy_cache` oder `proxy_pass` in derselben Location verwendet werden.

<a id="fastcgi_cache_purge"></a>

### `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Entspricht `proxy_cache_purge` für FastCGI-Zonen, die mit `fastcgi_cache` / `fastcgi_cache_path` konfiguriert sind.

<a id="scgi_cache_purge"></a>

### `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Entspricht `proxy_cache_purge` für SCGI-Zonen, die mit `scgi_cache` / `scgi_cache_path` konfiguriert sind.

<a id="uwsgi_cache_purge"></a>

### `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Entspricht `proxy_cache_purge` für uWSGI-Zonen, die mit `uwsgi_cache` / `uwsgi_cache_path` konfiguriert sind.

<a id="cache_purge_response_type"></a>

### `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

Wählt das Format erfolgreicher PURGE-Antworten; Standard ist JSON. `Cache-Purge-Code` und der Ergebniscode im Antwortkörper stimmen überein. Modulantworten enthalten `Cache-Control: no-store`; Fehler verwenden `application/problem+json` nach RFC 9457. Textschlüssel werden maskiert, binäre Schlüssel verlustfrei als Base64 mit Kodierungsangabe ausgegeben.

`none` unterdrückt den PURGE-Antwortkörper: abgeschlossene Erfolge liefern `204`, angenommene Hintergrundarbeit weiterhin `202`, Fehler weiterhin 4xx/5xx. Status und REFRESH behalten strukturiertes JSON. Siehe [HTTP-Vertrag](../HTTP_RESPONSES.md) für Codes, Felder und Grenzen.

<a id="cache_purge_background_queue"></a>

### `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

Aktiviert verzögerte Platzhalter- und `purge_all`-Operationen. Nach Aufnahme oder Zusammenfassung mit einem identischen wartenden oder laufenden Auftrag wird `202 Accepted` zurückgegeben. Exakte Purges bleiben unverändert.

Dieser Schalter steuert Legacy-Wildcard-/All-Verhalten. Eine `cache_purge_key`-Bindung nutzt immer verzögerte physische Bereinigung und logische `200`-Antworten, auch bei ausgeschaltetem Schalter.

Bei voller Warteschlange oder fehlgeschlagener Auftragsallokation bleibt die Anfrage bis zur Aufnahme offen. Während des Wartens findet kein eigener Verzeichnisdurchlauf statt. Duplikate wartender oder laufender Aufträge können auch bei voller Kapazität angenommen werden. Kann der kleine Wartezustand der Anfrage nicht allokiert werden, folgt `500`. Beim Verbindungsabbruch entfernt Cleanup den Timer und verringert die Warteanzeige.

Für diesen Schalter und `cache_purge_vary_aware` gilt der zuletzt deklarierte Wert. Ungültige Werte werden abgelehnt. `cache_purge_legacy_status` ist veraltet und wird ignoriert. Threads und Lebenszyklus: [Ausführungsmodell](#execution-model).

<a id="cache_purge_queue_size"></a>

### `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

Maximale Zahl ausstehender Aufträge über alle Cache-Pfade. Wartende und zugewiesene Aufträge belegen Kapazität. Duplikate verwenden eine Allokation gemeinsam; eine Wiederholung behält Allokation und Kapazitätsplatz. Unterschiedliche Aufträge warten auf einen freien Platz.

Die gemeinsame Zone entsteht, sobald Hintergrundverarbeitung oder ein Status-Endpunkt konfiguriert ist. Kapazitätsänderungen erfordern einen Neustart, auch wenn sie auf dieselbe Zonengröße gerundet werden. Reload lehnt Größenänderungen ab, um angenommene Arbeit zu erhalten. Zusätzliche Cache-Wurzeln können ebenfalls mehr Zonenspeicher und einen Neustart erfordern. Wichtige wartende und aktive Arbeit vorher abarbeiten lassen.

Auch die Deklaration eines Namespace erzeugt die Aufgabenwarteschlange.

Die Zonengröße umfasst Auftragsspeicher, Pfadmetadaten und ausgerichtete Worker-Metriken. Sie hängt vom Build und der Anzahl der Cache-Pfade ab; siehe [Speicherbemessung](#memory-sizing).

<a id="cache_purge_thread_pool"></a>

### `cache_purge_thread_pool`

```
Syntax:  cache_purge_thread_pool <name>;
Default: default
Context: http, server, location
```

Wählt mit normaler Vererbung den Executor angenommener Hintergrundaufträge. Beispiel:

```nginx
thread_pool purge_io threads=2 max_queue=128;
http {
    cache_purge_background_queue on;
    cache_purge_thread_pool purge_io;
}
```

NGINX validiert benannte Pools und meldet Abschlüsse über das native Task-Event. Private Pool-Strukturen oder eigene Abschlusswarteschlangen werden nicht kopiert. Bei Thread-Unterstützung müssen benannte Pools existieren. Ohne Threads und unter Windows wird die Direktive akzeptiert und in begrenzten Event-Loop-Schritten gearbeitet. Ein voller ausgewählter Pool verzögert den Auftrag; er verlagert I/O nicht in die Event-Loop. Eigene Pools benötigen zusätzliche Thread-Stacks und Kontextwechsel.

<a id="cache_purge_batch_size"></a>

### `cache_purge_batch_size`

```
Syntax:  cache_purge_batch_size <number>;
Default: 10
Minimum: 1
Maximum: 64
Context: http
```

Maximale Zahl bearbeiteter Aufträge je Worker zwischen Drosselungspausen. Fortschritt bleibt über Cursor-Schritte und Thread-Abschlüsse erhalten. Eine leere Warteschlange beendet die aktuelle Gruppe. Werte über 64 werden mit einer Konfigurationswarnung auf 64 begrenzt; null ist ungültig.

Gesteuert werden Aufträge, nicht Dateien oder Callback-Dauer. Kleinere Werte erzeugen mehr Pausen zwischen Aufträgen, unterbrechen aber keinen aktiven Thread-Durchlauf. Zur Abstimmung der Plattenlast zusammen mit `cache_purge_throttle_ms` verwenden.

<a id="cache_purge_throttle_ms"></a>

### `cache_purge_throttle_ms`

```
Syntax:  cache_purge_throttle_ms <time>;
Default: 10ms
Minimum: 1ms
Context: http
```

Pause zwischen Hintergrund-Auftragsgruppen. Akzeptiert `10ms`, `500ms`, `1s` und zusammengesetzte Werte in Anführungszeichen wie `"2s 500ms"`. Eine bloße Ganzzahl bedeutet Millisekunden: `10` entspricht `10ms`. Ohne Einheit erscheint eine Konfigurationswarnung; eine ausdrückliche Einheit vermeidet sie.

Ein untätiger Worker prüft nach dem Zehnfachen dieses Intervalls erneut. Unvollständige Schritte und erneute Mutex-Versuche verwenden 1-ms-Timer. Timerwerte sind minimale Planungsintervalle, keine Fristen. Null und Werte oberhalb des sicheren plattformspezifischen Bereichs werden abgelehnt.

Aufnahme-, Pool- und verzögerte Löschwiederholungen werden im Abstand von 10 ms geprüft.

<a id="cache_purge_queue_status"></a>

### `cache_purge_queue_status`

```
Syntax:  cache_purge_queue_status <cache_path>;
Default: not configured
Context: location
```

Liefert für GET/HEAD einen JSON-Snapshot mit `Cache-Control: no-store`. Der Pfad muss eine konfigurierte Cache-Wurzel bezeichnen. Unbekannte Pfade verhindern das Laden der Konfiguration, auch bei deaktivierter Hintergrundverarbeitung. Den Zugriff begrenzen:

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

`queue` beschreibt die gemeinsame Warteschlange aller Cache-Pfade:

| Feld | Typ / Einheit | Bedeutung |
|---|---|---|
| `size` | Ganzzahl / Aufträge | Auf einen Worker wartende Aufträge |
| `running` | Ganzzahl / Aufträge | Zugewiesene Aufträge einschließlich Pool-Wartezeit |
| `max_size` | Ganzzahl / Aufträge | Kapazität für wartende und zugewiesene Aufträge |
| `wait_time` | Ganzzahl / Millisekunden | Alter des ältesten wartenden Auftrags; bei leerer Queue null |
| `full` | Boolean | Die abgelesene Summe wartender und zugewiesener Aufträge erreicht die Kapazität |

`cache` beschreibt die in dieser Location gewählte Cache-Wurzel:

| Feld | Typ / Einheit | Bedeutung |
|---|---|---|
| `pending.total` | Ganzzahl / Aufträge | Wartende Aufträge dieses Caches |
| `pending.full` | Ganzzahl / Aufträge | Wartende `purge_all`-Aufträge als Teil von `pending.total` |
| `active` | Ganzzahl / Aufträge | Zugewiesene Aufträge bis zum Abschluss der Abrechnung |
| `requests.overflow` | Ganzzahl / Anfragen | Anfragen mit Aufnahme-Wartezeit, einschließlich Mutex- und Allokationsdruck |
| `requests.deduplicated` | Ganzzahl / Anfragen | Mit wartender oder laufender Arbeit zusammengefasste Anfragen |
| `requests.waiting` | Ganzzahl / Anfragen | Aktuell auf Aufnahme wartende Anfragen dieses Caches |
| `files.deleted` | Ganzzahl / Dateien | Erfolgreiche Hintergrund-Unlinks, bei Durchlaufabschluss gemeldet |
| `files.errors` | Ganzzahl / Fehler | Hintergrund-Unlink-Fehler außer bereits fehlenden Dateien |
| `files.bytes` | Ganzzahl / Bytes | Geschätzte logische Größen erfolgreich entfernter Dateien |
| `responses.hit`, `responses.miss` | Ganzzahl / Hauptanfragen | In der Log-Phase beobachtete Core-HIT- und MISS-Entscheidungen |
| `responses.hit_ratio` | Zahl / 0..1 | HIT / (HIT + MISS), ohne Beobachtungen null |
| `logical.invalidations` | Ganzzahl / Entscheidungen | Veröffentlichte Generationsinvalidierungen einschließlich Duplikaten und REFRESH |
| `cleanup.scan_errors` | Ganzzahl / Fehler | Hintergrundfehler bei Metadaten, Open, Read, Close oder Speicherzuteilung |
| `cleanup.retries` | Ganzzahl / Durchläufe | Wegen Fehlern zusätzlich geplante Bereinigungsdurchläufe |
| `cleanup.exhausted` | Ganzzahl / Aufgaben | Nach drei erfolglosen Wiederholungen beendete Aufgaben |

`pending.full` zählt Gesamtcache-Operationen. Ein Platzhalterschlüssel `*` bleibt ein eigener Operationstyp. `active` enthält Pool-Wartezeit. Aktive Aufträge sind nicht in `queue.size` oder `pending.total` enthalten, belegen aber Aufnahmekapazität.

Bei zusammengefassten Namespace-Durchläufen folgt `pending.full` der Klassifikation der zuerst angenommenen Aufgabe. REFRESH-Scans/-Probes gehören weder zur PURGE-Queue-Tiefe noch zu deren Löschfehlerzählern. Rotierte Wiederholungen können bewirken, dass `wait_time` das abgetastete Alter des Kopfes statt des ältesten Eintrags angibt.

Jeder Prozess-Slot hat genau einen schreibenden Event-Loop-Thread; Anfang und Schrittweite sind an Cache-Lines ausgerichtet. Threads liefern Ergebnisse über Abschlussereignisse zurück. Statusleser verwenden weder Warteschlangen-Mutex noch atomisches Fetch-Add, auch nicht mit Inkrement null. Auf 64-Bit-Builds werden Zähler mit einfachen ausgerichteten volatile Loads und Stores gelesen und geschrieben. Auf 32-Bit-Builds schützen begrenzte Sequenzprüfungen die 64-Bit-Lesezugriffe; dort kann ein gerade beschriebener Shard in einem Sample fehlen. Felder sind Näherungswerte und keine gemeinsame Worker-Transaktion. Die Aufnahme verwendet ausschließlich gesperrten Kontrollzustand, niemals Monitoring-Werte.

Kumulative Zähler sättigen beim Maximum einer vorzeichenlosen 64-Bit-Zahl, überstehen gewöhnliche Reloads und werden beim Neustart zurückgesetzt. Der Wartezähler folgt dem Request-Cleanup. Synchrone Operationen und Löschungen des Core-Cache-Managers sind nicht in Hintergrund-Löschsummen enthalten. Bytes sind Dateilängenschätzungen, keine Dateisystemblöcke und kein Beleg für freigegebenen Plattenplatz: offene Leser und gleichzeitige Schreiber können das Ergebnis ändern. Teilergebnisse eines Durchlaufs zählen mit; Ergebnisse abgestürzter Worker können fehlen. Die Metriken sind kein Audit-Protokoll.

HIT/MISS berücksichtigt nur Hauptanfragen mit nativer Cache-Entscheidung. Fehlender Upstream, Cache-Status null sowie BYPASS, EXPIRED, STALE, UPDATING und REVALIDATED zählen nicht zu diesem Verhältnis. Normale ungecachte Anfragen werden nicht zu MISS. Ein Status-Endpunkt kann auch ohne Hintergrund-Purge eine SHM-Zone anlegen. Auf 32-Bit-Systemen gilt das Warteschlangenalter für Intervalle unter etwa 24,9 Tagen; negative vorzeichenbehaftete Zeitdifferenzen werden als null ausgegeben.

<a id="cache_purge_legacy_status"></a>

### `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

Veraltete Direktive, die nur zur Konfigurationsmigration akzeptiert wird. Beide Werte erzeugen eine Warnung und werden ignoriert; entfernen Sie die Direktive. Ungültige Werte werden abgelehnt. Kein synchroner Treffer ergibt `200 no_match`, angenommene physische Bereinigung `202 accepted`, veröffentlichte Generationsinvalidierung `200 invalidated`. Die Umschaltung zwischen `412` und `404` entfällt. Siehe [HTTP-Vertrag](../HTTP_RESPONSES.md).

<a id="cache_purge_vary_aware"></a>

### `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

Mit `on` durchsucht ein erfolgreicher exakter Purge nach dem Löschen der Primärdatei das Cache-Verzeichnis und entfernt verbleibende Dateien mit derselben `KEY:`-Zeichenfolge. Das erfasst `Vary`- und `gzip_vary`-Varianten, die an verschiedenen Pfaden liegen, aber einen logischen Schlüssel teilen. Das unmittelbar folgende Byte muss `\n` sein. Dadurch werden Schlüssel mit lediglich gemeinsamem Präfix nicht irrtümlich getroffen.

Standardmäßig deaktiviert, da jeder exakte Purge einen vollständigen Verzeichnisdurchlauf ergänzt. Für mehrere `Vary`-/`gzip_vary`-Darstellungen aktivieren. Der Durchlauf erfolgt auch bei bereits fehlendem Haupteintrag. Wildcard und `purge_all` erfassen Varianten ohnehin. Exakte Vary-Schlüssel beachten Groß-/Kleinschreibung und sind auf 511 Byte begrenzt; längere Schlüssel liefern vor jeder Löschung `400 invalid_selector`.

Die Generationsinvalidierung erfasst ohne diese Option alle Repräsentationen des logischen Schlüssels.

<a id="cache_purge_namespace"></a>

### `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

Deklariert einen Generations-Namespace vor seiner Bindung in Locations. Namen bestehen aus 1–32 ASCII-Buchstaben, Ziffern, `_` oder `-`; SHM muss mindestens acht native Seiten umfassen. Ein eigenes dauerhaftes Zustandsverzeichnis außerhalb von Cache- und Temporärpfaden verwenden. Der Core erzeugt einen persistenten exklusiven Marker für eine Neustartepoche. Diese kleinen Dateien solange aufbewahren, wie der zugehörige Cache existieren kann; das Löschen nur des Zustands kann alte Epochen wiederverwendbar machen. Stromausfall-/fsync-Dauerhaftigkeit wird nicht zugesichert. Reload bewahrt Epoche und Regeln; Größen-/Pfadänderung oder Entfernen des Namespace erfordert Neustart. Namespace-SHM und Aufgabenqueue sind getrennt dimensioniert.

<a id="cache_purge_key"></a>

### `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

Bindet die Location an einen vorhandenen Namespace und stellt `$cache_purge_key` bereit. Den vollständigen `proxy_cache_key`, `fastcgi_cache_key`, `scgi_cache_key` oder `uwsgi_cache_key` auf diese Variable setzen, auch in separaten PURGE-Locations. Weicht der ausgewertete native Schlüssel ab, endet PURGE mit `500`, statt strenge Invalidierung vorzutäuschen. `$cache_purge_key` darf nicht in seinem eigenen logischen Ausdruck vorkommen. Logische Schlüssel sind höchstens 8096 Bytes lang. Nur beteiligte Locations binden, wenn andere Legacy-Semantik brauchen; ein `off` zum Aufheben einer geerbten Bindung existiert nicht.

<a id="proxy_cache_refresh"></a>

### `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

Erzeugt einen expliziten REFRESH-Endpunkt nur für Hauptrequests; andere Methoden erhalten `405`. Er benötigt eine `cache_purge_key`-Bindung, `$cache_purge_key` als nativen Ausdruck und ein Refresh-Profil. Der logische Schlüssel wählt exakte Treffer, ein abschließendes `*` als Präfix oder `*` für den Namespace dieses Cache-Pfads. Die Location mit Core-Zugriffs-/Authentifizierungsregeln schützen. REFRESH unterstützt nur Proxy-Caches; PURGE unterstützt weiterhin alle vier Protokolle.

<a id="cache_purge_refresh_profile"></a>

### `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

Ordnet eine bekannte Repräsentation einer wörtlichen internen Proxy-Location zu. Nach Entfernen des exakten, groß-/kleinschreibungssensitiven Präfixes muss eine mit `/` beginnende Origin-URI übrig bleiben; Query und Escapes bleiben erhalten. Das Modul errät keinen Host und sucht nicht nach `://` im Schlüssel. Das Ziel benötigt `internal`, Cache aus, Weiterleitung von Request-Headern/-Body aus, HEAD, einen nichtleeren wörtlichen Host und wörtliche Repräsentationsheader. Die bedingten Header müssen exakt `$cache_purge_refresh_etag` bzw. `$cache_purge_refresh_last_modified` verwenden. `$cache_purge_refresh_uri` liefert die URI an `proxy_pass`. Diese drei seiteneffektfreien Variablen gelten nur für Probes. Preaccess prüft auch interne Weiterleitungen. Der Administrator muss die Identität von Origin-Repräsentation und Cache-Schlüssel sicherstellen; Vary-Einträge werden mangels ursprünglicher Request-Header übersprungen.

In der Probe-Location `proxy_intercept_errors off` setzen; geerbtes Intercepting wird abgelehnt, damit Origin-Fehler nicht zu invalidierendem Erfolg umgeschrieben werden.

<a id="cache_purge_refresh_timeout"></a>

### `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

Begrenzt neue Scan-/Probe-Aufträge einer REFRESH-Seite einschließlich Wartezeit auf den Worker-Scanner. Nach Ablauf startet keine neue Arbeit und die Antwort ist unvollständig. Ein laufender HEAD endet unter Core-`proxy_connect_timeout`, `proxy_send_timeout` und `proxy_read_timeout`; diese explizit setzen. Dies ist keine harte Antwortfrist und unterbricht keinen Dateisystemaufruf. Disconnect und Graceful Shutdown stoppen weitere Aufträge; Speicher aktiver Threads bleibt bis zum nativen Abschluss erhalten.

---

<a id="execution-model"></a>

## Ausführungsmodell

Ein **Auftrag** ist eine Platzhalter- oder `purge_all`-Operation. Eine **Gruppe** umfasst die Aufträge, die ein Worker vor einer Drosselungspause bearbeitet. Ein **Schritt** ist ein begrenzter Abschnitt eines inkrementellen Verzeichnisdurchlaufs; ein Auftrag kann viele Schritte benötigen.

Mit Generationsschlüsseln fasst ein physischer Durchlauf je Cache-Pfad und Namespace alle angenommenen Invalidierungen zusammen. REFRESH-Seiten sind eigene HTTP-Operationen, keine PURGE-Aufgaben.

| Anfrage und Konfiguration | Dateidurchlauf | HTTP-Abschluss |
|---|---|---|
| Legacy: exakter Schlüssel, mit oder ohne Vary-Abgleich | Anfrage-Worker; Vary ergänzt einen Verzeichnisdurchlauf | Nach der Operation |
| Platzhalter oder `purge_all`, Hintergrundwarteschlange aus | Synchroner Durchlauf im Anfrage-Worker | Nach dem Durchlauf |
| Hintergrundauftrag, ausgewählter Pool | Höchstens ein Pool-Auftrag je Worker gleichzeitig | `202` nach Aufnahme |
| Hintergrundauftrag ohne nutzbaren Thread-Pool | Inkrementelle Schritte in der Worker-Event-Loop | `202` nach Einreihung |
| Kapazität oder Speicher erschöpft | Anfrage wartet auf Aufnahme; kein eigener Dateidurchlauf | `202` nach Aufnahme |
| Generations-PURGE, jede Schlüsselform | Verzögerter Namespace-Durchlauf; kein Cache-Open/Unlink im Request | `200 invalidated` nach Veröffentlichung und Aufnahme |
| REFRESH-Endpunkt | Begrenzter Scan und bis zu 32 serielle HEAD-Probes | `200` JSON mit Abschluss- und Fortsetzungsfeldern |

Zeilen ohne Generationsschlüssel beschreiben Legacy-Verhalten. Ein auf Kapazität wartender Generationsrequest liefert nach Aufnahme `200`, nicht `202`.

Ein inkrementeller Schritt endet nach höchstens 64 Verzeichnisiterationen oder sobald 5 ms verstrichene Zeit festgestellt werden. Übersprungene Einträge und Verzeichnisenden zählen mit. Ein einzelner Dateisystem-Systemaufruf kann dadurch nicht zeitlich begrenzt werden.

Für Dateioperationen in Hintergrundthreads mit `--with-threads` bauen und einen Core-Pool im **main**-Kontext außerhalb von `http` definieren:

```nginx
thread_pool default threads=2;
```

`cache_purge_thread_pool <name>` wählt einen benannten Pool in `http`, `server` oder `location`. Standard ist `default`; wird dieser Pool verwendet, aber nicht ausdrücklich deklariert, erstellt NGINX ihn mit seinen Standardwerten. Andere Namen müssen mit der Core-Direktive `thread_pool` deklariert werden. Die Auswahl wird vererbt; bei zusammengefassten Aufträgen bestimmt die erste angenommene Anfrage den Pool. Jeder Worker besitzt eigene Core-Pools. Bei Sättigung wird die Übergabe nach 10 ms erneut versucht; der Auftrag behält seinen Kapazitätsplatz. Ein voller Pool löst keinen synchronen Durchlauf aus. Ohne einkompilierte Thread-Unterstützung, unter Windows, im Einzelprozessmodus oder nach Entfernung des Pools eines alten Auftrags bei einem Reload wird der inkrementelle Cursor verwendet.

REFRESH verwendet den gewählten Pool für höchstens einen zusätzlichen Scanner pro Worker. Native HTTP-Subrequests laufen im Event Loop; der I/O-Thread greift nie auf ihren Zustand zu.

Strukturelle Änderungen an Warteschlange und Index verwenden den Slab-Mutex. Aufnahme und Hintergrund-Callbacks versuchen `trylock` und wiederholen bei Bedarf per Timer. Wartende **und laufende** Aufträge bleiben in einem Core-String-Rbtree je Cache-Pfad; vollständige Schlüsselvergleiche lösen Hash-Kollisionen auf. Indexoperationen benötigen `O(log Q)` für `Q` ausstehende Aufträge dieses Pfads; die Pfadsuche ist linear in der Anzahl registrierter Pfade. Native `ngx_queue_t`-Listen verwalten wartende und aktive Aufträge. Das erste Duplikat eines laufenden Auftrags fordert einen Wiederholungsdurchlauf an; weitere Duplikate werden damit zusammengefasst. Nach dem aktuellen Durchlauf kommt die Wiederholung ans Warteschlangenende. Ein späterer Purge geht damit nicht stillschweigend verloren. Fortlaufend neue Purges können weitere Durchläufe auslösen. Erst der endgültige Abschluss gibt den Kapazitätsplatz frei. Dateioperationen und Antwortformatierung laufen außerhalb der Sperre.

Der Rbtree dedupliziert ausschließlich physische Löschaufgaben. Generationsregeln liegen in begrenzten Byte-Tries einer eigenen SHM-Zone. Namespace-Durchläufe und Legacy-Schlüsselaufgaben haben unterschiedliche Indexidentitäten; REFRESH-Entscheidungen nutzen denselben Löschpfad.

Bei voller Warteschlange oder fehlgeschlagener Auftragsallokation wartet die HTTP-Anfrage mit einem 10-ms-Timer auf Aufnahme. Core-Handler erkennen Verbindungsabbrüche. Anfragen und Cleanup-Handler bleiben im zuständigen Worker; SHM enthält keine Anfragezeiger. Das Lesen der gesamten HTTP/2- oder HTTP/3-Verbindung wird nicht abgeschaltet. Warteschlangendruck erzeugt keine 429/503-Antworten; ein bereits eingelesener kurzer PURGE erzeugt nicht zwingend ein TCP Zero Window. Wartende Anfragen benötigen weiterhin Speicher und Verbindungen. Core-Zeitlimits und Aufnahmerichtlinien müssen zum Verbindungsbudget passen.

Angenommene Aufträge behalten Schlüssel, Pool-Namen und Schutzregeln für temporäre Pfade über gewöhnliche Reloads. Ein neueres Duplikat aktualisiert die Schutzregeln eines wartenden Auftrags oder seines nächsten Wiederholungsdurchlaufs. Das Abschalten neuer Hintergrundaufträge verwirft bereits angenommene Arbeit nicht. Ein Worker im Graceful Shutdown beendet den aktuellen Durchlauf und übernimmt keinen neuen Auftrag. Beim schnellen Beenden kommt ein unvollständiger inkrementeller Auftrag zurück in die Warteschlange; der Exit-Hook führt keinen vollständigen Dateidurchlauf aus. Für bereits übergebene Thread-Aufträge verwaltet der Core beim Abbau des Pools die Lebensdauer.

PID und Slot-Epoche kennzeichnen zugewiesene Aufträge. Worker prüfen bei der Wiederherstellung im Sekundenabstand höchstens 64 aktive Einträge und reihen Aufträge beendeter Besitzer erneut ein. Das stellt verwaiste Zuweisungen wieder her, ist aber kein transaktionales Journal. Ein Absturz während einer Listen- oder Slab-Änderung kann weiterhin Zustand beschädigen und einen Neustart erfordern. Unmittelbar vor dem Absturz ausgeführte Löschungen können in den Metriken fehlen. Wartende Aufträge und Wiederholungen benötigen einen laufenden Worker. Wenn vollständige Ausführung erforderlich ist, die Warteschlange vor dem Stoppen leeren lassen. Sie ist nicht dauerhaft gespeichert.

Das interne Warteschlangenlayout ist keine Upgrade-ABI. Beim Wechsel auf einen Build mit anderem Layout einen vollständigen Neustart durchführen und vorher nötigenfalls angenommene Aufträge abarbeiten lassen. Gewöhnliche Reloads desselben Builds verwenden die Zone erneut.

Löschen erfolgt nach bestem Bemühen. `202` bestätigt die Annahme, nicht die abgeschlossene Löschung. Ein beendeter Durchlauf ist kein atomarer Cache-Snapshot. Gleichzeitige Anfragen können während oder nach dem Durchlauf neue Einträge erzeugen; Dateifehler werden protokolliert. Platzhalter- und Gesamtdurchläufe entfernen Dateien, ohne jeden nativen Cache-Knoten synchron zu aktualisieren. Exakte und Vary-Operationen invalidieren passende Knoten.

Fehlgeschlagene Hintergrunddurchläufe werden nach 100 ms, 1 s und 10 s höchstens dreimal wiederholt. Wartende Wiederholungen behalten ihren Deduplizierungs-/Aufnahmeplatz, lassen aber andere Aufgaben passieren. Ein neuer PURGE setzt das Wiederholungsfenster zurück. Danach wird die Aufgabe mit Fehlermeldung beendet; `cleanup.exhausted` prüfen und nach Behebung des Speicherfehlers erneut purgen. Eine Generationsinvalidierung bleibt auch bei fehlgeschlagener physischer Bereinigung wirksam. Fehlende Dateien gelten als erwartete Rennen; ungültige Dateien werden übersprungen.

---

<a id="performance-tuning"></a>

## Leistungsabstimmung

Die drei Parameter steuern unterschiedliche Grenzen: `queue_size` begrenzt ausstehende Arbeit, `batch_size` gruppiert Aufträge pro Worker und `throttle_ms` legt Gruppenpausen fest. Anfragelatenz, Speicherkonkurrenz und Warteschlangenalter vor und nach Änderungen messen. Mehr Queue-Kapazität erhöht nicht den Abarbeitungsdurchsatz.

Diese Vorgaben sind Ausgangskonfigurationen, keine Aussagen zur Hardwareleistung. Die Spaltennamen lassen das gemeinsame Präfix `cache_purge_` weg.

| Last / Speicherbudget | `queue_size` | `batch_size` | `throttle_ms` |
|---|---:|---:|---:|
| Begrenzter Host oder Datenträger | 512 | 5 | 25ms |
| Allgemeiner Ausgangspunkt | 2048 | 20 | 10ms |
| Schneller Speicher mit gemessener Reserve | 8192 | 50 | 5ms |
| Kurze Lastspitzen bei vorsichtiger Hintergrundverarbeitung | 8192 | 5 | 50ms |

Die allgemeine Vorgabe lautet beispielsweise:

```nginx
cache_purge_background_queue on;
cache_purge_queue_size        2048;
cache_purge_batch_size        20;
cache_purge_throttle_ms       10ms;
```

Eine größere Queue kann Lastspitzen aufnehmen; dauerhaft höhere Ankunfts- als Abschlussraten füllen sie dennoch. Zusätzlichen Bedarf näherungsweise als `max(0, arrival_rate - completion_rate) * burst_seconds` Aufträge berechnen, anhand gemessener Aufträge/s nach Deduplizierung. Reserve für unterschiedliche Auftragsdauer und vorhandenen Rückstau lassen. Aufnahme-Wartezeit erhöht offene Anfragen und Verbindungen; bei gesättigtem Speicher deren RAM- und FD-Budget berücksichtigen.

<a id="memory-sizing"></a>

### Speicherbemessung

Die konfigurierte Zonengröße wird aus den nativen Strukturgrößen berechnet:

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

| Bestandteil | Reservierter oder belegter Speicher |
|---|---|
| Auftragsreserve | `Q * S`, einschließlich Slab-Rundung sowie eingebetteter Schlüssel und Pool-Namen |
| Metriken eines Pfads | `R * NGX_MAX_PROCESSES` plus Ausrichtung; 192 KiB bei 192 Bytes Schrittweite und 1024 Slots |
| Queue-Steuerung | `H`, einschließlich Prozessbesitzer- und Epochenarrays |
| Pfadreserve | `M`, einschließlich Namen, Rbtree-Wurzeln, Ausrichtung und Metrikarrays |
| Mindestzone | Acht native Speicherseiten |

Für das gemessene LP64-Layout (`I=184`, `H=20576`, Pfaddatensatz 104 Bytes, Statistikdatensatz 144 Bytes), mit 64-Byte-Cache-Line, 4-KiB-Seiten und einem 20 Byte langen Cache-Pfad ergibt die Formel:

| Kapazität | Zone in Bytes | Zone in MiB |
|---:|---:|---:|
| 512 | 1654784 | 1.58 |
| 2048 | 5373952 | 5.12 |
| 8192 | 20242432 | 19.30 |

Der tatsächliche Aufgabenbedarf beträgt vor Slab-Rundung `I + index_bytes + 1 + pool_name_bytes`. Der eingebettete Index enthält ein Aktionsbyte plus Legacy-Schlüssel oder einen Namespace-Namen für den Generationsdurchlauf. Legacy-`purge_all` speichert nur das Aktionsbyte. Die Kapazität begrenzt wartende und aktive Zuteilungen zusammen. Native Größen, Cache-Line-Größe, `NGX_MAX_PROCESSES` und konfigurierte Pfade bestimmen den Bedarf; Windows- und 32-Bit-Layouts unterscheiden sich. Schutzdatensätze bei Reload verbrauchen weiteres SHM. Speicher kann vor Erreichen des Aufgabenlimits ausgehen.

Prozessspeicher wird separat benötigt. Jeder aktive Durchlauf verwendet einen festen Schlüsselpuffer, einen wiederverwendbaren Pfadpuffer und einen festen Frame je offenem Verzeichnis. Frames speichern Präfixlängen, ohne Vorfahrenpfade zu kopieren. Der Bedarf ist damit `O(depth + longest_path)`, unabhängig von der Dateianzahl. Verzeichnisstreams besitzen Plattformpuffer. Wartende Anfragen halten nur einen kleinen Timer-/Cleanup-Kontext ohne Durchlaufcursor. Thread-Stacks und Core-Pools benötigen weiteren Speicher.

Namespace-SHM hat eine separat konfigurierte Größe. Ein REFRESH-Scanner benötigt im gemessenen LP64-Layout einen festen Kandidatenpuffer/-kontext von 283864 Bytes (32 Einträge), Traversierungsrahmen und höchstens 32 Core-Subrequests im HTTP-Request-Pool. Der Seitenabschluss gibt den Request frei; die Fortsetzung beginnt einen neuen. Weitere Wartende reservieren kein Kandidatenarray.

<a id="throughput-and-latency"></a>

### Durchsatz und Latenz

Für einen ständig ausgelasteten Worker ist folgende Näherung hilfreich:

```text
B = tasks completed in a batch
T = throttle delay in milliseconds
D = elapsed batch processing time in milliseconds, including slice waits
rate_per_worker ~= 1000 * B / (D + T) tasks/s
```

Ohne `D` ergibt sich die reine Planungsobergrenze `1000 * B / T`: mit Standardwerten 1000 Aufträge/s je Worker. Das ist weder eine Dateilöschrate noch ein Messergebnis. Dauern zehn Aufträge insgesamt 200 ms, ergeben dieselben 10 ms Pause etwa 47,6 Aufträge/s. Worker teilen sich den Datenträger; eine Multiplikation mit ihrer Anzahl ist keine Durchsatzgarantie. Ein untätiger Worker benötigt bis zu `10 * T` zuzüglich Event-Loop-Verzögerung, bevor er neue Arbeit prüft.

Kleinere Gruppen oder längere Pausen reduzieren Arbeit zwischen Ruhephasen. Für geringere Worker-Latenz bei Datei-I/O den optionalen Pool verwenden. Kein Parameter begrenzt einen einzelnen Systemaufruf oder zerlegt einen Thread-Auftrag in Gruppen.

---

<a id="monitoring-and-debugging"></a>

## Monitoring und Debugging

Der [Status-Endpunkt](#cache_purge_queue_status) zeigt wartende und aktive Aufträge, Aufnahme-Wartezeit, Duplikate, Unlink-Ergebnisse und HIT/MISS. `202 Accepted` bestätigt Aufnahme oder Zusammenfassung, nicht die Löschung oder sofortige logische Invalidierung. Anfragelatenz gemeinsam mit Queue-Alter und Datenträgerkonkurrenz beobachten.

| Stufe | Bedingung |
|---|---|
| `warn` | Zu langer Schlüssel, entfernter Auftrags-Pool oder begrenzter Konfigurationswert |
| `error` | Zugriff verweigert, Cache-Suche oder REFRESH-Profil fehlgeschlagen, Wiederholungen erschöpft; Core-Pool- oder Windows-Lesefehler |
| `crit` | Öffnen, Metadatenabfrage, Löschen oder Verzeichnisoperation fehlgeschlagen; POSIX-Core-Lesefehler |
| `alert` | Datei konnte nicht geschlossen werden |
| `emerg` | Ungültige Konfiguration, inkompatibles SHM, Größenänderung oder Metadatenallokation bei Reload fehlgeschlagen |
| `debug` | Zusätzlicher Anfrage- und Durchlaufkontext |

Moduldiagnosen verwenden die nativen NGINX-Logging-Funktionen und beginnen mit `ngx_cache_purge: `. NGINX ergänzt Schweregrad, Zeitstempel, Prozess-/Thread-IDs, errno-Text und Anfrage- oder Konfigurationskontext für die konfigurierte Ausgabe. Dateisystemmeldungen nennen die tatsächlich fehlgeschlagene Operation und den Pfad in Anführungszeichen; Namespace- und REFRESH-Fehler nennen das betroffene Objekt und den Ablehnungsgrund. Für die Erfassung einen standardmäßigen NGINX-Error-Log-Parser verwenden und nach dem Meldungspräfix filtern.

Debug-Meldungen verwenden `NGX_LOG_DEBUG_HTTP` und benötigen sowohl einen Build mit `--with-debug` als auch `error_log /var/log/nginx/error.log debug;`.

Warteschlangendruck erscheint in den Aufnahmemetriken, nicht als Warnung bei jedem Versuch. Bereits verschwundene Dateien sind ein erwarteter Wettlauf und erzeugen keine Moduldiagnose. Meldungen der NGINX-Core-Funktionen, darunter Konfigurationsparser, Speicherverwaltung, I/O und Thread-Pools, behalten das Core-Format; zur Fehlersuche das vollständige Protokoll prüfen.

```bash
tail -f /var/log/nginx/error.log | grep 'ngx_cache_purge:'
```

Geprüfte Diagnosen stehen unter [Logging-Prüfungen](../../t/TESTING.md#logging-contracts).

---

<a id="troubleshooting"></a>

## Fehlerbehebung

| Symptom | Prüfung oder Anpassung |
|---|---|
| Hohe Worker-Latenz bei großen Durchläufen | Hintergrundqueue und optionalen I/O-Pool aktivieren. Exakte/Vary-Operationen sowie Operationen ohne Hintergrundqueue bleiben synchron. |
| Mehr Aufnahme-Warteanfragen | Ankunftsrate, Queue-Alter, Pool-Kapazität und Plattenlast prüfen. Mehr Kapazität puffert Spitzen; Größenänderung erfordert Neustart. |
| Hohes iowait | `cache_purge_batch_size` reduzieren, `cache_purge_throttle_ms` erhöhen und Purge-Zufluss begrenzen. |
| Langsame Abarbeitung | Auftragsdauer und Datenträgerkonkurrenz messen, bevor Gruppen vergrößert oder Pausen verkürzt werden. |
| Client erwartet die bisherigen `412`/`404` | `cache_purge_legacy_status` entfernen; `200` mit `Cache-Purge-Code: no_match` auswerten. |
| Vary-Varianten bleiben bestehen | `cache_purge_vary_aware` aktivieren; Primär-Purge und 511-Byte-Grenze prüfen. |
| Angenommene Arbeit erscheint unvollständig | Aktive Aufträge, Wiederherstellung und Dateifehler prüfen. `202` ist kein Abschlussbeleg; vermutete SHM-Beschädigung erfordert Neustart. |

---

<a id="testing"></a>

## Tests

Die [Testübersicht](../TEST_COVERAGE.md) (Englisch) zeigt geprüfte Fälle und verbleibende Lücken.

Gesamte Suite in Docker bauen und ausführen:

```sh
make test-all
NGINX_VERSION=1.31.6 make test-all
```

Unter `t/` bleiben die vorhandenen Compose-Dienste verfügbar:

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

Mit dem Compose-Plugin `docker compose` statt `docker-compose` verwenden. Die Makefiles erkennen beide Befehle. `nginx-test` und `make test-all` führen alle sieben `t/*.t`-Suiten über denselben `prove`-Einstieg wie CI aus. Das Integrationsimage enthält native Hilfs-/Layoutprüfungen und Laufzeittests für die Pool-Ausführung. Strikte Builds ohne Threads prüfen die inkrementelle Ausführung. Jeder fehlgeschlagene Test lässt den Befehl fehlschlagen.

Strenge Builds, Sanitizer, Valgrind, Plattformjobs und Berichte beschreibt [`t/TESTING.md`](../../t/TESTING.md). Alle GitHub-Jobs stehen in `ci.yml`.

---

<a id="migration"></a>

## Migration

<a id="queue-and-status-changes"></a>

### Änderungen an Queue und Status

Änderungen am SHM-Layout erfordern einen vollständigen Neustart. Methoden-/ACL-Syntax und Protokollhandler bleiben erhalten. v4 verwendet standardmäßig JSON, übereinstimmende Ergebniskennungen in Header und Antwortkörper sowie `200 no_match` bei fehlendem Eintrag. `cache_purge_legacy_status` erzeugt nur eine Warnung. Passen Sie Clients an den [HTTP-Vertrag](../HTTP_RESPONSES.md) an. Die Kapazität umfasst zugewiesene Aufgaben; Überlast wartet auf Zulassung. Aktive Duplikate können einen weiteren Durchlauf vormerken.

Status ergänzt Felder und erhält vorhandene Feldnamen. `requests.overflow` zählt nun Aufnahme-Warteanfragen. Metriken sind Näherungen mit kumulativen 64-Bit-Zählern. Ein Thread-Build registriert `default` automatisch bei Verwendung; für isoliertes Purge-I/O einen benannten Pool wählen. Auch reine Statuskonfigurationen allokieren SHM.

Generationsinvalidierung und REFRESH werden in v4.0 explizit aktiviert. `cache_purge_key` binden und `$cache_purge_key` in jeder beteiligten Location als vollständigen nativen Cache-Schlüssel verwenden. Dies ändert Dateinamen; vorhandene Legacy-Dateien sind kalt und bleiben bis zur Core-Ablaufbereinigung oder expliziten Legacy-Löschung. Zustandsmarker beim Cache aufbewahren. Bestehende PURGE-Locations erhalten kein REFRESH. Nutzer des experimentellen PR60 müssen neuen Endpunkt, Profil und Seitennavigation konfigurieren; dessen unbegrenzte Antwort, erratene URI-/Host-Zuordnung und implizite Methodenerweiterung sind keine Kompatibilitätsgarantien.

<a id="from-the-original-frickle-module"></a>

### Vom ursprünglichen FRiCKLE-Modul

Methoden-/Zugriffslisten- und Zone/Schlüssel-Formen bleiben verfügbar. Fehlende Einträge liefern `200 no_match`; entfernen Sie `cache_purge_legacy_status` und aktualisieren Sie Clients. Hintergrundverarbeitung aktivieren Sie mit:

```nginx
http {
    cache_purge_background_queue on;
}
```

<a id="from-nginx-selective-cache-purge-module"></a>

### Von nginx-selective-cache-purge-module

Das Modul ersetzen. Die Hintergrundqueue ist jetzt integriert. Bisherige Queue-Parameter auf `cache_purge_queue_size`, `cache_purge_batch_size` und `cache_purge_throttle_ms` abbilden.

---

<a id="temporary-directories"></a>

## Temporäre Verzeichnisse

Durchläufe überspringen konfigurierte temporäre Unterverzeichnisse der Cache-Wurzel anhand vollständiger Pfadkomponenten. Namen wie `proxy_temp` sind nicht reserviert, wenn sie nicht als temporärer Pfad konfiguriert sind. Nur reguläre Dateien mit genau 32 hexadezimalen Zeichen im Dateinamen sind Löschkandidaten. Numerische und mit Suffix versehene temporäre Cache-Schreibdateien werden übersprungen, auch bei `use_temp_path=off`.

Jeder Durchlauf prüft außerdem native Header-Version, Offsets, vollständigen `KEY: `-Marker, CRC32 und Schlüssel-/Varianten-Dateihash bei maximal 8192 Schlüsselbytes. Ungültige, verkürzte, fremde oder nicht passende Dateien werden auch bei purge-all übersprungen. ETag-/Vary-Längen genau am Core-Maximum sind erlaubt. Formatprüfung authentifiziert keine bösartige Datei.

Der konfigurierte Cache-Root darf ein Symlink oder eine Windows-Junction sein. Untergeordnete Symlinks/Reparse Points werden übersprungen. Auf POSIX mit vollständigem Feature-Probe nutzen Kindverzeichnisse und Dateien Core-`ngx_openat_file`/`ngx_file_at_info` mit No-follow-Flags; gelöscht wird relativ zum gehaltenen Elternverzeichnis. Das verhindert Umleitung durch ausgetauschte untergeordnete Symlinks, ist aber keine Sandbox gegen das Verschieben bereits offener Verzeichnisse. Windows und ältere Unix-Systeme verwenden Core-Vollpfadoperationen mit Linkprüfung und behalten ein Pfadaustauschrennen. Auch Legacy-Exact-Lookup/-Löschung verwendet Vollpfade. Cache-Wurzeln privat halten; keine nicht vertrauenswürdigen Schreiber zulassen.

Konfigurierte Pfade einheitlich und kanonisch schreiben. Vergleiche lösen keine Symlink-, Mount- oder Unicode-Aliase auf. Wartende Aufträge behalten ihren Schutzsatz; neuere Duplikate aktualisieren wartende Arbeit oder die nächste Wiederholung. Ein laufender Durchlauf behält seinen bisherigen Satz. Es handelt sich nicht um die Vereinigung aller lebenden Worker-Generationen. Betroffene Pfade während wartender oder aktiver Aufträge nicht umwidmen.

---

<a id="security"></a>

## Sicherheit

- Purge-Endpunkte mit `allow` / `deny` oder vorgelagerter Authentifizierung begrenzen. Niemals öffentlich freigeben.
- `cache_purge_queue_size` passend zur Last wählen, um SHM-Verbrauch bei Anfragefluten zu begrenzen.
- Purge-Locations gegebenenfalls mit `limit_req` unabhängig vom normalen Verkehr drosseln.

---

<a id="license"></a>

## Lizenz

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

## Weitere Informationen

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [nginx documentation](https://nginx.org/en/docs/)
