[ngx_cache_purge](README.de.md)

<a id="directives"></a>

# Direktiven

<a id="proxy_cache_purge"></a>

## `proxy_cache_purge`

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

## `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Entspricht `proxy_cache_purge` für FastCGI-Zonen, die mit `fastcgi_cache` / `fastcgi_cache_path` konfiguriert sind.

<a id="scgi_cache_purge"></a>

## `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Entspricht `proxy_cache_purge` für SCGI-Zonen, die mit `scgi_cache` / `scgi_cache_path` konfiguriert sind.

<a id="uwsgi_cache_purge"></a>

## `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Entspricht `proxy_cache_purge` für uWSGI-Zonen, die mit `uwsgi_cache` / `uwsgi_cache_path` konfiguriert sind.

<a id="cache_purge_response_type"></a>

## `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

Wählt das Format erfolgreicher PURGE-Antworten; Standard ist JSON. `Cache-Purge-Code` und der Ergebniscode im Antwortkörper stimmen überein. Modulantworten enthalten `Cache-Control: no-store`; Fehler verwenden `application/problem+json` nach RFC 9457. Textschlüssel werden maskiert, binäre Schlüssel verlustfrei als Base64 mit Kodierungsangabe ausgegeben.

`none` unterdrückt den PURGE-Antwortkörper: abgeschlossene Erfolge liefern `204`, angenommene Hintergrundarbeit weiterhin `202`, Fehler weiterhin 4xx/5xx. Status und REFRESH behalten strukturiertes JSON. Siehe [HTTP-Vertrag](../HTTP_RESPONSES.md) für Codes, Felder und Grenzen.

<a id="cache_purge_background_queue"></a>

## `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

Aktiviert verzögerte Platzhalter- und `purge_all`-Operationen. Nach Aufnahme oder Zusammenfassung mit einem identischen wartenden oder laufenden Auftrag wird `202 Accepted` zurückgegeben. Exakte Purges bleiben unverändert.

Dieser Schalter steuert Legacy-Wildcard-/All-Verhalten. Eine `cache_purge_key`-Bindung nutzt immer verzögerte physische Bereinigung und logische `200`-Antworten, auch bei ausgeschaltetem Schalter.

Bei voller Warteschlange oder fehlgeschlagener Auftragsallokation bleibt die Anfrage bis zur Aufnahme offen. Während des Wartens findet kein eigener Verzeichnisdurchlauf statt. Duplikate wartender oder laufender Aufträge können auch bei voller Kapazität angenommen werden. Kann der kleine Wartezustand der Anfrage nicht allokiert werden, folgt `500`. Beim Verbindungsabbruch entfernt Cleanup den Timer und verringert die Warteanzeige.

Für diesen Schalter und `cache_purge_vary_aware` gilt der zuletzt deklarierte Wert. Ungültige Werte werden abgelehnt. `cache_purge_legacy_status` ist veraltet und wird ignoriert. Threads und Lebenszyklus: [Ausführungsmodell](../ARCHITECTURE.md#execution-model).

<a id="cache_purge_queue_size"></a>

## `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

Maximale Zahl ausstehender Aufträge über alle Cache-Pfade. Wartende und zugewiesene Aufträge belegen Kapazität. Duplikate verwenden eine Allokation gemeinsam; eine Wiederholung behält Allokation und Kapazitätsplatz. Unterschiedliche Aufträge warten auf einen freien Platz.

Die gemeinsame Zone entsteht, sobald Hintergrundverarbeitung oder ein Status-Endpunkt konfiguriert ist. Kapazitätsänderungen erfordern einen Neustart, auch wenn sie auf dieselbe Zonengröße gerundet werden. Reload lehnt Größenänderungen ab, um angenommene Arbeit zu erhalten. Zusätzliche Cache-Wurzeln können ebenfalls mehr Zonenspeicher und einen Neustart erfordern. Wichtige wartende und aktive Arbeit vorher abarbeiten lassen.

Auch die Deklaration eines Namespace erzeugt die Aufgabenwarteschlange.

Die Zonengröße umfasst Auftragsspeicher, Pfadmetadaten und ausgerichtete Worker-Metriken. Sie hängt vom Build und der Anzahl der Cache-Pfade ab; siehe [Speicherbemessung](../OPERATIONS.md#memory-sizing).

<a id="cache_purge_thread_pool"></a>

## `cache_purge_thread_pool`

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

## `cache_purge_batch_size`

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

## `cache_purge_throttle_ms`

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

## `cache_purge_queue_status`

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

`namespaces` enthält die konfigurierten `cache_purge_namespace`-Einträge unabhängig vom Cache dieser Location: Alle Cache-Pfade eines Namensraums teilen dessen Generationszustand. Jeder Eintrag wird unter der Sperre seines Namensraums kopiert. Die Einträge werden einzeln erfasst; die Liste ist kein globaler Snapshot.

| Feld | Typ / Einheit | Bedeutung |
|---|---|---|
| `name` | Zeichenkette | Name des Namensraums im Cache-Schlüsselpräfix `p4:<name>:` |
| `epoch` | Zeichenkette / 10 Ziffern | Aktuelle Epoche des Namensraums; ein vollständiger Neustart erzeugt eine neue |
| `baseline` | Zeichenkette / 16 Hexadezimalziffern in Kleinschreibung | Generation der Schlüssel ohne Erhöhung durch eine Regel; eine Invalidierung des gesamten Namensraums setzt sie auf `serial` |
| `serial` | Zeichenkette / 16 Hexadezimalziffern in Kleinschreibung | Bei 1 beginnende Sequenznummer, die mit jeder veröffentlichten Invalidierung dieser Epoche steigt |
| `rules` | Boolesch | `true`, wenn exakte oder Präfixregeln existieren; eine Invalidierung des gesamten Namensraums entfernt sie, auch wenn Speichermangel im SHM die Invalidierung ausweitet |

`baseline` und `serial` haben dieselbe feste Breite wie die Generation im Cache-Schlüssel und lassen sich als Zeichenketten vergleichen. `namespaces_complete` ist `false`, wenn die Liste für die Antwort gekürzt wurde. Zurückgegebene Einträge bleiben verwendbar; ein fehlender Namensraum gilt als unbekannt. Siehe [Cache-Dateien im Generationsmodus lesen](OPERATIONS.de.md#reading-cache-files).

`pending.full` zählt Gesamtcache-Operationen. Ein Platzhalterschlüssel `*` bleibt ein eigener Operationstyp. `active` enthält Pool-Wartezeit. Aktive Aufträge sind nicht in `queue.size` oder `pending.total` enthalten, belegen aber Aufnahmekapazität.

Bei zusammengefassten Namespace-Durchläufen folgt `pending.full` der Klassifikation der zuerst angenommenen Aufgabe. REFRESH-Scans/-Probes gehören weder zur PURGE-Queue-Tiefe noch zu deren Löschfehlerzählern. Rotierte Wiederholungen können bewirken, dass `wait_time` das abgetastete Alter des Kopfes statt des ältesten Eintrags angibt.

Jeder Prozess-Slot hat genau einen schreibenden Event-Loop-Thread; Anfang und Schrittweite sind an Cache-Lines ausgerichtet. Threads liefern Ergebnisse über Abschlussereignisse zurück. Statusleser verwenden weder Warteschlangen-Mutex noch atomisches Fetch-Add, auch nicht mit Inkrement null. Auf 64-Bit-Builds werden Zähler mit einfachen ausgerichteten volatile Loads und Stores gelesen und geschrieben. Auf 32-Bit-Builds schützen begrenzte Sequenzprüfungen die 64-Bit-Lesezugriffe; dort kann ein gerade beschriebener Shard in einem Sample fehlen. Felder sind Näherungswerte und keine gemeinsame Worker-Transaktion. Die Aufnahme verwendet ausschließlich gesperrten Kontrollzustand, niemals Monitoring-Werte.

Kumulative Zähler sättigen beim Maximum einer vorzeichenlosen 64-Bit-Zahl, überstehen gewöhnliche Reloads und werden beim Neustart zurückgesetzt. Der Wartezähler folgt dem Request-Cleanup. Synchrone Operationen und Löschungen des Core-Cache-Managers sind nicht in Hintergrund-Löschsummen enthalten. Bytes sind Dateilängenschätzungen, keine Dateisystemblöcke und kein Beleg für freigegebenen Plattenplatz: offene Leser und gleichzeitige Schreiber können das Ergebnis ändern. Teilergebnisse eines Durchlaufs zählen mit; Ergebnisse abgestürzter Worker können fehlen. Die Metriken sind kein Audit-Protokoll.

HIT/MISS berücksichtigt nur Hauptanfragen mit nativer Cache-Entscheidung. Fehlender Upstream, Cache-Status null sowie BYPASS, EXPIRED, STALE, UPDATING und REVALIDATED zählen nicht zu diesem Verhältnis. Normale ungecachte Anfragen werden nicht zu MISS. Ein Status-Endpunkt kann auch ohne Hintergrund-Purge eine SHM-Zone anlegen. Auf 32-Bit-Systemen gilt das Warteschlangenalter für Intervalle unter etwa 24,9 Tagen; negative vorzeichenbehaftete Zeitdifferenzen werden als null ausgegeben.

<a id="cache_purge_legacy_status"></a>

## `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

Veraltete Direktive, die nur zur Konfigurationsmigration akzeptiert wird. Beide Werte erzeugen eine Warnung und werden ignoriert; entfernen Sie die Direktive. Ungültige Werte werden abgelehnt. Kein synchroner Treffer ergibt `200 no_match`, angenommene physische Bereinigung `202 accepted`, veröffentlichte Generationsinvalidierung `200 invalidated`. Die Umschaltung zwischen `412` und `404` entfällt. Siehe [HTTP-Vertrag](../HTTP_RESPONSES.md).

<a id="cache_purge_vary_aware"></a>

## `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

Mit `on` durchsucht ein erfolgreicher exakter Purge nach dem Löschen der Primärdatei das Cache-Verzeichnis und entfernt verbleibende Dateien mit derselben `KEY:`-Zeichenfolge. Das erfasst `Vary`- und `gzip_vary`-Varianten, die an verschiedenen Pfaden liegen, aber einen logischen Schlüssel teilen. Das unmittelbar folgende Byte muss `\n` sein. Dadurch werden Schlüssel mit lediglich gemeinsamem Präfix nicht irrtümlich getroffen.

Standardmäßig deaktiviert, da jeder exakte Purge einen vollständigen Verzeichnisdurchlauf ergänzt. Für mehrere `Vary`-/`gzip_vary`-Darstellungen aktivieren. Der Durchlauf erfolgt auch bei bereits fehlendem Haupteintrag. Wildcard und `purge_all` erfassen Varianten ohnehin. Exakte Vary-Schlüssel beachten Groß-/Kleinschreibung und sind auf 511 Byte begrenzt; längere Schlüssel liefern vor jeder Löschung `400 invalid_selector`.

Die Generationsinvalidierung erfasst ohne diese Option alle Repräsentationen des logischen Schlüssels.

<a id="cache_purge_namespace"></a>

## `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

Deklariert einen Generations-Namespace vor seiner Bindung in Locations. Namen bestehen aus 1–32 ASCII-Buchstaben, Ziffern, `_` oder `-`; SHM muss mindestens acht native Seiten umfassen. Ein eigenes dauerhaftes Zustandsverzeichnis außerhalb von Cache- und Temporärpfaden verwenden. Der Core erzeugt einen persistenten exklusiven Marker für eine Neustartepoche. Diese kleinen Dateien solange aufbewahren, wie der zugehörige Cache existieren kann; das Löschen nur des Zustands kann alte Epochen wiederverwendbar machen. Stromausfall-/fsync-Dauerhaftigkeit wird nicht zugesichert. Reload bewahrt Epoche und Regeln; Größen-/Pfadänderung oder Entfernen des Namespace erfordert Neustart. Namespace-SHM und Aufgabenqueue sind getrennt dimensioniert.

<a id="cache_purge_key"></a>

## `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

Bindet die Location an einen vorhandenen Namespace und stellt `$cache_purge_key` bereit. Den vollständigen `proxy_cache_key`, `fastcgi_cache_key`, `scgi_cache_key` oder `uwsgi_cache_key` auf diese Variable setzen, auch in separaten PURGE-Locations. Weicht der ausgewertete native Schlüssel ab, endet PURGE mit `500`, statt strenge Invalidierung vorzutäuschen. `$cache_purge_key` darf nicht in seinem eigenen logischen Ausdruck vorkommen. Logische Schlüssel sind höchstens 8096 Bytes lang. Nur beteiligte Locations binden, wenn andere Legacy-Semantik brauchen; ein `off` zum Aufheben einer geerbten Bindung existiert nicht.

<a id="proxy_cache_refresh"></a>

## `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

Erzeugt einen expliziten REFRESH-Endpunkt nur für Hauptrequests; andere Methoden erhalten `405`. Er benötigt eine `cache_purge_key`-Bindung, `$cache_purge_key` als nativen Ausdruck und ein Refresh-Profil. Der logische Schlüssel wählt exakte Treffer, ein abschließendes `*` als Präfix oder `*` für den Namespace dieses Cache-Pfads. Die Location mit Core-Zugriffs-/Authentifizierungsregeln schützen. REFRESH unterstützt nur Proxy-Caches; PURGE unterstützt weiterhin alle vier Protokolle.

<a id="cache_purge_refresh_profile"></a>

## `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

Ordnet eine bekannte Repräsentation einer wörtlichen internen Proxy-Location zu. Nach Entfernen des exakten, groß-/kleinschreibungssensitiven Präfixes muss eine mit `/` beginnende Origin-URI übrig bleiben; Query und Escapes bleiben erhalten. Das Modul errät keinen Host und sucht nicht nach `://` im Schlüssel. Das Ziel benötigt `internal`, Cache aus, Weiterleitung von Request-Headern/-Body aus, HEAD, einen nichtleeren wörtlichen Host und wörtliche Repräsentationsheader. Die bedingten Header müssen exakt `$cache_purge_refresh_etag` bzw. `$cache_purge_refresh_last_modified` verwenden. `$cache_purge_refresh_uri` liefert die URI an `proxy_pass`. Diese drei seiteneffektfreien Variablen gelten nur für Probes. Preaccess prüft auch interne Weiterleitungen. Der Administrator muss die Identität von Origin-Repräsentation und Cache-Schlüssel sicherstellen; Vary-Einträge werden mangels ursprünglicher Request-Header übersprungen.

In der Probe-Location `proxy_intercept_errors off` setzen; geerbtes Intercepting wird abgelehnt, damit Origin-Fehler nicht zu invalidierendem Erfolg umgeschrieben werden.

<a id="cache_purge_refresh_timeout"></a>

## `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

Begrenzt neue Scan-/Probe-Aufträge einer REFRESH-Seite einschließlich Wartezeit auf den Worker-Scanner. Nach Ablauf startet keine neue Arbeit und die Antwort ist unvollständig. Ein laufender HEAD endet unter Core-`proxy_connect_timeout`, `proxy_send_timeout` und `proxy_read_timeout`; diese explizit setzen. Dies ist keine harte Antwortfrist und unterbricht keinen Dateisystemaufruf. Disconnect und Graceful Shutdown stoppen weitere Aufträge; Speicher aktiver Threads bleibt bis zum nativen Abschluss erhalten.
