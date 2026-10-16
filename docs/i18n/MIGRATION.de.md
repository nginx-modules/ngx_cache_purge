# ngx_cache_purge

[README](README.de.md) · [Direktiven](DIRECTIVES.de.md)

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
