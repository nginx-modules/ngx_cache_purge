# ngx_cache_purge

[README](README.pl.md) · [Dyrektywy](DIRECTIVES.pl.md)

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

<a id="reading-cache-files"></a>

### Odczyt plików cache w trybie generacji

Przeglądarka cache, narzędzie wstępnego ładowania lub raport użycia nie określi aktualności generacji na podstawie samych plików: po unieważnieniu mogą pozostać nieaktualne pliki, nawet bez zastępstwa. Wiersz `KEY:` pliku generacji ma postać `p4:<namespace>:<epoch>:<generation>:<logical key>`. [Endpoint statusu](DIRECTIVES.pl.md#cache_purge_queue_status) udostępnia stan przestrzeni nazw. Bieżąca generacja klucza to `baseline`, chyba że podniosła ją reguła dokładna lub prefiksowa; odpowiedź nie zawiera tych reguł.

| Stan przestrzeni nazw pliku | Ocena |
|---|---|
| Brak przestrzeni nazw na liście | `unknown` |
| Epoka pliku różni się od `epoch` | `stale` |
| Epoka jest zgodna; generacja pliku jest mniejsza niż `baseline` | `stale` |
| Epoka jest zgodna; generacja wynosi co najmniej `baseline`; `rules` ma wartość `false` | `live` |
| Epoka jest zgodna; generacja wynosi co najmniej `baseline`; `rules` ma wartość `true` | `unknown` |

**live** oznacza tu wyłącznie, że plik nie jest nieaktualny według generacji przestrzeni nazw; nie potwierdza świeżości treści, ważności TTL ani HIT w cache NGINX. Przy `namespaces_complete=false` zwrócone wpisy nadal nadają się do użycia, a pominięte przestrzenie mają stan nieznany.

Plik o wyższej generacji z tą samą przestrzenią nazw, epoką i kluczem logicznym dowodzi, że plik o niższej generacji jest nieaktualny: bieżąca generacja nigdy nie maleje w obrębie epoki. W pozostałych przypadkach zachowaj ocenę unknown.

Odczytaj status przed skanowaniem i po nim, łącząc wpisy według nazwy przestrzeni. Używaj ocen opartych na statusie dla przestrzeni tylko wtedy, gdy występuje w obu odpowiedziach, a `epoch`, `baseline` i `serial` są niezmienione. Brak lub zmiana stanu unieważnia jej oceny. Epoki porównuj wyłącznie pod względem równości. Te obserwacje nie zapobiegają późniejszym unieważnieniom.

Pusta kolejka czyszczenia nie dowodzi aktualności. Żądanie rozpoczęte przed unieważnieniem może zapisać plik starej generacji już po zakończeniu czyszczenia. Plik pozostanie do następnego czyszczenia lub usunięcia przez natywny mechanizm cache. Czyszczenie obejmuje też tylko ścieżkę cache location obsługującej PURGE, podczas gdy przestrzeń nazw może obejmować wiele ścieżek.

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

[Endpoint statusu](DIRECTIVES.pl.md#cache_purge_queue_status) pokazuje oczekiwania, aktywne zadania, przyjęcia, duplikaty, usunięcia i HIT/MISS. `202 Accepted` potwierdza przyjęcie/scalenie, nie usunięcie ani natychmiastową logiczną invalidację. Mierz opóźnienia razem z wiekiem kolejki i rywalizacją o dysk.

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

<a id="temporary-directories"></a>

## Katalogi tymczasowe

Przejścia pomijają skonfigurowane katalogi tymczasowe pod korzeniem, porównując całe komponenty ścieżek. Nazwy jak `proxy_temp` nie są zastrzeżone bez konfiguracji tymczasowej. Tylko zwykłe pliki o dokładnie 32 znakach hex w nazwie mogą być usuwane. Numeryczne i sufiksowe pliki tymczasowe zapisu są pomijane, także z `use_temp_path=off`.

Każde przejście sprawdza wersję natywnego nagłówka, offsety, pełny znacznik `KEY: `, CRC32 i hash klucza/wariantu nazwy, z limitem klucza 8192 bajty. Pliki błędne, ucięte, obcej wersji lub niedopasowane pomija się również w purge-all. Długości ETag/Vary równe maksimum rdzenia są akceptowane. Kontrola formatu nie uwierzytelnia złośliwego pliku.

Korzeń może być symlinkiem lub junction Windows. Potomne symlinki/reparse points są pomijane. POSIX z pełną detekcją operacji względem deskryptora używa dla potomków `ngx_openat_file`/`ngx_file_at_info` rdzenia z no-follow; usuwa względem otwartego rodzica. Blokuje przekierowanie przez podmieniony symlink potomny, ale nie jest sandboxem przeciw przeniesieniu już otwartego katalogu. Windows/stare Unix używają pełnych ścieżek i kontroli linków, zachowując wyścig podmiany. Historyczne dokładne wyszukanie/usuwanie też działa pełnymi ścieżkami. Utrzymuj korzenie prywatne bez niezaufanych zapisujących.

Konfiguruj ścieżki w spójnej postaci kanonicznej. Porównanie nie rozwiązuje aliasów symlinków, montowań czy Unicode. Zadania zachowują ochronę; nowszy duplikat aktualizuje oczekujące lub następny przebieg. Aktywne przejście zachowuje poprzednią; nie jest to suma wszystkich żywych generacji workerów. Nie zmieniaj przeznaczenia ścieżek z powiązanymi oczekującymi/aktywnymi zadaniami.

---

<a id="testing"></a>

## Testowanie

[Mapa pokrycia](../TEST_COVERAGE.md) (po angielsku) wskazuje sprawdzone przypadki i pozostałe luki.

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

Rygorystyczne kompilacje, sanitizery, Valgrind, platformy i raporty: [`t/TESTING.md`](../../t/TESTING.md). Wszystkie zadania GitHub są w `build-test-release.yml`.
