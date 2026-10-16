[ngx_cache_purge](README.pl.md)

<a id="directives"></a>

# Dyrektywy

<a id="proxy_cache_purge"></a>

## `proxy_cache_purge`

```
Syntax:  proxy_cache_purge on | off;
         proxy_cache_purge <method> [purge_all] from all | <cidr> ...;
         proxy_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

**Forma w obsługującej lokalizacji** (`from …`) przechwytuje wskazaną metodę HTTP w proxy location i usuwa wpis. `on` oznacza `PURGE`, `off` wyłącza. Ogranicz CIDR lub użyj `from all`. `purge_all` przed `from` opróżnia całą strefę niezależnie od URI.

**Oddzielna lokalizacja** (strefa i klucz) jest dozwolona tylko w location PURGE, zwykle z regex przechwytującym klucz. Wyszukuje strefę po nazwie i czyści według skompilowanego wyrażenia. Niezgodna z `proxy_cache` i `proxy_pass` w tej samej lokalizacji.

<a id="fastcgi_cache_purge"></a>

## `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Odpowiednik `proxy_cache_purge` dla stref `fastcgi_cache` / `fastcgi_cache_path`.

<a id="scgi_cache_purge"></a>

## `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Odpowiednik `proxy_cache_purge` dla stref `scgi_cache` / `scgi_cache_path`.

<a id="uwsgi_cache_purge"></a>

## `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Odpowiednik `proxy_cache_purge` dla stref `uwsgi_cache` / `uwsgi_cache_path`.

<a id="cache_purge_response_type"></a>

## `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

Wybiera format udanych odpowiedzi PURGE; domyślnie używany jest JSON. `Cache-Purge-Code` jest zgodny z kodem w treści. Odpowiedzi modułu zawierają `Cache-Control: no-store`, a błędy używają `application/problem+json` zgodnie z RFC 9457. Znaki specjalne kluczy tekstowych są zabezpieczane; klucze binarne używają Base64 z oznaczeniem kodowania.

`none` wyłącza treść PURGE: zakończony sukces zwraca `204`, przyjęta praca w tle zachowuje `202`, a błędy 4xx/5xx. Status i REFRESH nadal zwracają strukturalny JSON. Kody, pola i ograniczenia opisuje [kontrakt HTTP](../HTTP_RESPONSES.md).

<a id="cache_purge_background_queue"></a>

## `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

Włącza odroczone przetwarzanie wzorców i `purge_all`. Po zakolejkowaniu lub scaleniu z identyczną oczekującą/aktywną pracą zwraca `202 Accepted`. Dokładne klucze pozostają bez zmian.

Dotyczy historycznych wzorców i pełnego czyszczenia. Wiązanie `cache_purge_key` zawsze używa odroczonego czyszczenia fizycznego i logicznego `200`, nawet przy wyłączeniu tej opcji.

Przy pełnej kolejce lub błędzie alokacji żądanie czeka otwarte na przyjęcie, bez przejścia katalogów. Duplikat istniejącego zadania może zostać przyjęty mimo pełnej kolejki. Brak pamięci na mały stan oczekiwania daje `500`. Rozłączenie anuluje timer i zwalnia licznik oczekiwania.

Dla tej opcji i `cache_purge_vary_aware` obowiązuje ostatnia zadeklarowana wartość. Nieprawidłowe wartości są odrzucane. `cache_purge_legacy_status` jest przestarzała i ignorowana. Wątki i cykl życia: [model wykonania](../ARCHITECTURE.md#execution-model).

<a id="cache_purge_queue_size"></a>

## `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

Maksimum niezakończonych zadań wszystkich ścieżek, zarówno oczekujących, jak i przydzielonych. Duplikaty współdzielą alokację; powtórzenie zachowuje pamięć i miejsce. Inne zadania czekają na wolne miejsce.

Strefa powstaje przy konfiguracji tła lub endpointu statusu. Zmiana pojemności wymaga restartu nawet przy tym samym zaokrąglonym rozmiarze. Reload odrzuca zmianę, chroniąc przyjętą pracę. Dodatkowe korzenie też mogą wymagać pamięci i restartu. Wcześniej zakończ oczekujące/aktywne zadania, które muszą się wykonać.

Deklaracja przestrzeni nazw również tworzy kolejkę.

Rozmiar obejmuje zadania, metadane ścieżek i wyrównane metryki workerów. Zależy od kompilacji i liczby ścieżek; zobacz [wymiarowanie pamięci](../OPERATIONS.md#memory-sizing).

<a id="cache_purge_thread_pool"></a>

## `cache_purge_thread_pool`

```
Syntax:  cache_purge_thread_pool <name>;
Default: default
Context: http, server, location
```

Wybiera wykonawcę przyjętych zadań tła ze zwykłym dziedziczeniem. Przykład:

```nginx
thread_pool purge_io threads=2 max_queue=128;
http {
    cache_purge_background_queue on;
    cache_purge_thread_pool purge_io;
}
```

NGINX weryfikuje pule i dostarcza zakończenie przez natywne zdarzenie zadania. Prywatne struktury ani kolejki zakończeń nie są kopiowane. Przy wątkach własne pule muszą istnieć. Bez wątków i na Windows dyrektywa działa z ograniczonymi krokami pętli. Pełna pula opóźnia zadanie, nie przenosi I/O z powrotem do pętli. Dedykowane pule dodają stosy i przełączenia kontekstu.

<a id="cache_purge_batch_size"></a>

## `cache_purge_batch_size`

```
Syntax:  cache_purge_batch_size <number>;
Default: 10
Minimum: 1
Maximum: 64
Context: http
```

Maksimum zadań na worker między przerwami. Postęp zachowuje się między krokami i zakończeniami wątków. Pusta kolejka kończy partię. Wartości ponad 64 są obcinane z ostrzeżeniem; zero odrzucane.

Kontroluje zadania, nie pliki ani czas callbacku. Mniej zadań oznacza więcej przerw, bez przerywania aktywnego przejścia wątku. Strojenie dysku łącz z `cache_purge_throttle_ms`.

<a id="cache_purge_throttle_ms"></a>

## `cache_purge_throttle_ms`

```
Syntax:  cache_purge_throttle_ms <time>;
Default: 10ms
Minimum: 1ms
Context: http
```

Przerwa między partiami. Przyjmuje `10ms`, `500ms`, `1s` oraz złożone wartości w cudzysłowie, np. `"2s 500ms"`. Liczba bez jednostki oznacza milisekundy: `10` i `10ms` są równoważne. Brak jednostki generuje ostrzeżenie; podanie jej go usuwa.

Bezczynny worker sprawdza po dziesięciokrotności odstępu. Nieukończone kroki i zajęty mutex używają timerów 1 ms. To minimalne odstępy planowania, nie terminy graniczne. Zero i wartości poza bezpiecznym zakresem platformy są odrzucane.

Przyjęcie, przeciążona pula i odroczone próby czyszczenia są sprawdzane co 10 ms.

<a id="cache_purge_queue_status"></a>

## `cache_purge_queue_status`

```
Syntax:  cache_purge_queue_status <cache_path>;
Default: not configured
Context: location
```

Udostępnia obraz JSON przez GET/HEAD z `Cache-Control: no-store`. Ścieżka musi wskazywać skonfigurowany korzeń; nieznany blokuje konfigurację nawet bez tła. Ogranicz dostęp:

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

`queue` opisuje wspólną kolejkę wszystkich ścieżek:

| Pole | Typ / jednostka | Znaczenie |
|---|---|---|
| `size` | Całkowita / zadania | Zadania czekające na worker |
| `running` | Całkowita / zadania | Przydzielone, także czekające na pulę |
| `max_size` | Całkowita / zadania | Łączna pojemność oczekujących i przydzielonych |
| `wait_time` | Całkowita / milisekundy | Wiek najstarszego oczekiwania; zero przy pustej kolejce |
| `full` | Logiczna | Próbka sumy oczekujących i przydzielonych osiągnęła pojemność |

`cache` opisuje korzeń wybrany przez location:

| Pole | Typ / jednostka | Znaczenie |
|---|---|---|
| `pending.total` | Całkowita / zadania | Oczekujące dla tego cache |
| `pending.full` | Całkowita / zadania | Oczekujące `purge_all`, wliczone w `pending.total` |
| `active` | Całkowita / zadania | Przydzielone do końca rozliczenia wyniku |
| `requests.overflow` | Całkowita / żądania | Żądania oczekujące na przyjęcie, także przez blokadę/alokację |
| `requests.deduplicated` | Całkowita / żądania | Scalone z oczekującą lub aktywną pracą |
| `requests.waiting` | Całkowita / żądania | Aktualnie oczekujące na przyjęcie w tym cache |
| `files.deleted` | Całkowita / pliki | Udane usunięcia tła zgłoszone po przejściu |
| `files.errors` | Całkowita / błędy | Błędy usuwania poza już nieistniejącym plikiem |
| `files.bytes` | Całkowita / bajty | Szacowane logiczne rozmiary usuniętych plików |
| `responses.hit`, `responses.miss` | Całkowita / główne żądania | Natywne HIT/MISS zaobserwowane w fazie log |
| `responses.hit_ratio` | Liczba / 0..1 | HIT / (HIT + MISS), zero bez obserwacji |
| `logical.invalidations` | Całkowita / decyzje | Opublikowane invalidacje, w tym duplikaty i REFRESH |
| `cleanup.scan_errors` | Całkowita / błędy | Błędy metadanych/otwarcia/odczytu/zamknięcia/alokacji tła |
| `cleanup.retries` | Całkowita / przejścia | Dodatkowe przejścia po błędzie czyszczenia |
| `cleanup.exhausted` | Całkowita / zadania | Zadania wycofane po trzech nieudanych ponowieniach |

`pending.full` liczy pełne czyszczenia; wzorzec klucza `*` jest innym typem. `active` obejmuje oczekiwanie na pulę i zajmuje pojemność, choć nie wchodzi w `queue.size` ani `pending.total`.

Przy scalonym przejściu przestrzeni `pending.full` odpowiada klasyfikacji pierwszego zadania. Skanowania/sondy REFRESH nie wchodzą w głębokość PURGE ani błędy usuwania. Rotacja opóźnionej próby może sprawić, że `wait_time` jest wiekiem próbkowanej głowy, nie maksimum.

Każdy slot procesu ma jednego pisarza z pętli zdarzeń; początki i odstępy są wyrównane do linii cache CPU. Wątki zwracają wyniki zdarzeniami. Odczyt statusu nie używa mutexu ani atomic fetch-add, również z zerem. Na 64 bitach są zwykłe wyrównane odczyty/zapisy volatile; na 32 bitach ograniczone sprawdzenia sekwencji chronią 64-bitowy odczyt, czasem pomijając zajętą część próbki. Pola są przybliżone, nietransakcyjne między workerami. Przyjęcie korzysta z zablokowanego stanu sterowania, nie metryk.

Liczniki nasycają się na maksimum unsigned 64 bit, przeżywają reload i zerują przy restarcie. Wskaźnik oczekiwania podąża za sprzątaniem żądania. Operacje synchroniczne i usunięcia cache managera są wyłączone z sum tła. Bajty szacują długość, nie bloki ani odzyskany dysk; otwarci czytelnicy i współbieżne zapisy zmieniają wynik. Częściowe wyniki się liczą; wyniki uszkodzonego workera mogą zaginąć. To nie dziennik audytu.

HIT/MISS obejmuje główne żądania z natywną decyzją cache. Brak upstream, status zero, BYPASS, EXPIRED, STALE, UPDATING i REVALIDATED nie wchodzą do ilorazu. Zwykłe żądanie bez cache nie staje się MISS. Sam monitoring może utworzyć SHM bez tła. Na 32 bitach wiek obejmuje mniej niż około 24,9 dnia; ujemna różnica zegara ze znakiem daje zero.

<a id="cache_purge_legacy_status"></a>

## `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

Przestarzała dyrektywa akceptowana tylko na potrzeby migracji konfiguracji. Obie wartości powodują ostrzeżenie i są ignorowane; usuń dyrektywę. Nieprawidłowe wartości są odrzucane. Brak dopasowania synchronicznego zwraca `200 no_match`, przyjęte fizyczne czyszczenie `202 accepted`, opublikowane unieważnienie generacji `200 invalidated`. Przełącznik `412`/`404` został usunięty. Zobacz [kontrakt HTTP](../HTTP_RESPONSES.md).

<a id="cache_purge_vary_aware"></a>

## `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

Przy `on`, udana dokładna purga usuwa plik główny, potem przechodzi cache i usuwa pliki z identycznym `KEY:`. Obejmuje `Vary` i `gzip_vary` w różnych ścieżkach, ale o jednym kluczu logicznym. Bajt bezpośrednio po kluczu musi być `\n`, co wyklucza fałszywe zgodności samego prefiksu.

Domyślnie wyłączone, ponieważ każde dokładne czyszczenie dodaje pełny przegląd katalogu. Włącz dla wielu reprezentacji `Vary`/`gzip_vary`. Przegląd następuje również przy braku głównego wpisu. Maski i `purge_all` już obejmują warianty. Dokładne klucze Vary rozróżniają wielkość liter i mają limit 511 bajtów; dłuższe zwracają `400 invalid_selector` przed jakimkolwiek usuwaniem.

Invalidacja generacji obejmuje wszystkie reprezentacje klucza bez tej opcji.

<a id="cache_purge_namespace"></a>

## `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

Deklaruje przestrzeń generacji przed wiązaniem lokalizacji. Nazwa: 1–32 litery ASCII, cyfry, `_` lub `-`; SHM minimum osiem stron natywnych. Użyj osobnego trwałego katalogu stanu poza cache i katalogami tymczasowymi. Rdzeń tworzy trwały znacznik z wyłącznością, rezerwując epokę restartu. Zachowuj te małe pliki tak długo, jak może istnieć odpowiadający cache; skasowanie stanu bez cache pozwala ponownie użyć starej epoki. Brak gwarancji trwałości wobec zaniku zasilania/fsync. Zwykły reload zachowuje epokę i reguły; zmiana rozmiaru/ścieżki lub usunięcie przestrzeni wymaga restartu. SHM przestrzeni i kolejki mają osobne rozmiary.

<a id="cache_purge_key"></a>

## `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

Wiąże location z istniejącą przestrzenią i udostępnia `$cache_purge_key`. Ustaw tę zmienną jako całe `proxy_cache_key`, `fastcgi_cache_key`, `scgi_cache_key` lub `uwsgi_cache_key`, również w oddzielnej lokalizacji PURGE. Różnica rozwiniętego klucza natywnego i wygenerowanego powoduje `500`, zamiast deklaracji ścisłej invalidacji. Nie odwołuj się do `$cache_purge_key` we własnym wyrażeniu logicznym. Limit klucza logicznego: 8096 bajtów. Wiąż tylko uczestniczące lokalizacje, gdy pozostałe wymagają starej semantyki; nie ma `off` anulującego dziedziczone wiązanie.

<a id="proxy_cache_refresh"></a>

## `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

Tworzy jawny REFRESH tylko dla żądań głównych; inne metody dostają `405`. Wymaga wiązania `cache_purge_key`, natywnego wyrażenia `$cache_purge_key` i profilu refresh. Rozwinięty klucz wybiera dokładną wartość, prefiks zakończony `*` lub `*` dla przestrzeni na tej ścieżce. Chroń location natywną kontrolą dostępu/uwierzytelnianiem. REFRESH obsługuje tylko proxy; PURGE wszystkie cztery protokoły.

<a id="cache_purge_refresh_profile"></a>

## `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

Mapuje znaną reprezentację na literalną wewnętrzną lokalizację proxy. Usunięcie dokładnego prefiksu z rozróżnianiem liter musi pozostawić URI źródła zaczynające się od `/`; query string i escape są zachowane. Moduł nie zgaduje hosta ani nie szuka `://` w kluczu. Cel wymaga `internal`, wyłączonego cache, przekazywania nagłówków/ciała wyłączonego, HEAD, niepustego literalnego Host i literalnych nagłówków reprezentacji. Nagłówki warunkowe używają dokładnie `$cache_purge_refresh_etag` i `$cache_purge_refresh_last_modified`; `$cache_purge_refresh_uri` dostarcza URI do `proxy_pass`. Te trzy czyste zmienne mają sens tylko w sondach. Cel sprawdzany jest w preaccess, także po przekierowaniu wewnętrznym. Administrator gwarantuje tę samą reprezentację źródła co klucz; Vary pomija się, bo natywne metadane nie zachowują pierwotnych nagłówków.

Ustaw `proxy_intercept_errors off` w lokalizacji sondy. Dziedziczona interceptacja jest odrzucana, aby błąd źródła nie zmienił się w sukces invalidujący.

<a id="cache_purge_refresh_timeout"></a>

## `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

Ogranicza rozpoczynanie skanów/sond strony REFRESH, wraz z oczekiwaniem na skaner workera. Upływ czasu zatrzymuje nową pracę i oznacza odpowiedź niepełną. Aktywny HEAD kończy się według jawnie ustawionych `proxy_connect_timeout`, `proxy_send_timeout` i `proxy_read_timeout`. To nie twardy termin odpowiedzi i nie przerywa syscall. Rozłączenie/łagodne zamknięcie zatrzymuje nowe zgłoszenia, zachowując pamięć aktywnego wątku do natywnego zakończenia.
