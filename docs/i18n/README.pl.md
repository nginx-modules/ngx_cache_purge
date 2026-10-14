<a id="ngx_cache_purge"></a>

# ngx_cache_purge

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

Moduł NGINX do usuwania wpisów pamięci podręcznej `FastCGI`, `proxy`, `SCGI` i `uWSGI`. Wersja 4.0 zachowuje dotychczasowy interfejs, dodając jawnie włączaną invalidację generacji przed odroczonym usuwaniem plików oraz ograniczony warunkowy REFRESH dla cache proxy.

---

<a id="contents"></a>

## Spis treści

- [Możliwości](#features)
- [Zgodność](#compatibility)
- [Instalacja](#installation)
- [Model wykonania](#execution-model)
- [Dyrektywy](#directives)
- [Usuwanie według części klucza](#partial-key-purge)
- [Invalidacja generacji](#generation-invalidation)
- [Warunkowy REFRESH](#conditional-refresh)
- [Przykładowe konfiguracje](#sample-configurations)
- [Strojenie wydajności](#performance-tuning)
- [Monitorowanie i debugowanie](#monitoring-and-debugging)
- [Rozwiązywanie problemów](#troubleshooting)
- [Testowanie](#testing)
- [Migracja](#migration)
- [Katalogi tymczasowe](#temporary-directories)
- [Bezpieczeństwo](#security)
- [Licencja](#license)

---

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
- **Warunkowy REFRESH** — jawny profil HEAD, wspólny limit sond i strony kontynuacji po 32 wpisy

---

<a id="compatibility"></a>

## Zgodność

Tabela pokazuje cele skonfigurowane w CI. Udane zadanie potwierdza daną konfigurację źródeł i kompilacji, nie wszystkie wydania serii.

| NGINX | CI: push / PR | CI: + full |
|---|---|---|
| 1.30.5, 1.31.6 | `test-matrix` | — |
| 1.31.6 | `build-compatibility` (GCC/Clang), `sanitizer`, `native-macos`, `native-windows` (x64/x86) | `memory-leak-test` (Valgrind) |
| 1.7.9 | `build-compatibility` (GCC) | — |
| 1.7.11, 1.7.12 | `build-compatibility` (GCC) | — |
| 1.20.2, 1.26.3, 1.28.3 | — | `test-matrix` |
| 1.11.4, 1.11.5 | — | `build-compatibility` (GCC) |

Zadania Linux używają tych samych obrazów Docker i punktu wejścia co testy lokalne. Natywne zadania macOS i Windows sprawdzają własne API plików i procesów; Windows kompiluje MSVC i testuje HTTP, ścieżki i reload w `nginx.exe`. Scenariusze POSIX procesów/sygnałów działają na Linux i macOS. Windows nie uruchamia ich ani opcjonalnej ścieżki puli wątków NGINX.

Bez `--with-threads` przeglądanie jest przyrostowe. Ten tryb jest również używany z epoll przed NGINX 1.7.12 z powodu wyścigu przy powiadomieniu w rdzeniu. BSD nie ma natywnego CI. Zwykłe przebiegi sprawdzają stable/mainline i granice API; rozszerzone dodają starsze wydania i Valgrind. Zmiany wyłącznie dokumentacji pomijają kompilacje. Harmonogram i polecenia: [testowanie](../../t/TESTING.md).

Kod podzielono na konfigurację, obsługę żądań, kolejkę, przeglądanie, unieważnianie, REFRESH i odpowiedzi. Wspólne funkcje ścieżek są w `ngx_cache_purge.h`, operacje platformowe w nagłówkach POSIX/Win32. `generation.*` zmieniono na `invalidation.*`; generacja nadal oznacza wersję kluczy. Zobacz [architekturę](../ARCHITECTURE.md).

Moduł stosuje styl NGINX zgodny z C89 oraz API zdarzeń, slab, rbtree i atomików rdzenia. Nie wymaga atomików C11 ani nowszego API plików. Rygorystyczne kompilacje GNU89 sprawdzają kolejność deklaracji i ostrzeżenia; wyniki zapisują faktycznie użyte kompilator i platformę.

---

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
./configure --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

---

<a id="execution-model"></a>

## Model wykonania

**Zadanie** to jedna operacja ze wzorcem lub `purge_all`. **Partia** to liczba zadań przetwarzanych przez worker przed przerwą. **Krok** to ograniczony fragment przejścia przyrostowego; zadanie może wymagać wielu kroków.

Przy generacjach jedno fizyczne przejście na ścieżkę i przestrzeń nazw scala zaakceptowane invalidacje. Strony REFRESH są osobnymi operacjami HTTP, nie zadaniami kolejki PURGE.

| Żądanie i konfiguracja | Przejście plików | Zakończenie HTTP |
|---|---|---|
| Historyczny dokładny klucz, z Vary lub bez | Worker żądania; Vary dodaje przejście katalogu | Po operacji |
| Wzorzec lub `purge_all`, kolejka wyłączona | Worker żądania, synchronicznie | Po przejściu |
| Zadanie w tle, wybrana pula | Jedno zadanie puli na worker naraz | `202` po przyjęciu |
| Zadanie w tle bez dostępnej puli | Pętla zdarzeń workera, przyrostowe kroki | `202` po zakolejkowaniu |
| Wyczerpana pojemność/alokacja | Żądanie czeka na przyjęcie bez własnego przejścia | `202` po przyjęciu |
| PURGE generacji, dowolny klucz | Odroczone przejście przestrzeni; bez otwierania/usuwania plików w żądaniu | `200 invalidated` po publikacji i przyjęciu |
| Endpoint REFRESH | Ograniczone skanowanie i do 32 kolejnych HEAD | JSON `200` z polami końca i kontynuacji |

Wiersze bez generacji opisują zachowanie historyczne. Żądanie generacji oczekujące na pojemność zwraca po przyjęciu `200`, nie `202`.

Krok kończy się po najwyżej 64 iteracjach katalogu lub zauważeniu upływu 5 ms; limit obejmuje pominięcia i końce katalogów. Nie ogranicza pojedynczego wywołania systemowego.

Aby przenieść operacje plikowe tła do wątków, kompiluj z `--with-threads` i zadeklaruj pulę rdzenia w kontekście **main**, poza `http`:

```nginx
thread_pool default threads=2;
```

`cache_purge_thread_pool <name>` wybiera istniejącą pulę w `http`, `server` lub `location`. Domyślna `default` jest tworzona przez NGINX ze standardowymi ustawieniami, jeśli użyta bez deklaracji. Własne nazwy wymagają `thread_pool`. Wybór jest dziedziczony; pierwsze przyjęte żądanie określa wykonawcę scalonej pracy. Każdy worker ma własne pule. Pełna pula powoduje ponowne zgłoszenie po 10 ms z zachowaniem miejsca przyjęcia, nigdy przejście synchroniczne. Bez wątków, na Windows, w trybie jednego procesu lub po usunięciu puli starego zadania przez reload działa kursor przyrostowy.

REFRESH używa wybranej puli dla jednego dodatkowego skanera na worker. Natywne podżądania HTTP działają w pętli zdarzeń; wątek I/O nie dotyka ich stanu.

Zmiany strukturalne kolejki i indeksu chroni mutex slab. Przyjęcie i callbacki tła używają trylock i ponowień timerem. Zadania oczekujące **i działające** pozostają w drzewie tekstowym rbtree rdzenia na ścieżkę; pełne klucze rozstrzygają kolizje hash. Indeks ma koszt `O(log Q)` dla `Q` niezakończonych zadań; wyszukiwanie ścieżki jest liniowe względem ich liczby. Listy `ngx_queue_t` śledzą oczekujące i aktywne zadania. Pierwszy aktywny duplikat żąda powtórzenia; kolejne się z nim scalają. Po bieżącym przejściu powtórzenie trafia na koniec, więc późniejszy PURGE nie ginie. Ciągłe nowe purgi mogą wymuszać dalsze przejścia. Miejsce zwalnia się dopiero po ostatecznym końcu. I/O i formatowanie odpowiedzi działają poza blokadą.

Rbtree deduplikuje tylko fizyczne czyszczenie. Reguły generacji są w ograniczonych drzewach trie bajtów oddzielnej strefy SHM. Ścisłe przejście przestrzeni ma inną tożsamość indeksu niż zadanie historycznego klucza; decyzje REFRESH korzystają z tej samej ścieżki czyszczenia.

Pełna kolejka lub błąd alokacji pozostawia HTTP w oczekiwaniu z timerem 10 ms i natywną obsługą rozłączenia. Żądania i procedury sprzątania pozostają we własnym workerze; SHM nie zawiera ich wskaźników. Odczyt całego połączenia HTTP/2 i HTTP/3 nie jest wyłączany. Presja kolejki nie generuje 429/503; krótki odczytany PURGE nie gwarantuje TCP Zero Window. Oczekiwanie kosztuje pamięć i połączenia: stosuj timeouty i polityki przyjęcia rdzenia zgodnie z budżetem.

Przyjęte zadania zachowują klucze, pule i ochronę ścieżek tymczasowych przy zwykłym reload. Nowszy duplikat aktualizuje ochronę oczekującego zadania lub następnego przejścia. Wyłączenie przyjmowania zachowuje przyjętą pracę. Worker przy łagodnym zamknięciu kończy bieżące przejście i nie bierze nowych zadań. Szybkie wyjście oddaje nieukończone zadanie przyrostowe do kolejki bez kończenia skanowania w hooku wyjścia. Cykl życia zgłoszonych zadań wątkowych kontroluje zamknięcie puli rdzenia.

Epoki PID/slotu identyfikują przydziały. Co sekundę workery sprawdzają do 64 aktywnych rekordów na przebieg i kolejkują ponownie pracę zakończonego właściciela. Odzyskuje to porzucone przydziały, nie tworzy dziennika transakcyjnego. Awaria w trakcie aktualizacji wspólnej listy/slab może uszkodzić stan i wymagać restartu. Usunięcia tuż przed awarią mogą nie trafić do metryk. Oczekujące zadania/powtórzenia potrzebują workera: opróżnij kolejkę przed zatrzymaniem, jeśli mają się skończyć. Kolejka nie jest trwała.

Wewnętrzny układ kolejki nie jest ABI aktualizacji. Zmiana na kompilację o innym układzie wymaga pełnego restartu, po zakończeniu wymaganej pracy. Zwykły reload tej samej kompilacji używa istniejącej strefy.

Usuwanie działa best effort. `202` potwierdza przyjęcie, nie usunięcie; ukończone przejście nie jest atomowym obrazem. Współbieżne żądania mogą tworzyć wpisy podczas i po przejściu; błędy są logowane. Wzorce i pełne przejścia usuwają pliki bez synchronicznej aktualizacji każdego węzła rdzenia. Operacje dokładne i Vary unieważniają pasujące węzły.

Błędne przejście tła ponawia się po 100 ms, 1 s i 10 s, najwyżej trzy razy. Odroczone zadanie zachowuje miejsce przyjęcia/deduplikacji i pozwala działać innym. Nowy PURGE zeruje okno ponowień. Po wyczerpaniu zadanie jest wycofywane z błędem w logu; sprawdź `cleanup.exhausted` i ponów po naprawie dysku. Invalidacja generacji działa mimo błędu fizycznego. Brakujące pliki to spodziewane wyścigi; uszkodzone są pomijane bez nieskończonych prób.

---

<a id="directives"></a>

## Dyrektywy

<a id="proxy_cache_purge"></a>

### `proxy_cache_purge`

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

### `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Odpowiednik `proxy_cache_purge` dla stref `fastcgi_cache` / `fastcgi_cache_path`.

<a id="scgi_cache_purge"></a>

### `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Odpowiednik `proxy_cache_purge` dla stref `scgi_cache` / `scgi_cache_path`.

<a id="uwsgi_cache_purge"></a>

### `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Odpowiednik `proxy_cache_purge` dla stref `uwsgi_cache` / `uwsgi_cache_path`.

<a id="cache_purge_response_type"></a>

### `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

Wybiera format udanych odpowiedzi PURGE; domyślnie używany jest JSON. `Cache-Purge-Code` jest zgodny z kodem w treści. Odpowiedzi modułu zawierają `Cache-Control: no-store`, a błędy używają `application/problem+json` zgodnie z RFC 9457. Znaki specjalne kluczy tekstowych są zabezpieczane; klucze binarne używają Base64 z oznaczeniem kodowania.

`none` wyłącza treść PURGE: zakończony sukces zwraca `204`, przyjęta praca w tle zachowuje `202`, a błędy 4xx/5xx. Status i REFRESH nadal zwracają strukturalny JSON. Kody, pola i ograniczenia opisuje [kontrakt HTTP](../HTTP_RESPONSES.md).

<a id="cache_purge_background_queue"></a>

### `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

Włącza odroczone przetwarzanie wzorców i `purge_all`. Po zakolejkowaniu lub scaleniu z identyczną oczekującą/aktywną pracą zwraca `202 Accepted`. Dokładne klucze pozostają bez zmian.

Dotyczy historycznych wzorców i pełnego czyszczenia. Wiązanie `cache_purge_key` zawsze używa odroczonego czyszczenia fizycznego i logicznego `200`, nawet przy wyłączeniu tej opcji.

Przy pełnej kolejce lub błędzie alokacji żądanie czeka otwarte na przyjęcie, bez przejścia katalogów. Duplikat istniejącego zadania może zostać przyjęty mimo pełnej kolejki. Brak pamięci na mały stan oczekiwania daje `500`. Rozłączenie anuluje timer i zwalnia licznik oczekiwania.

Dla tej opcji i `cache_purge_vary_aware` obowiązuje ostatnia zadeklarowana wartość. Nieprawidłowe wartości są odrzucane. `cache_purge_legacy_status` jest przestarzała i ignorowana. Wątki i cykl życia: [model wykonania](#execution-model).

<a id="cache_purge_queue_size"></a>

### `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

Maksimum niezakończonych zadań wszystkich ścieżek, zarówno oczekujących, jak i przydzielonych. Duplikaty współdzielą alokację; powtórzenie zachowuje pamięć i miejsce. Inne zadania czekają na wolne miejsce.

Strefa powstaje przy konfiguracji tła lub endpointu statusu. Zmiana pojemności wymaga restartu nawet przy tym samym zaokrąglonym rozmiarze. Reload odrzuca zmianę, chroniąc przyjętą pracę. Dodatkowe korzenie też mogą wymagać pamięci i restartu. Wcześniej zakończ oczekujące/aktywne zadania, które muszą się wykonać.

Deklaracja przestrzeni nazw również tworzy kolejkę.

Rozmiar obejmuje zadania, metadane ścieżek i wyrównane metryki workerów. Zależy od kompilacji i liczby ścieżek; zobacz [wymiarowanie pamięci](#memory-sizing).

<a id="cache_purge_thread_pool"></a>

### `cache_purge_thread_pool`

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

### `cache_purge_batch_size`

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

### `cache_purge_throttle_ms`

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

### `cache_purge_queue_status`

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

### `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

Przestarzała dyrektywa akceptowana tylko na potrzeby migracji konfiguracji. Obie wartości powodują ostrzeżenie i są ignorowane; usuń dyrektywę. Nieprawidłowe wartości są odrzucane. Brak dopasowania synchronicznego zwraca `200 no_match`, przyjęte fizyczne czyszczenie `202 accepted`, opublikowane unieważnienie generacji `200 invalidated`. Przełącznik `412`/`404` został usunięty. Zobacz [kontrakt HTTP](../HTTP_RESPONSES.md).

<a id="cache_purge_vary_aware"></a>

### `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

Przy `on`, udana dokładna purga usuwa plik główny, potem przechodzi cache i usuwa pliki z identycznym `KEY:`. Obejmuje `Vary` i `gzip_vary` w różnych ścieżkach, ale o jednym kluczu logicznym. Bajt bezpośrednio po kluczu musi być `\n`, co wyklucza fałszywe zgodności samego prefiksu.

Domyślnie wyłączone, ponieważ każde dokładne czyszczenie dodaje pełny przegląd katalogu. Włącz dla wielu reprezentacji `Vary`/`gzip_vary`. Przegląd następuje również przy braku głównego wpisu. Maski i `purge_all` już obejmują warianty. Dokładne klucze Vary rozróżniają wielkość liter i mają limit 511 bajtów; dłuższe zwracają `400 invalid_selector` przed jakimkolwiek usuwaniem.

Invalidacja generacji obejmuje wszystkie reprezentacje klucza bez tej opcji.

<a id="cache_purge_namespace"></a>

### `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

Deklaruje przestrzeń generacji przed wiązaniem lokalizacji. Nazwa: 1–32 litery ASCII, cyfry, `_` lub `-`; SHM minimum osiem stron natywnych. Użyj osobnego trwałego katalogu stanu poza cache i katalogami tymczasowymi. Rdzeń tworzy trwały znacznik z wyłącznością, rezerwując epokę restartu. Zachowuj te małe pliki tak długo, jak może istnieć odpowiadający cache; skasowanie stanu bez cache pozwala ponownie użyć starej epoki. Brak gwarancji trwałości wobec zaniku zasilania/fsync. Zwykły reload zachowuje epokę i reguły; zmiana rozmiaru/ścieżki lub usunięcie przestrzeni wymaga restartu. SHM przestrzeni i kolejki mają osobne rozmiary.

<a id="cache_purge_key"></a>

### `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

Wiąże location z istniejącą przestrzenią i udostępnia `$cache_purge_key`. Ustaw tę zmienną jako całe `proxy_cache_key`, `fastcgi_cache_key`, `scgi_cache_key` lub `uwsgi_cache_key`, również w oddzielnej lokalizacji PURGE. Różnica rozwiniętego klucza natywnego i wygenerowanego powoduje `500`, zamiast deklaracji ścisłej invalidacji. Nie odwołuj się do `$cache_purge_key` we własnym wyrażeniu logicznym. Limit klucza logicznego: 8096 bajtów. Wiąż tylko uczestniczące lokalizacje, gdy pozostałe wymagają starej semantyki; nie ma `off` anulującego dziedziczone wiązanie.

<a id="proxy_cache_refresh"></a>

### `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

Tworzy jawny REFRESH tylko dla żądań głównych; inne metody dostają `405`. Wymaga wiązania `cache_purge_key`, natywnego wyrażenia `$cache_purge_key` i profilu refresh. Rozwinięty klucz wybiera dokładną wartość, prefiks zakończony `*` lub `*` dla przestrzeni na tej ścieżce. Chroń location natywną kontrolą dostępu/uwierzytelnianiem. REFRESH obsługuje tylko proxy; PURGE wszystkie cztery protokoły.

<a id="cache_purge_refresh_profile"></a>

### `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

Mapuje znaną reprezentację na literalną wewnętrzną lokalizację proxy. Usunięcie dokładnego prefiksu z rozróżnianiem liter musi pozostawić URI źródła zaczynające się od `/`; query string i escape są zachowane. Moduł nie zgaduje hosta ani nie szuka `://` w kluczu. Cel wymaga `internal`, wyłączonego cache, przekazywania nagłówków/ciała wyłączonego, HEAD, niepustego literalnego Host i literalnych nagłówków reprezentacji. Nagłówki warunkowe używają dokładnie `$cache_purge_refresh_etag` i `$cache_purge_refresh_last_modified`; `$cache_purge_refresh_uri` dostarcza URI do `proxy_pass`. Te trzy czyste zmienne mają sens tylko w sondach. Cel sprawdzany jest w preaccess, także po przekierowaniu wewnętrznym. Administrator gwarantuje tę samą reprezentację źródła co klucz; Vary pomija się, bo natywne metadane nie zachowują pierwotnych nagłówków.

Ustaw `proxy_intercept_errors off` w lokalizacji sondy. Dziedziczona interceptacja jest odrzucana, aby błąd źródła nie zmienił się w sukces invalidujący.

<a id="cache_purge_refresh_timeout"></a>

### `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

Ogranicza rozpoczynanie skanów/sond strony REFRESH, wraz z oczekiwaniem na skaner workera. Upływ czasu zatrzymuje nową pracę i oznacza odpowiedź niepełną. Aktywny HEAD kończy się według jawnie ustawionych `proxy_connect_timeout`, `proxy_send_timeout` i `proxy_read_timeout`. To nie twardy termin odpowiedzi i nie przerywa syscall. Rozłączenie/łagodne zamknięcie zatrzymuje nowe zgłoszenia, zachowując pamięć aktywnego wątku do natywnego zakończenia.

---

<a id="partial-key-purge"></a>

## Usuwanie według części klucza

Dodaj `*` do **obliczonego klucza pamięci podręcznej**, aby dopasować prefiks bez rozróżniania wielkości liter. Klucze dokładne i Vary nadal ją rozróżniają. Prefiks fizycznego czyszczenia może mieć do 511 bajtów bez końcowej `*`. Dłuższy prefiks powoduje jeden wpis w dzienniku i `400 invalid_selector` bez przeglądania plików.

Z `cache_purge_key` dopasowanie dotyczy wyrażenia logicznego, maksymalnie 8096 bajtów z końcową `*`.

Dla wyrażenia kończącego się `$uri` to żądanie wybiera prefiks URI:

```http
PURGE /images/header*
```

Gwiazdka musi kończyć obliczony klucz, nie tylko ścieżkę URI. Uwzględnij parametry lub zmienne na końcu `proxy_cache_key`. Samo `*` przechodzi wszystkie kwalifikujące się wpisy.

---

<a id="generation-invalidation"></a>

## Invalidacja generacji

W powiązanej przestrzeni PURGE publikuje nową generację i planuje zdeduplikowane przejście starych przed odpowiedzią `200` ze statusem `invalidated`. Przyjęcie nie otwiera ani nie usuwa plików. Nowe żądanie pierwszy raz obliczające klucz po publikacji nie trafia w poprzednią generację. Żądanie przypina generację dla każdej przestrzeni/klucza, także przez przekierowania wewnętrzne; zaczęta odpowiedź może skończyć się, lecz późny zapis nie stanie się HIT dla nowych żądań. Dokładne dopasowanie rozróżnia litery; prefiksy ignorują wielkość ASCII. `purge_all` zmienia całą przestrzeń, włącznie z celowo współdzielącymi ją ścieżkami. Fizyczne czyszczenie kolejkuje się dla wybranej ścieżki; inne korzystają z wygaśnięcia rdzenia lub kolejnej purgi.

Przyjęcie invalidacji nie zwalnia natychmiast dysku. Późny stary zapis po przejściu pozostaje niedostępny i znika podczas późniejszego czyszczenia lub wygaśnięcia inactive. Pod presją SHM reguły zapadają się do nowej bazy przestrzeni: dodatkowe klucze stają się zimne, zamiast przywracać invalidowane. Kolejna invalidacja odzyskuje do 64 odłączonych węzłów trie. Częste purgi jednego klucza nie powiększają drzewa bez końca. Koszt zależy od ograniczonej długości klucza i indeksu, nie liczby plików; brak gwarancji czasu rzeczywistego O(1) lub braku zagłodzenia pod przeciążeniem. Przyjęcie może czekać. Po invalidacji użyj `proxy_cache_lock` i odpowiedników przeciw lawinie GET.

<a id="conditional-refresh"></a>

## Warunkowy REFRESH

REFRESH sprawdza nadal przechowywane obiekty przed upływem TTL; nie rozgrzewa cache ani nie przedłuża TTL przy `304`. Uzupełnia `proxy_cache_revalidate` i `proxy_cache_background_update`, obsługujące zwykły ruch wygasłych obiektów. Źródło ignorujące walidatory i zawsze zwracające `200` zmienia go w invalidację plus HEAD. Kandydaci to aktualne, poprawne wpisy na kanonicznych ścieżkach hash. Podmiana fizyczna w tej samej generacji może być ostrożnościowo invalidowana; porównanie generacji blokuje invalidację nowszego PURGE przez stary HEAD.

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

Worker ma najwyżej jedną aktywną stronę i sondę; wspólny limit wynosi 16 sond między workerami. Inne strony czekają bez tablic kandydatów. Strona skanuje wspólnym kursorem, zachowuje do 32 kandydatów, po czym uruchamia kolejno natywne HEAD. Ogranicza pamięć i liczbę podżądań dzielących główny pool. Identycznych REFRESH nie deduplikuje się; współdzielą limit równoległości. Ich invalidacje używają wspólnej zdeduplikowanej kolejki PURGE.

Zacznij bez kursora, potem wysyłaj `X-Cache-Purge-Cursor: <next_cursor>` do tego samego endpointu z tym samym selektorem/profilem, aż `complete` będzie prawdziwe. Kursor to ostatnia ukończona 32-znakowa nazwa hex; błędne/powtórzone nagłówki dają `400`. `scan_complete` oznacza udaną enumerację; `complete` również brak następnej strony i koniec wybranych decyzji. `timed_out` i `scan_error` rozróżniają przerwanie/błąd. `undispatched` liczy wybrane nieukończone; niepełny skan to tylko dolna granica, nie rozmiar cache. Przy niepełnym skanie sond nie uruchamia się, wyniki są puste, wejściowy kursor bez zmian. Wynik ujawnia hashe i surowe statusy upstream, nie klucze ani poświadczenia.

Każda strona ponownie skanuje katalog po kolejne hashe: ograniczona pamięć za powtarzane I/O, `O(files × pages)` dla całości. Paginacja jest słabo spójna przy równoczesnych dodaniach/usunięciach; zacznij od nowa, jeśli potrzebujesz późniejszego pełnego obrazu. Dostosuj selektory i timeouty. Nie nadaje się do częstego skanowania bardzo dużego aktywnego cache.

<a id="generation-and-refresh-example"></a>

### Przykład generacji i REFRESH

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

```sh
curl -X PURGE 'http://127.0.0.1:8080/objects/item'
curl -X REFRESH -H 'X-Cache-Key: origin.test/objects/*'      'http://127.0.0.1:8080/admin/refresh'
# Repeat with X-Cache-Purge-Cursor set to the returned next_cursor.
```

<a id="sample-configurations"></a>

## Przykładowe konfiguracje

<a id="basic--inline-purge-method"></a>

### Podstawowa — metoda PURGE w tej samej lokalizacji

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

### Podstawowa — oddzielna lokalizacja PURGE

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

### Włączona kolejka w tle

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

### Usuwanie wszystkich wpisów

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

### JSON z nadpisaniem na poziomie location

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

### Usuwanie z obsługą Vary

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

Po usunięciu klucza automatycznie usuwa się też wszystkie warianty gzip/Vary z innych ścieżek.

---

<a id="performance-tuning"></a>

## Strojenie wydajności

`queue_size` ogranicza niezakończoną pracę, `batch_size` grupuje zadania workera, `throttle_ms` rozsuwa grupy. Mierz opóźnienia, rywalizację o dysk i wiek kolejki przed zmianą i po niej. Większa kolejka nie zwiększa tempa opróżniania.

To konfiguracje startowe, nie deklaracje wydajności sprzętu. Nagłówki pomijają wspólny prefiks `cache_purge_`.

| Obciążenie / budżet dysku | `queue_size` | `batch_size` | `throttle_ms` |
|---|---:|---:|---:|
| Ograniczony host lub dysk | 512 | 5 | 25ms |
| Ogólny punkt wyjścia | 2048 | 20 | 10ms |
| Szybki dysk ze zmierzonym zapasem | 8192 | 50 | 5ms |
| Krótkie skoki, ostrożne opróżnianie | 8192 | 5 | 50ms |

Przykład konfiguracji ogólnej:

```nginx
cache_purge_background_queue on;
cache_purge_queue_size        2048;
cache_purge_batch_size        20;
cache_purge_throttle_ms       10ms;
```

Większa kolejka pochłania skok, ale stały napływ ponad tempo kończenia ją zapełni. Dodatkowy zapas szacuj jako `max(0, arrival_rate - completion_rate) * burst_seconds` zadań, używając zadań/s po deduplikacji. Zostaw margines na nierówne czasy i istniejące zaległości. Czekanie dodaje otwarte żądania/połączenia: uwzględnij ich pamięć i deskryptory przy nasyceniu dysku.

<a id="memory-sizing"></a>

### Wymiarowanie pamięci

Rozmiar strefy wylicza się z natywnych struktur:

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

| Składnik | Pamięć zarezerwowana lub użyta |
|---|---|
| Rezerwa zadań | `Q * S`, wraz z zaokrągleniem slab i osadzonymi nazwami klucza/puli |
| Metryki jednej ścieżki | `R * NGX_MAX_PROCESSES` plus wyrównanie; 192 KiB dla odstępu 192 bajty i 1024 slotów |
| Sterowanie kolejką | `H`, w tym właściciele procesów i tablice epok |
| Rezerwa ścieżek | `M`, w tym nazwy, korzenie rbtree, wyrównanie i tablice metryk |
| Minimalna strefa | Osiem natywnych stron |

Dla zmierzonego LP64 (`I=184`, `H=20576`, rekord ścieżki 104 bajty, statystyk 144), linii cache 64 bajty, stron 4 KiB i jednej ścieżki 20 bajtów:

| Pojemność | Bajty strefy | MiB strefy |
|---:|---:|---:|
| 512 | 1654784 | 1.58 |
| 2048 | 5373952 | 5.12 |
| 8192 | 20242432 | 19.30 |

Zadanie faktycznie zajmuje `I + index_bytes + 1 + pool_name_bytes` przed slab. Osadzony indeks ma bajt akcji i historyczny klucz albo nazwę przestrzeni generacji. Historyczne `purge_all` przechowuje sam znacznik. Pojemność ogranicza razem alokacje oczekujące i aktywne. Rozmiary natywne, linia cache, `NGX_MAX_PROCESSES` i ścieżki wyznaczają wynik; Windows/32 bity różnią się. Rekordy ochrony reload dodają SHM. Alokacja może zawieść przed limitem zadań.

Pamięć procesu jest osobna. Aktywne przejście ma stały bufor klucza, wielokrotnie używany bufor ścieżki i stałą ramkę na otwarty katalog. Ramki przechowują długości prefiksów bez kopiowania przodków: `O(depth + longest_path)`, niezależnie od plików. Strumienie katalogów mają bufory platformy. Czekające żądania mają mały kontekst timera/sprzątania, bez kursora. Stosy wątków i pule rdzenia kosztują osobno.

Przestrzeń ma własny rozmiar SHM. Skaner REFRESH zajmuje stałe 283864 bajty bufora/kontekstu LP64 (32 wpisy), ramki oraz do 32 podżądań w puli HTTP. Koniec strony zwalnia żądanie; kontynuacja rozpoczyna nowe. Dodatkowe oczekiwania nie alokują tablic kandydatów.

<a id="throughput-and-latency"></a>

### Przepustowość i opóźnienia

Dla stale zajętego workera przydatne przybliżenie:

```text
B = tasks completed in a batch
T = throttle delay in milliseconds
D = elapsed batch processing time in milliseconds, including slice waits
rate_per_worker ~= 1000 * B / (D + T) tasks/s
```

Pominięcie `D` daje pułap samego planowania `1000 * B / T`: domyślnie 1000 zadań/s na worker. To nie zmierzony wynik ani tempo usuwania plików. Dziesięć zadań zajmujących łącznie 200 ms przy pauzie 10 ms daje około 47,6 zadań/s. Workery współdzielą dysk; mnożenie przez ich liczbę nie gwarantuje przepustowości. Bezczynny worker może czekać do `10 * T` plus opóźnienie pętli przed sprawdzeniem nowej pracy.

Zmniejsz partię lub wydłuż pauzę, aby ograniczyć pracę między przerwami. Do mniejszych opóźnień podczas I/O użyj opcjonalnej puli. Żaden parametr nie ogranicza jednej syscall ani nie dzieli zadania wątku na partie.

---

<a id="monitoring-and-debugging"></a>

## Monitorowanie i debugowanie

[Endpoint statusu](#cache_purge_queue_status) pokazuje oczekiwania, aktywne zadania, przyjęcia, duplikaty, usunięcia i HIT/MISS. `202 Accepted` potwierdza przyjęcie/scalenie, nie usunięcie ani natychmiastową logiczną invalidację. Mierz opóźnienia razem z wiekiem kolejki i rywalizacją o dysk.

| Poziom | Warunek |
|---|---|
| `warn` | Zbyt długi klucz, usunięta pula lub ograniczony parametr |
| `error` | Odmowa dostępu, błąd wyszukania/profilu REFRESH, wyczerpane ponowienia; błąd puli rdzenia/odczytu Windows |
| `crit` | Błąd otwarcia/stat/usunięcia lub katalogu; odczytu POSIX rdzenia |
| `alert` | Błąd zamknięcia pliku |
| `emerg` | Błędna konfiguracja, niezgodna SHM, zmiana rozmiaru lub błąd alokacji metadanych reload |
| `debug` | Dodatkowy kontekst żądania/przejścia |

Diagnostyka używa natywnego logowania NGINX z prefiksem `ngx_cache_purge: `. NGINX dodaje poziom, czas, ID procesu/wątku, errno i kontekst żądania/konfiguracji. Błędy plików podają rzeczywistą operację i ścieżkę w cudzysłowie; przestrzeń/REFRESH wskazują obiekt i przyczynę odmowy. Używaj standardowego parsera error log i filtruj prefiks.

Debug używa `NGX_LOG_DEBUG_HTTP`; wymaga kompilacji `--with-debug` i `error_log /var/log/nginx/error.log debug;`.

Presję kolejki pokazują metryki oczekiwania, nie ostrzeżenie na każdą próbę. Zniknięty plik jest oczekiwanym wyścigiem bez diagnostyki modułu. Pomocnicze funkcje rdzenia — parser, alokator, I/O, pule — zachowują własny format. Przy błędach analizuj pełny log.

```bash
tail -f /var/log/nginx/error.log | grep 'ngx_cache_purge:'
```

Sprawdzone komunikaty opisują [testy logowania](../../t/TESTING.md#logging-contracts).

---

<a id="troubleshooting"></a>

## Rozwiązywanie problemów

| Objaw | Kontrola lub regulacja |
|---|---|
| Opóźnienie workera przy dużym przejściu | Włącz kolejkę i opcjonalną pulę dla I/O. Dokładne/Vary i praca bez kolejki pozostają synchroniczne. |
| Rosną oczekiwania na przyjęcie | Sprawdź napływ, wiek, pulę i dysk. Większa pojemność pochłania skoki; zmiana wymaga restartu. |
| Wysokie iowait | Zmniejsz `cache_purge_batch_size`, zwiększ `cache_purge_throttle_ms`, ogranicz napływ PURGE. |
| Wolne opróżnianie | Zmierz czas zadań i rywalizację przed zwiększeniem partii lub skróceniem pauzy. |
| Klient oczekuje dawnych `412`/`404` | Usuń `cache_purge_legacy_status`; obsługuj `200` z `Cache-Purge-Code: no_match`. |
| Pozostają warianty Vary | Włącz `cache_purge_vary_aware`; sprawdź główny wynik i limit 511 bajtów. |
| Przyjęta praca wygląda na niepełną | Sprawdź aktywne zadania, odzyskiwanie i błędy. `202` nie potwierdza końca; podejrzenie uszkodzenia SHM wymaga restartu. |

---

<a id="testing"></a>

## Testowanie

Zbuduj i uruchom cały zestaw w Dockerze:

```sh
make test-all
NGINX_VERSION=1.31.6 make test-all
```

Z katalogu `t/` nadal dostępne są istniejące usługi Compose:

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

Dla wtyczki Compose zastąp `docker-compose` przez `docker compose`; Makefile wykrywa oba. `nginx-test` i `make test-all` uruchamiają siedem zestawów `t/*.t` przez ten sam `prove` co CI. Obraz integracyjny zawiera natywne testy pomocnicze/struktur i wykonanie z pulą. Rygorystyczne kompilacje bez wątków testują tryb przyrostowy. Każdy nieudany test oznacza niepowodzenie polecenia.

Rygorystyczne kompilacje, sanitizery, Valgrind, platformy i raporty: [`t/TESTING.md`](../../t/TESTING.md). Wszystkie zadania GitHub są w `ci.yml`.

---

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

---

<a id="temporary-directories"></a>

## Katalogi tymczasowe

Przejścia pomijają skonfigurowane katalogi tymczasowe pod korzeniem, porównując całe komponenty ścieżek. Nazwy jak `proxy_temp` nie są zastrzeżone bez konfiguracji tymczasowej. Tylko zwykłe pliki o dokładnie 32 znakach hex w nazwie mogą być usuwane. Numeryczne i sufiksowe pliki tymczasowe zapisu są pomijane, także z `use_temp_path=off`.

Każde przejście sprawdza wersję natywnego nagłówka, offsety, pełny znacznik `KEY: `, CRC32 i hash klucza/wariantu nazwy, z limitem klucza 8192 bajty. Pliki błędne, ucięte, obcej wersji lub niedopasowane pomija się również w purge-all. Długości ETag/Vary równe maksimum rdzenia są akceptowane. Kontrola formatu nie uwierzytelnia złośliwego pliku.

Korzeń może być symlinkiem lub junction Windows. Potomne symlinki/reparse points są pomijane. POSIX z pełną detekcją operacji względem deskryptora używa dla potomków `ngx_openat_file`/`ngx_file_at_info` rdzenia z no-follow; usuwa względem otwartego rodzica. Blokuje przekierowanie przez podmieniony symlink potomny, ale nie jest sandboxem przeciw przeniesieniu już otwartego katalogu. Windows/stare Unix używają pełnych ścieżek i kontroli linków, zachowując wyścig podmiany. Historyczne dokładne wyszukanie/usuwanie też działa pełnymi ścieżkami. Utrzymuj korzenie prywatne bez niezaufanych zapisujących.

Konfiguruj ścieżki w spójnej postaci kanonicznej. Porównanie nie rozwiązuje aliasów symlinków, montowań czy Unicode. Zadania zachowują ochronę; nowszy duplikat aktualizuje oczekujące lub następny przebieg. Aktywne przejście zachowuje poprzednią; nie jest to suma wszystkich żywych generacji workerów. Nie zmieniaj przeznaczenia ścieżek z powiązanymi oczekującymi/aktywnymi zadaniami.

---

<a id="security"></a>

## Bezpieczeństwo

- Ogranicz endpointy PURGE przez `allow` / `deny` lub wcześniejsze uwierzytelnianie. Nigdy nie wystawiaj publicznie.
- Dobierz `cache_purge_queue_size` do ruchu, ograniczając SHM przy zalewie żądań.
- Rozważ `limit_req` dla purgi niezależnie od zwykłego ruchu.

---

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

---

<a id="see-also"></a>

## Zobacz też

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [Dokumentacja nginx](https://nginx.org/en/docs/)
