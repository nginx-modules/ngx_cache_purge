# ngx_cache_purge

[README](README.de.md) · [Direktiven](DIRECTIVES.de.md)

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

<a id="reading-cache-files"></a>

### Cache-Dateien im Generationsmodus lesen

Ein Cache-Browser, Preloader oder Nutzungsbericht kann die Generationsgültigkeit nicht allein aus Dateien ableiten: Nach einer Invalidierung können veraltete Dateien ohne Ersatz verbleiben. Die `KEY:`-Zeile einer Generationsdatei lautet `p4:<namespace>:<epoch>:<generation>:<logical key>`. Der [Status-Endpunkt](DIRECTIVES.de.md#cache_purge_queue_status) liefert den Namensraumzustand. Die aktuelle Generation eines Schlüssels ist `baseline`, sofern keine exakte oder Präfixregel sie erhöht hat; diese Regeln fehlen in der Antwort.

| Zustand des Namensraums der Datei | Ergebnis |
|---|---|
| Namensraum fehlt in der Liste | `unknown` |
| Epoche der Datei weicht von `epoch` ab | `stale` |
| Epoche stimmt überein; Dateigeneration liegt unter `baseline` | `stale` |
| Epoche stimmt überein; Generation ist mindestens `baseline`; `rules` ist `false` | `live` |
| Epoche stimmt überein; Generation ist mindestens `baseline`; `rules` ist `true` | `unknown` |

**live** bedeutet hier nur, dass die Datei gemäß der Namensraumgeneration nicht veraltet ist. Es bestätigt weder Inhaltsfrische noch TTL-Gültigkeit oder einen NGINX-Cache-HIT. Bei `namespaces_complete=false` bleiben zurückgegebene Einträge verwendbar; ausgelassene Namensräume gelten als unbekannt.

Eine Datei mit höherer Generation bei gleichem Namensraum, gleicher Epoche und gleichem logischen Schlüssel beweist, dass die Datei mit niedrigerer Generation veraltet ist: Die aktuelle Generation sinkt innerhalb einer Epoche nie. Andernfalls bleibt das Ergebnis unknown.

Status vor und nach dem Scan lesen und Einträge anhand des Namensraumnamens zuordnen. Aus dem Status abgeleitete Ergebnisse für einen Namensraum nur verwenden, wenn er in beiden Antworten vorkommt und `epoch`, `baseline` und `serial` unverändert sind. Fehlender oder geänderter Zustand macht dessen Ergebnisse ungültig. Epochen nur auf Gleichheit vergleichen. Diese Beobachtungen verhindern keine späteren Invalidierungen.

Eine leere Bereinigungswarteschlange beweist keine Gültigkeit. Ein vor der Invalidierung begonnener Request kann nach Abschluss der Bereinigung eine Datei der alten Generation schreiben. Sie bleibt bis zu einer späteren Bereinigung oder zur Verdrängung durch den nativen Cache bestehen. Die Bereinigung betrifft außerdem nur den Cache-Pfad der PURGE-Location, während ein Namensraum mehrere Pfade umfassen kann.

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

Der [Status-Endpunkt](DIRECTIVES.de.md#cache_purge_queue_status) zeigt wartende und aktive Aufträge, Aufnahme-Wartezeit, Duplikate, Unlink-Ergebnisse und HIT/MISS. `202 Accepted` bestätigt Aufnahme oder Zusammenfassung, nicht die Löschung oder sofortige logische Invalidierung. Anfragelatenz gemeinsam mit Queue-Alter und Datenträgerkonkurrenz beobachten.

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

<a id="temporary-directories"></a>

## Temporäre Verzeichnisse

Durchläufe überspringen konfigurierte temporäre Unterverzeichnisse der Cache-Wurzel anhand vollständiger Pfadkomponenten. Namen wie `proxy_temp` sind nicht reserviert, wenn sie nicht als temporärer Pfad konfiguriert sind. Nur reguläre Dateien mit genau 32 hexadezimalen Zeichen im Dateinamen sind Löschkandidaten. Numerische und mit Suffix versehene temporäre Cache-Schreibdateien werden übersprungen, auch bei `use_temp_path=off`.

Jeder Durchlauf prüft außerdem native Header-Version, Offsets, vollständigen `KEY: `-Marker, CRC32 und Schlüssel-/Varianten-Dateihash bei maximal 8192 Schlüsselbytes. Ungültige, verkürzte, fremde oder nicht passende Dateien werden auch bei purge-all übersprungen. ETag-/Vary-Längen genau am Core-Maximum sind erlaubt. Formatprüfung authentifiziert keine bösartige Datei.

Der konfigurierte Cache-Root darf ein Symlink oder eine Windows-Junction sein. Untergeordnete Symlinks/Reparse Points werden übersprungen. Auf POSIX mit vollständigem Feature-Probe nutzen Kindverzeichnisse und Dateien Core-`ngx_openat_file`/`ngx_file_at_info` mit No-follow-Flags; gelöscht wird relativ zum gehaltenen Elternverzeichnis. Das verhindert Umleitung durch ausgetauschte untergeordnete Symlinks, ist aber keine Sandbox gegen das Verschieben bereits offener Verzeichnisse. Windows und ältere Unix-Systeme verwenden Core-Vollpfadoperationen mit Linkprüfung und behalten ein Pfadaustauschrennen. Auch Legacy-Exact-Lookup/-Löschung verwendet Vollpfade. Cache-Wurzeln privat halten; keine nicht vertrauenswürdigen Schreiber zulassen.

Konfigurierte Pfade einheitlich und kanonisch schreiben. Vergleiche lösen keine Symlink-, Mount- oder Unicode-Aliase auf. Wartende Aufträge behalten ihren Schutzsatz; neuere Duplikate aktualisieren wartende Arbeit oder die nächste Wiederholung. Ein laufender Durchlauf behält seinen bisherigen Satz. Es handelt sich nicht um die Vereinigung aller lebenden Worker-Generationen. Betroffene Pfade während wartender oder aktiver Aufträge nicht umwidmen.

---

<a id="testing"></a>

## Tests

Die [Testübersicht](../TEST_COVERAGE.md) (Englisch) zeigt geprüfte Fälle und verbleibende Lücken.

Gesamte Suite in Docker bauen und ausführen:

```sh
make test-all
NGINX_VERSION=1.31.7 make test-all
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

Strenge Builds, Sanitizer, Valgrind, Plattformjobs und Berichte beschreibt [`t/TESTING.md`](../../t/TESTING.md). Alle GitHub-Jobs stehen in `build-test-release.yml`.
