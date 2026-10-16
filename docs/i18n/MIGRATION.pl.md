# ngx_cache_purge

[README](README.pl.md) · [Dyrektywy](DIRECTIVES.pl.md)

<a id="migration"></a>

## Migracja

<a id="queue-and-status-changes"></a>

### Zmiany kolejki i statusu

Zmiany struktury SHM wymagają pełnego restartu. Składnia metoda/ACL i obsługa protokołów pozostają dostępne. v4 domyślnie używa JSON, zgodnych kodów w nagłówku i treści oraz `200 no_match` przy braku wpisu. `cache_purge_legacy_status` powoduje tylko ostrzeżenie. Dostosuj klientów do [kontraktu HTTP](../HTTP_RESPONSES.md). Pojemność obejmuje przydzielone zadania; przeciążone żądanie czeka na przyjęcie. Aktywne duplikaty mogą zaplanować kolejny przebieg.

Status dodaje pola bez zmiany starych nazw. `requests.overflow` liczy teraz oczekiwania. Metryki są przybliżone z licznikami 64-bitowymi. Kompilacja z wątkami automatycznie rejestruje użyte `default`; nazwana pula izoluje I/O. Sam status alokuje SHM.

Generacje i REFRESH są jawnie włączane w v4.0. Powiąż `cache_purge_key`, ustawiając `$cache_purge_key` jako cały natywny klucz każdej uczestniczącej lokalizacji. Zmienia to nazwy plików; historyczne stają się zimne i czekają na wygaśnięcie lub jawną historyczną purgę. Zachowaj znaczniki stanu wraz z cache. Istniejące PURGE nie zyskują REFRESH. Użytkownicy eksperymentalnego PR60 muszą skonfigurować endpoint/profil i paginację; nieograniczona odpowiedź, zgadywanie URI/Host i niejawne rozszerzanie metod PR60 nie są gwarancjami zgodności.

<a id="from-the-original-frickle-module"></a>

### Z oryginalnego modułu FRiCKLE

Formy metoda/lista dostępu i strefa/klucz pozostają dostępne. Brak wpisu zwraca `200 no_match`; usuń `cache_purge_legacy_status` i zaktualizuj klientów. Aby włączyć pracę w tle, dodaj:

```nginx
http {
    cache_purge_background_queue on;
}
```

<a id="from-nginx-selective-cache-purge-module"></a>

### Z nginx-selective-cache-purge-module

Zastąp moduł; kolejka jest wbudowana. Przenieś poprzednie ustawienia do `cache_purge_queue_size`, `cache_purge_batch_size` i `cache_purge_throttle_ms`.
