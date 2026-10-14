<a id="ngx_cache_purge"></a>

# ngx_cache_purge

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

`FastCGI`, `proxy`, `SCGI`, `uWSGI` 캐시를 삭제하는 NGINX 모듈입니다. 버전 4.0은 기존 인터페이스를 유지하면서, 명시적으로 활성화하는 세대 무효화와 지연 파일 삭제, proxy 캐시용으로 처리량을 제한한 조건부 REFRESH를 추가합니다.

---

<a id="contents"></a>

## 목차

- [기능](#features)
- [호환성](#compatibility)
- [설치](#installation)
- [실행 모델](#execution-model)
- [지시어](#directives)
- [부분 키 삭제](#partial-key-purge)
- [세대 무효화](#generation-invalidation)
- [조건부 REFRESH](#conditional-refresh)
- [설정 예제](#sample-configurations)
- [성능 조정](#performance-tuning)
- [모니터링과 디버깅](#monitoring-and-debugging)
- [문제 해결](#troubleshooting)
- [테스트](#testing)
- [마이그레이션](#migration)
- [임시 디렉터리](#temporary-directories)
- [보안](#security)
- [라이선스](#license)

---

<a id="features"></a>

## 기능

- **인라인 삭제** — IP 접근 제어를 적용하는 전용 HTTP 메서드(`PURGE`)
- **별도 삭제 location** — `proxy_pass` 없이 정규식으로 캡처한 키를 삭제하는 영역/키 구문 `proxy_cache_purge zone key`
- **와일드카드 / 부분 삭제** — 끝의 `*`로 캐시 디렉터리를 순회하여 일치하는 항목 삭제
- **`purge_all`** — 요청 하나로 캐시 영역의 모든 항목 삭제
- **백그라운드 큐** — 제한된 이벤트 루프 조각 또는 선택적 NGINX 스레드 풀로 지연 순회; 작업 배치와 대기 시간 설정 가능
- **Vary 인식 삭제** — 정확한 키 삭제 후 같은 키를 공유하는 gzip/Vary 파일 변형도 삭제
- **응답 형식** — `json`(기본값), `html`, `xml`, `text`, `none`
- **이름 있는 실행 풀** — 풀 선택 상속, 스레드 없는 빌드와 Windows에서는 점진적 실행
- **활성 작업 중복 제거** — 인덱스 작업 하나와, 순회 중 받은 반복 삭제에 대해 최대 하나의 대기 재실행
- **상태** — 공유 큐 뮤텍스를 잡지 않고 읽는 캐시별 큐, 삭제, HIT/MISS 지표
- **세대 키** — 요청 처리 경로의 디스크 I/O 없이 정확한 키·접두사·네임스페이스 무효화; 늦게 끝난 이전 쓰기는 접근 불가 상태 유지
- **조건부 REFRESH** — 명시적 HEAD 프로필, 공유 검사 한도, 32개 항목 단위의 연속 페이지

---

<a id="compatibility"></a>

## 호환성

표는 CI에 설정된 대상을 나타냅니다. 성공한 작업은 해당 소스와 빌드 설정을 검증하며, 같은 계열의 모든 릴리스를 검증하지는 않습니다.

| NGINX | CI: push / PR | CI: + full |
|---|---|---|
| 1.30.5, 1.31.6 | `test-matrix` | — |
| 1.31.6 | `build-compatibility` (GCC/Clang), `sanitizer`, `native-macos`, `native-windows` (x64/x86) | `memory-leak-test` (Valgrind) |
| 1.7.9 | `build-compatibility` (GCC) | — |
| 1.7.11, 1.7.12 | `build-compatibility` (GCC) | — |
| 1.20.2, 1.26.3, 1.28.3 | — | `test-matrix` |
| 1.11.4, 1.11.5 | — | `build-compatibility` (GCC) |

Linux 작업은 로컬 실행과 같은 Docker 이미지 및 테스트 진입점을 사용합니다. 네이티브 macOS와 Windows 작업은 각 플랫폼의 파일·프로세스 API를 검사합니다. Windows는 MSVC로 빌드하여 `nginx.exe`의 HTTP, 경로, reload를 검사합니다. POSIX 프로세스/시그널 시나리오는 Linux와 macOS에서 실행됩니다. Windows에서는 이 시나리오와 선택적 NGINX 스레드 풀 경로를 실행하지 않습니다.

`--with-threads`가 없으면 점진적 순회를 사용합니다. NGINX 1.7.12 이전의 epoll 빌드에서도 코어 알림의 경쟁 조건을 피하기 위해 이 모드를 사용합니다. BSD 네이티브 CI는 없습니다. 일반 실행은 현재 stable/mainline과 API 경계를 검사하고 확장 실행은 이전 버전과 Valgrind를 추가합니다. 문서만 변경하면 빌드를 생략합니다. 일정과 명령은 [테스트 문서](../../t/TESTING.md)에 있습니다.

코드는 설정, 핸들러, 큐, 순회, 무효화, REFRESH, 응답으로 나뉩니다. 공통 경로 함수는 `ngx_cache_purge.h`, 플랫폼 연산은 POSIX/Win32 헤더에 있습니다. `generation.*`는 `invalidation.*`로 이름을 바꾸었으며 내부의 세대 개념은 여전히 키 버전을 나타냅니다. [구조 설명](../ARCHITECTURE.md)을 참조하십시오.

모듈은 NGINX의 C89 호환 스타일과 코어의 이벤트, slab, rbtree, 원자적 API를 사용합니다. C11 원자 연산이나 새로운 파일 시스템 API가 필요하지 않습니다. 엄격한 GNU89 빌드는 선언 순서와 경고를 검사하며, 실제 시험한 컴파일러와 플랫폼을 결과에 기록합니다.

---

<a id="installation"></a>

## 설치

```bash
cd /path/to/nginx-source
./configure --add-module=/path/to/ngx_cache_purge
make
make install
```

동적 모듈:

```bash
./configure --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

---

<a id="execution-model"></a>

## 실행 모델

**작업**은 와일드카드 또는 `purge_all` 연산 하나입니다. **배치**는 worker가 대기하기 전 처리하는 작업 수입니다. **조각**은 점진적 디렉터리 순회의 제한된 한 단계이며, 한 작업에 여러 조각이 필요할 수 있습니다.

세대 키를 사용하면 캐시 경로와 네임스페이스마다 하나의 물리적 순회가 수락한 무효화를 합칩니다. REFRESH 페이지는 삭제 큐 작업이 아닌 별도 HTTP 연산입니다.

| 요청 및 설정 | 파일 순회 | HTTP 완료 |
|---|---|---|
| 기존 정확한 키, Vary 사용 여부 무관 | 요청 worker; Vary는 디렉터리 순회 추가 | 연산 이후 |
| 와일드카드 또는 `purge_all`, 백그라운드 큐 꺼짐 | 요청 worker에서 동기 순회 | 순회 이후 |
| 백그라운드 작업, 선택된 풀 | worker마다 한 번에 풀 작업 하나 | 수락 후 `202` |
| 백그라운드 작업, 사용 가능한 풀 없음 | worker 이벤트 루프에서 점진적 조각 | 큐 등록 후 `202` |
| 큐 용량/할당 소진 | 요청은 순회 없이 수락 대기 | 수락 후 `202` |
| 세대 PURGE, 모든 키 형태 | 지연 네임스페이스 순회; 요청에서 캐시 파일 열기/삭제 없음 | 게시와 수락 후 `200 invalidated` |
| REFRESH 엔드포인트 | 제한된 스캔과 최대 32개 순차 HEAD | 완료·연속 필드를 담은 `200` JSON |

세대 키가 없는 행은 기존 동작입니다. 용량을 기다리던 세대 요청은 수락 후 `202`가 아닌 `200`을 반환합니다.

점진적 조각은 디렉터리 반복 최대 64회 또는 경과 시간 5 ms를 관찰하면 멈춥니다. 건너뛴 항목과 디렉터리 끝도 포함됩니다. 개별 파일 시스템 호출 시간은 제한하지 못합니다.

백그라운드 파일 연산을 스레드로 옮기려면 `--with-threads`로 빌드하고 `http` 바깥 **main** 컨텍스트에 코어 풀을 정의하십시오:

```nginx
thread_pool default threads=2;
```

`cache_purge_thread_pool <name>`은 `http`, `server`, `location`에서 기존 이름 있는 풀을 선택합니다. 기본값 `default`를 선언 없이 참조하면 NGINX가 표준 설정으로 생성합니다. 사용자 지정 이름은 코어 `thread_pool`로 선언해야 합니다. 선택은 상속되며, 처음 수락한 요청이 합쳐진 작업의 실행자를 결정합니다. 각 worker는 자체 풀을 가집니다. 풀이 포화되면 작업의 수락 자리를 유지하며 10 ms 후 제출을 재시도하고, 동기 순회로 바꾸지 않습니다. 스레드 미지원, Windows, 단일 프로세스 모드, reload로 이전 작업의 풀이 제거된 경우에는 점진적 커서를 사용합니다.

REFRESH는 선택한 풀로 worker당 추가 스캐너 하나를 실행합니다. 네이티브 HTTP 하위 요청은 이벤트 루프에서 실행되며, I/O 스레드는 그 상태에 접근하지 않습니다.

큐와 인덱스의 구조 변경은 slab 뮤텍스로 보호합니다. 수락 및 백그라운드 콜백은 trylock과 타이머 재시도를 사용합니다. 대기 **및 실행 중** 작업은 경로별 코어 문자열 rbtree에 남으며, 전체 키 비교로 해시 충돌을 해결합니다. 경로의 미완료 작업 `Q`개에 대해 인덱스 연산은 `O(log Q)`, 캐시 경로 검색은 등록 경로 수에 선형입니다. `ngx_queue_t` 목록은 대기/활성 작업을 추적합니다. 실행 중 작업의 첫 중복은 재실행을 요청하고, 나머지는 이에 합쳐집니다. 현재 순회 후 재실행은 대기열 끝으로 가므로 나중 PURGE가 조용히 사라지지 않습니다. 계속 새 PURGE가 오면 추가 순회가 생길 수 있습니다. 최종 완료 후에만 수락 자리를 반환합니다. 파일 작업과 응답 포맷은 잠금 밖에서 실행합니다.

Rbtree는 물리적 정리 작업만 중복 제거합니다. 세대 규칙은 별도 SHM 영역의 제한된 바이트 trie에 있습니다. 엄격한 네임스페이스 순회의 인덱스 식별자는 기존 키 작업과 다르며, REFRESH 결정은 같은 정리 경로에 들어갑니다.

큐가 가득 차거나 작업 할당이 실패하면 HTTP 요청은 10 ms 재시도 타이머와 코어 연결 해제 처리를 사용하며 기다립니다. 요청과 정리 핸들러는 소유 worker에 남고 SHM에는 요청 포인터가 없습니다. HTTP/2·HTTP/3 연결 전체의 읽기를 끄지 않습니다. 큐 압력 때문에 429/503을 만들지 않으며, 이미 읽은 짧은 PURGE가 TCP Zero Window를 유발한다는 보장도 없습니다. 대기 요청은 메모리와 연결을 소비하므로 코어 타임아웃/수락 정책을 연결 예산에 맞추십시오.

수락한 작업은 일반 reload 후에도 키, 풀 이름, 임시 경로 보호 설정을 유지합니다. 새 중복 요청은 대기 작업 또는 다음 재실행의 보호를 갱신합니다. 새 수락을 꺼도 기존 작업은 보존합니다. 정상 종료 중 worker는 현재 순회를 끝내고 새 작업을 받지 않습니다. 빠른 종료는 종료 훅에서 순회를 끝내지 않고 미완료 점진적 작업을 대기열로 돌려보냅니다. 이미 제출한 스레드 작업의 수명은 코어 풀 종료가 관리합니다.

PID/슬롯 epoch로 작업 소유권을 식별합니다. worker는 1초 간격, 복구 순회당 최대 64개 활성 레코드를 확인해 소유자가 종료된 작업을 다시 큐에 넣습니다. 버려진 배정을 복구하지만 트랜잭션 저널은 아닙니다. 공유 목록/slab 갱신 중 충돌은 상태 손상과 재시작을 초래할 수 있고, 충돌 직전 삭제는 지표에서 빠질 수 있습니다. 대기 작업과 재실행에는 살아 있는 worker가 필요합니다. 완료가 중요하면 정지 전에 큐를 비우십시오. 큐는 영속적이지 않습니다.

내부 큐 구조는 업그레이드 ABI가 아닙니다. 구조가 다른 빌드로 교체할 때는 완료해야 할 작업을 먼저 끝내고 전체 재시작하십시오. 같은 빌드의 일반 reload는 영역을 재사용합니다.

삭제는 best effort입니다. `202`는 수락 확인이지 삭제 완료가 아니며, 완료된 순회도 원자적 캐시 스냅샷은 아닙니다. 동시 요청이 순회 도중/이후 항목을 만들 수 있고 파일 오류는 기록됩니다. 와일드카드/전체 순회는 모든 네이티브 노드를 동기 갱신하지 않고 파일을 삭제합니다. 정확한 키와 Vary 연산은 일치 노드를 무효화합니다.

실패한 백그라운드 순회는 100 ms, 1 s, 10 s 후 최대 세 번 재시도합니다. 지연 작업은 중복 제거/수락 자리를 유지하면서 다른 작업을 허용합니다. 새 PURGE는 재시도 창을 초기화합니다. 소진되면 작업을 제거하고 오류를 기록합니다. `cleanup.exhausted`를 확인하고 저장소를 고친 뒤 재시도하십시오. 물리적 정리가 실패해도 세대 무효화는 유효합니다. 사라진 파일은 예상한 경쟁이며, 잘못된 파일은 무한 재시도하지 않고 건너뜁니다.

---

<a id="directives"></a>

## 지시어

<a id="proxy_cache_purge"></a>

### `proxy_cache_purge`

```
Syntax:  proxy_cache_purge on | off;
         proxy_cache_purge <method> [purge_all] from all | <cidr> ...;
         proxy_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

**인라인 형식** (`from …`)은 proxy location에서 지정한 HTTP 메서드를 가로채 해당 캐시 항목을 삭제합니다. `on`은 `PURGE`의 축약형이고 `off`는 비활성화합니다. CIDR 목록으로 제한하거나 `from all`로 모두 허용합니다. `from` 앞의 `purge_all`은 URI와 무관하게 전체 영역을 비웁니다.

**별도 location 형식**(영역과 키 인수)은 삭제 location에서만 허용되며 보통 정규식으로 키를 캡처합니다. 이름으로 영역을 찾아 컴파일한 키 식을 삭제합니다. 같은 location의 `proxy_cache`, `proxy_pass`와 함께 쓸 수 없습니다.

<a id="fastcgi_cache_purge"></a>

### `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

`fastcgi_cache` / `fastcgi_cache_path` 영역용 `proxy_cache_purge`와 동등합니다.

<a id="scgi_cache_purge"></a>

### `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

`scgi_cache` / `scgi_cache_path` 영역용 `proxy_cache_purge`와 동등합니다.

<a id="uwsgi_cache_purge"></a>

### `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

`uwsgi_cache` / `uwsgi_cache_path` 영역용 `proxy_cache_purge`와 동등합니다.

<a id="cache_purge_response_type"></a>

### `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

성공한 PURGE 응답의 본문 형식을 선택하며 기본값은 JSON입니다. `Cache-Purge-Code`와 본문의 결과 코드는 일치합니다. 모듈 응답에는 `Cache-Control: no-store`가 포함되며 오류는 RFC 9457의 `application/problem+json`을 사용합니다. 텍스트 키는 형식에 맞게 이스케이프하고 바이너리 키는 인코딩을 명시한 Base64로 표현합니다.

`none`은 PURGE 본문을 생략합니다. 완료된 성공은 `204`, 수락된 백그라운드 작업은 `202`, 오류는 4xx/5xx를 반환합니다. 상태와 REFRESH 엔드포인트는 구조화된 JSON을 유지합니다. 코드, 필드, 제한은 [HTTP 응답 규약](../HTTP_RESPONSES.md)을 참조하십시오.

<a id="cache_purge_background_queue"></a>

### `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

와일드카드와 `purge_all`의 지연 처리를 활성화합니다. 큐 등록 또는 같은 대기/실행 작업과 병합 후 `202 Accepted`를 반환합니다. 정확한 키 삭제는 바뀌지 않습니다.

기존 와일드카드/전체 삭제를 제어하는 스위치입니다. `cache_purge_key` 바인딩은 이 스위치가 꺼져도 항상 지연 물리 정리와 논리적 `200`을 사용합니다.

큐가 가득 차거나 작업 할당이 실패하면 수락할 때까지 요청이 열린 채 기다리며 디렉터리를 순회하지 않습니다. 기존 대기/실행 작업의 중복은 용량이 찼어도 수락할 수 있습니다. 작은 요청 대기 상태 할당도 실패하면 `500`을 반환합니다. 연결 해제 정리는 타이머를 취소하고 대기 게이지를 해제합니다.

이 플래그와 `cache_purge_vary_aware`는 마지막으로 선언한 값을 사용합니다. 잘못된 값은 거부됩니다. `cache_purge_legacy_status`는 폐기되어 무시됩니다. 스레드와 수명 주기는 [실행 모델](#execution-model)을 참조하십시오.

<a id="cache_purge_queue_size"></a>

### `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

모든 캐시 경로에 걸친 미완료 작업 수의 상한입니다. 대기와 배정 작업 모두 용량을 사용합니다. 중복은 할당을 재사용하며 재실행도 메모리와 자리를 유지합니다. 다른 작업은 빈자리를 기다립니다.

백그라운드 처리나 상태 엔드포인트를 설정하면 공유 영역이 생깁니다. 반올림한 영역 크기가 같아도 용량 변경에는 재시작이 필요합니다. Reload는 수락한 작업을 보존하기 위해 크기 변경을 거부합니다. 캐시 루트 추가도 메모리 증가와 재시작을 요구할 수 있습니다. 반드시 끝나야 할 대기/활성 작업은 먼저 완료하십시오.

네임스페이스를 선언해도 작업 큐가 생성됩니다.

영역 크기는 작업, 경로 메타데이터, 정렬된 worker 지표를 포함합니다. 빌드와 경로 수에 따라 다릅니다. [메모리 산정](#memory-sizing)을 참고하십시오.

<a id="cache_purge_thread_pool"></a>

### `cache_purge_thread_pool`

```
Syntax:  cache_purge_thread_pool <name>;
Default: default
Context: http, server, location
```

일반 상속 규칙으로 수락한 백그라운드 작업의 실행자를 선택합니다. 예:

```nginx
thread_pool purge_io threads=2 max_queue=128;
http {
    cache_purge_background_queue on;
    cache_purge_thread_pool purge_io;
}
```

NGINX가 이름 있는 풀을 검증하고 네이티브 작업 이벤트로 완료를 전달합니다. 비공개 풀 구조나 완료 큐를 복제하지 않습니다. 스레드 지원 시 사용자 지정 풀은 존재해야 합니다. 스레드 미지원/Windows에서도 지시어는 허용되며 제한된 이벤트 루프 조각을 사용합니다. 선택한 풀이 차면 실행을 늦출 뿐 I/O를 루프로 돌리지 않습니다. 전용 풀은 스레드 스택과 문맥 전환 비용을 더합니다.

<a id="cache_purge_batch_size"></a>

### `cache_purge_batch_size`

```
Syntax:  cache_purge_batch_size <number>;
Default: 10
Minimum: 1
Maximum: 64
Context: http
```

각 worker가 대기 사이에 처리할 최대 작업 수입니다. 순회 조각과 스레드 완료 사이에도 진행 상태가 유지됩니다. 큐가 비면 배치가 끝납니다. 64 초과는 설정 경고와 함께 제한하며 0은 거부합니다.

파일 수나 콜백 시간이 아니라 작업 수를 제어합니다. 작은 값은 작업 사이 대기를 늘리지만 활성 스레드 순회를 중단하지 않습니다. 디스크 부하는 `cache_purge_throttle_ms`와 함께 조정하십시오.

<a id="cache_purge_throttle_ms"></a>

### `cache_purge_throttle_ms`

```
Syntax:  cache_purge_throttle_ms <time>;
Default: 10ms
Minimum: 1ms
Context: http
```

백그라운드 배치 간 지연입니다. `10ms`, `500ms`, `1s`, `"2s 500ms"` 같은 따옴표 내 복합 값을 허용합니다. 단위 없는 정수는 밀리초이므로 `10`과 `10ms`는 같습니다. 단위가 없으면 설정 경고가 발생하고 명시하면 피할 수 있습니다.

유휴 worker는 이 간격의 10배 후 다시 확인합니다. 미완료 조각과 사용 중 뮤텍스 재시도는 1 ms 타이머를 씁니다. 이는 최소 스케줄링 간격이지 마감 시간이 아닙니다. 0과 플랫폼의 안전한 타이머 범위를 넘는 값은 거부됩니다.

수락, 풀 압력, 지연 정리 재시도는 10 ms 폴링을 사용합니다.

<a id="cache_purge_queue_status"></a>

### `cache_purge_queue_status`

```
Syntax:  cache_purge_queue_status <cache_path>;
Default: not configured
Context: location
```

GET/HEAD로 `Cache-Control: no-store`가 있는 JSON 스냅샷을 제공합니다. 경로는 설정된 캐시 루트여야 합니다. 알 수 없는 경로는 백그라운드 처리가 꺼져도 설정 로딩을 실패시킵니다. 엔드포인트 접근을 제한하십시오:

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

`queue`는 모든 경로의 공유 큐를 나타냅니다:

| 필드 | 형식 / 단위 | 의미 |
|---|---|---|
| `size` | 정수 / 작업 | worker를 기다리는 작업 |
| `running` | 정수 / 작업 | 풀 대기를 포함한 배정 작업 |
| `max_size` | 정수 / 작업 | 대기와 배정 작업의 합산 용량 |
| `wait_time` | 정수 / 밀리초 | 가장 오래된 대기 시간; 비어 있으면 0 |
| `full` | 불리언 | 표본의 대기·배정 합계가 용량에 도달 |

`cache`는 이 location이 선택한 캐시 루트를 나타냅니다:

| 필드 | 형식 / 단위 | 의미 |
|---|---|---|
| `pending.total` | 정수 / 작업 | 이 캐시의 대기 작업 |
| `pending.full` | 정수 / 작업 | `pending.total`에 포함된 대기 `purge_all` |
| `active` | 정수 / 작업 | 완료 집계가 끝날 때까지의 배정 작업 |
| `requests.overflow` | 정수 / 요청 | 잠금/할당 압력을 포함해 수락 대기에 들어간 요청 |
| `requests.deduplicated` | 정수 / 요청 | 대기 또는 실행 작업에 합쳐진 요청 |
| `requests.waiting` | 정수 / 요청 | 이 캐시의 현재 수락 대기 요청 |
| `files.deleted` | 정수 / 파일 | 순회 완료 시 보고된 성공한 백그라운드 삭제 |
| `files.errors` | 정수 / 오류 | 이미 없는 파일을 제외한 백그라운드 삭제 실패 |
| `files.bytes` | 정수 / 바이트 | 삭제된 파일의 논리 크기 추정값 |
| `responses.hit`, `responses.miss` | 정수 / 주 요청 | log 단계에서 관찰한 코어 HIT/MISS 결정 |
| `responses.hit_ratio` | 수 / 0..1 | HIT / (HIT + MISS), 관찰이 없으면 0 |
| `logical.invalidations` | 정수 / 결정 | 중복과 REFRESH를 포함해 게시된 세대 무효화 |
| `cleanup.scan_errors` | 정수 / 오류 | 백그라운드 메타데이터/열기/읽기/닫기/할당 오류 |
| `cleanup.retries` | 정수 / 순회 | 정리 실패로 예약한 추가 순회 |
| `cleanup.exhausted` | 정수 / 작업 | 세 번의 재시도 실패 후 제거한 작업 |

`pending.full`은 전체 캐시 연산을 셉니다. 키가 `*`인 와일드카드는 별도 유형입니다. `active`는 풀 대기를 포함하며 `queue.size`, `pending.total`에는 빠지지만 수락 용량은 사용합니다.

합쳐진 네임스페이스 순회에서 `pending.full`은 처음 수락한 작업 분류를 따릅니다. REFRESH 스캔/검사는 PURGE 큐 깊이나 삭제 오류에 포함하지 않습니다. 지연 재시도를 뒤로 이동하면 `wait_time`이 최대 대기 시간 대신 표본 큐 선두의 나이일 수 있습니다.

카운터는 프로세스 슬롯당 이벤트 루프의 단일 작성자가 갱신하며 시작과 간격을 캐시 라인에 정렬합니다. 스레드는 완료 이벤트로 결과를 돌려줍니다. 상태 읽기는 큐 뮤텍스나 0 증가를 포함한 atomic fetch-add를 사용하지 않습니다. 64비트는 일반 정렬 volatile 읽기/쓰기를, 32비트는 제한된 시퀀스 검증으로 64비트 읽기를 보호합니다. 바쁜 샤드는 표본 하나에서 빠질 수 있습니다. 필드는 근사치이고 worker 전체의 트랜잭션이 아닙니다. 수락은 모니터링 값이 아닌 잠긴 제어 상태를 사용합니다.

누적 카운터는 부호 없는 64비트 최댓값에서 포화되고 일반 reload 후 유지되며 재시작 시 초기화됩니다. 대기 게이지는 요청 정리를 따릅니다. 동기 연산과 코어 cache manager 삭제는 백그라운드 합계에서 제외됩니다. 바이트는 파일 길이 추정값이지 블록 수나 확보된 공간의 증명이 아닙니다. 열린 독자와 동시 쓰기가 결과를 바꿉니다. 부분 순회 결과도 집계하나 충돌한 worker 결과는 빠질 수 있습니다. 감사 로그가 아닙니다.

HIT/MISS는 네이티브 캐시 결정을 가진 주 요청만 셉니다. upstream 부재, 캐시 상태 0, BYPASS, EXPIRED, STALE, UPDATING, REVALIDATED는 비율에서 제외합니다. 원래 캐시하지 않는 요청을 MISS로 세지 않습니다. 모니터링만으로도 백그라운드 삭제 없이 SHM을 만들 수 있습니다. 32비트에서 큐 나이는 약 24.9일 미만을 다루며, 부호 있는 시간 차가 음수면 0으로 보고합니다.

<a id="cache_purge_legacy_status"></a>

### `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

설정 마이그레이션을 위해서만 허용되는 폐기된 지시어입니다. 두 값 모두 경고를 출력하며 동작에는 영향을 주지 않습니다. 지시어를 제거하십시오. 잘못된 값은 거부됩니다. 동기 작업에서 일치하는 항목이 없으면 `200 no_match`, 물리적 정리 수락은 `202 accepted`, 게시된 세대 무효화는 `200 invalidated`를 반환합니다. `412`/`404` 전환은 제거되었습니다. [HTTP 응답 규약](../HTTP_RESPONSES.md)을 참조하십시오.

<a id="cache_purge_vary_aware"></a>

### `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

`on`이면 정확한 키 삭제가 성공한 뒤 주 파일을 지우고 디렉터리를 순회해 같은 `KEY:` 문자열을 가진 나머지 파일을 삭제합니다. 논리 키가 같고 경로는 다른 모든 `Vary`, `gzip_vary` 변형을 포함합니다. 키 바로 다음 바이트가 `\n`인지 검사해 접두사만 같은 오탐을 방지합니다.

정확 키 PURGE마다 전체 디렉터리 순회가 추가되므로 기본적으로 비활성화됩니다. 여러 `Vary`/`gzip_vary` 표현이 있을 때 활성화하십시오. 기본 항목이 이미 없어도 순회합니다. 와일드카드와 `purge_all`은 이미 변형을 포함합니다. 정확 Vary 키는 대소문자를 구분하며 511바이트로 제한됩니다. 더 긴 키는 삭제 전에 `400 invalid_selector`를 반환합니다.

세대 무효화는 이 옵션 없이도 논리 키의 모든 표현을 포함합니다.

<a id="cache_purge_namespace"></a>

### `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

Location을 바인딩하기 전에 세대 네임스페이스를 선언합니다. 이름은 ASCII 문자, 숫자, `_`, `-`로 된 1–32자이고 SHM은 최소 네이티브 페이지 8개입니다. 캐시/임시 루트 바깥의 전용 영속 상태 디렉터리를 사용하십시오. 코어는 재시작 epoch를 할당할 때 영속 마커 파일을 배타적으로 생성합니다. 해당 캐시가 존재할 수 있는 동안 이 작은 파일들을 보관하십시오. 캐시는 남기고 상태만 삭제하면 오래된 epoch를 재사용할 수 있습니다. 전원 손실/fsync 내구성은 보장하지 않습니다. 일반 reload는 epoch와 규칙을 보존하지만 크기/경로 변경이나 네임스페이스 제거는 전체 재시작이 필요합니다. 네임스페이스와 작업 큐 SHM 크기는 별개입니다.

<a id="cache_purge_key"></a>

### `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

Location을 기존 네임스페이스에 바인딩하고 `$cache_purge_key`를 제공합니다. 별도 PURGE location까지 포함해 `proxy_cache_key`, `fastcgi_cache_key`, `scgi_cache_key`, `uwsgi_cache_key`의 전체 값을 이 변수로 설정하십시오. 확장된 네이티브 키와 생성된 키가 다르면 PURGE는 엄격한 무효화를 주장하는 대신 `500`으로 실패합니다. 자체 논리 식 안에서 `$cache_purge_key`를 참조하지 마십시오. 논리 키 최대 길이는 8096바이트입니다. 기존 의미가 필요한 location은 바인딩하지 마십시오. 상속된 바인딩을 취소하는 `off`는 없습니다.

<a id="proxy_cache_refresh"></a>

### `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

주 요청 전용 REFRESH 엔드포인트를 명시적으로 생성하며 다른 메서드는 `405`를 반환합니다. `cache_purge_key` 바인딩, 네이티브 키 식 `$cache_purge_key`, refresh 프로필이 필요합니다. 확장된 논리 키는 정확한 키, 끝이 `*`인 접두사, 이 경로의 네임스페이스 전체를 뜻하는 `*`를 선택합니다. 코어 접근/인증 규칙으로 보호하십시오. REFRESH는 proxy 캐시만 지원하고 PURGE는 네 프로토콜 모두 지원합니다.

<a id="cache_purge_refresh_profile"></a>

### `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

알려진 표현 하나를 리터럴 내부 proxy location에 매핑합니다. 대소문자를 구분하는 정확한 접두사를 논리 키에서 제거하면 `/`로 시작하는 원본 URI가 남아야 하며 쿼리와 이스케이프는 보존됩니다. 모듈은 host를 추측하거나 키에서 `://`를 찾지 않습니다. 대상은 `internal`, 캐시 끔, 요청 헤더/본문 전달 끔, HEAD 메서드, 비어 있지 않은 리터럴 Host, 리터럴 표현 헤더를 가져야 합니다. 두 조건부 헤더는 정확히 `$cache_purge_refresh_etag`, `$cache_purge_refresh_last_modified`를 사용하며 `$cache_purge_refresh_uri`가 `proxy_pass`에 URI를 줍니다. 이 세 순수 변수는 검사 요청에서만 의미가 있습니다. 내부 리디렉션을 포함해 preaccess에서 대상을 검사합니다. 관리자가 매핑과 캐시 키가 같은 원본 표현을 가리키도록 보장해야 합니다. Vary는 원래 요청 헤더를 네이티브 메타데이터가 저장하지 않아 건너뜁니다.

검사 location에 `proxy_intercept_errors off`를 설정하십시오. 원본 오류가 무효화를 유발하는 성공으로 변환되지 않도록 상속된 오류 가로채기도 거부합니다.

<a id="cache_purge_refresh_timeout"></a>

### `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

Worker 스캐너 대기를 포함해 REFRESH 페이지의 새 스캔/검사 제출을 제한합니다. 만료되면 새 작업을 멈추고 응답을 미완료로 표시합니다. 이미 실행 중인 HEAD는 명시적으로 설정해야 하는 코어 `proxy_connect_timeout`, `proxy_send_timeout`, `proxy_read_timeout`에 따라 끝납니다. 엄격한 응답 마감 시간이 아니며 시스템 호출을 중단하지 못합니다. 연결 해제/정상 종료 시 새 제출을 멈추되 활성 스레드 저장소는 네이티브 완료까지 유지합니다.

---

<a id="partial-key-purge"></a>

## 부분 키 삭제

**평가된 캐시 키** 뒤에 `*`를 붙이면 대소문자를 구분하지 않는 접두사 일치를 사용합니다. 정확 키와 Vary는 대소문자를 구분합니다. 물리적 정리 접두사는 마지막 `*`를 제외하고 최대 511바이트입니다. 더 길면 한 번 로그를 남기고 파일 순회 없이 `400 invalid_selector`를 반환합니다.

`cache_purge_key`에서는 논리 식으로 일치시키며 마지막 `*`를 포함해 최대 8096바이트를 허용합니다.

`$uri`로 끝나는 키 식에서 다음 요청은 URI 접두사를 선택합니다:

```http
PURGE /images/header*
```

별표는 URI 경로만이 아니라 평가된 전체 키의 마지막 바이트여야 합니다. `proxy_cache_key`의 쿼리나 변수 접미사를 고려하십시오. 키가 `*` 하나면 모든 적격 항목을 순회합니다.

---

<a id="generation-invalidation"></a>

## 세대 무효화

바인딩된 네임스페이스의 PURGE는 새 세대를 게시하고 이전 세대 정리를 중복 제거하여 예약한 뒤 `200`과 `invalidated`를 반환합니다. 수락 경로에서 캐시 파일을 열거나 삭제하지 않습니다. 게시 후 키를 처음 평가하는 새 요청은 이전 세대에 적중할 수 없습니다. 각 요청은 내부 리디렉션을 포함해 네임스페이스/논리 키별 세대를 고정합니다. 이미 시작한 응답은 끝나도 늦은 캐시 쓰기는 이후 새 요청의 HIT가 되지 못합니다. 정확한 일치는 대소문자를 구분하고 접두사는 ASCII 대소문자를 무시합니다. `purge_all`은 의도적으로 공유한 다른 경로까지 네임스페이스 전체를 바꿉니다. 물리 정리는 지정 경로에 예약되며 다른 경로는 코어 만료나 후속 PURGE를 사용합니다.

무효화 수락이 즉시 디스크 공간 회수를 보장하지는 않습니다. 순회 후 늦은 이전 쓰기는 접근 불가 상태로 남다가 이후 정리 또는 코어 inactive 만료로 제거됩니다. 네임스페이스 SHM이 부족하면 규칙을 새 공통 기준으로 통합합니다. 무효화한 키가 되살아나는 대신 추가 키들이 차가운 상태가 됩니다. 후속 무효화마다 분리된 trie 노드를 최대 64개 회수합니다. 같은 인기 키를 반복 삭제해도 규칙 트리가 무한히 커지지 않습니다. 복잡도는 파일 수가 아니라 제한된 키 길이와 작업 인덱스 크기에 달려 있습니다. 과부하에서 실제 경과 시간 O(1)이나 기아 방지를 보장하지 않으며 수락이 대기할 수 있습니다. 무효화 뒤 GET 폭주에는 `proxy_cache_lock`과 동등한 코어 제어를 사용하십시오.

<a id="conditional-refresh"></a>

## 조건부 REFRESH

REFRESH는 TTL 만료 전에 아직 캐시된 객체를 확인하며, 사전 적재하거나 `304`에서 TTL을 늘리지 않습니다. 일반 만료 객체 트래픽을 처리하는 `proxy_cache_revalidate`, `proxy_cache_background_update`를 보완합니다. 원본이 검증자를 무시하고 늘 `200`을 반환하면 무효화와 추가 HEAD가 됩니다. 현재 유효하며 정규 해시 경로에 있는 항목만 후보입니다. 같은 세대의 물리 교체는 보수적으로 무효화될 수 있지만, 세대 비교로 이전 HEAD가 이후 PURGE 세대를 무효화하는 것을 막습니다.

| 원본의 가공하지 않은 결과 | 동작 / 결과 |
|---|---|
| `304` | 유지; `kept` |
| `200`, `404`, `410` | 조건부 세대 무효화와 지연 정리; `invalidated` |
| 다른 상태 | 유지; `upstream_status` |
| 원본 상태 없음 / 전송 오류 | 유지; `transport_error` |
| Vary 메타데이터 | HEAD 없음; `skipped_vary` |
| 잘못된 매핑/프로필 | 유지; `skipped_profile` |
| 그동안 세대 교체 | 새 무효화 없음; `raced_replaced` |
| 내부 무효화/할당 오류 | 유지; `invalidation_error` |
| 페이지 정지 전 제출/완료되지 않은 작업 | `undispatched`; 커서가 이를 넘지 않음 |

Worker는 한 번에 활성 REFRESH 페이지와 검사 요청을 각각 최대 하나 가집니다. 공유 한도는 전체 worker에서 검사 16개입니다. 다른 페이지 요청은 후보 배열 할당 없이 기다립니다. 각 페이지는 공통 커서로 디렉터리를 스캔해 최대 32개 후보를 보관하고 코어 HEAD 하위 요청을 순차 제출합니다. 후보 메모리와 주 요청 풀을 공유하는 하위 요청 수를 모두 제한합니다. 같은 REFRESH HTTP 요청은 중복 제거하지 않고 동시성 한도를 공유합니다. 무효화는 공통 중복 제거 PURGE 큐를 사용합니다.

커서 없이 시작한 후 `complete`가 참이 될 때까지 같은 엔드포인트에 같은 논리 선택자/프로필과 `X-Cache-Purge-Cursor: <next_cursor>`를 보내십시오. 커서는 마지막으로 완료한 32자리 16진수 파일명입니다. 잘못되거나 중복된 커서 헤더는 `400`을 반환합니다. `scan_complete`는 디렉터리 열거 성공을, `complete`는 다음 일치 페이지가 없고 모든 선택된 결정도 끝났음을 뜻합니다. `timed_out`, `scan_error`는 중단/실패를 구분합니다. `undispatched`는 선택됐지만 끝나지 않은 수이며, 불완전 스캔은 전체 캐시 수가 아닌 하한만 나타냅니다. 스캔이 불완전하면 검사를 실행하지 않고 빈 결과와 원래 커서를 반환합니다. 결과에는 파일 해시와 원래 upstream 상태만 나오며 키나 자격 증명은 나오지 않습니다.

각 페이지는 다음 해시를 고르기 위해 재스캔합니다. 제한된 메모리 대신 반복 I/O를 치르므로 전체 연산은 `O(files × pages)`입니다. 동시 삽입/삭제에서 페이지의 일관성은 약합니다. 나중 시점의 완전한 스냅샷이 필요하면 처음부터 다시 시작하십시오. 선택자와 타임아웃을 알맞게 정하십시오. 매우 큰 활성 캐시를 자주 스캔하는 용도에는 적합하지 않습니다.

<a id="generation-and-refresh-example"></a>

### 세대와 REFRESH 예제

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

## 설정 예제

<a id="basic--inline-purge-method"></a>

### 기본 — 인라인 삭제 메서드

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

### 기본 — 별도 삭제 location

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

### 백그라운드 큐 활성화

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

### 전체 항목 삭제

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

### Location별로 재정의하는 JSON 응답

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

### Vary 인식 삭제

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

키를 삭제하면 다른 파일 경로에 저장된 모든 gzip/Vary 변형도 자동으로 삭제됩니다.

---

<a id="performance-tuning"></a>

## 성능 조정

`queue_size`는 미완료 작업을 제한하고, `batch_size`는 worker별 작업을 묶고, `throttle_ms`는 배치 간격을 정합니다. 변경 전후 요청 지연, 디스크 경합, 큐 나이를 측정하십시오. 큐를 키워도 소진 처리량은 늘지 않습니다.

다음은 시작 설정이며 하드웨어 성능 주장이 아닙니다. 열 이름은 공통 `cache_purge_` 접두사를 생략합니다.

| 작업 부하 / 저장소 예산 | `queue_size` | `batch_size` | `throttle_ms` |
|---|---:|---:|---:|
| 제한된 호스트/저장소 | 512 | 5 | 25ms |
| 일반 시작점 | 2048 | 20 | 10ms |
| 여유 용량을 측정한 빠른 저장소 | 8192 | 50 | 5ms |
| 짧은 폭주와 보수적 백그라운드 처리 | 8192 | 5 | 50ms |

일반 설정 예:

```nginx
cache_purge_background_queue on;
cache_purge_queue_size        2048;
cache_purge_batch_size        20;
cache_purge_throttle_ms       10ms;
```

큰 큐는 일시적 폭주를 흡수해도 도착률이 완료율을 계속 넘으면 가득 찹니다. 중복 병합 후 작업/초 비율을 사용해 추가 여유를 `max(0, arrival_rate - completion_rate) * burst_seconds`개로 추정하십시오. 작업 시간 편차와 기존 적체에도 여유를 두십시오. 수락 대기는 열린 요청과 연결을 늘리므로 저장소 포화 시 메모리/FD 예산에 포함해야 합니다.

<a id="memory-sizing"></a>

### 메모리 산정

영역 크기는 네이티브 구조 크기로 계산합니다:

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

| 구성 요소 | 예약 또는 사용 메모리 |
|---|---|
| 작업 예약분 | `Q * S`; slab 반올림과 인라인 키/풀 이름 여유 포함 |
| 한 경로의 지표 | `R * NGX_MAX_PROCESSES` 및 정렬; 192바이트 간격, 1024슬롯이면 192 KiB |
| 큐 제어 | `H`; 프로세스 소유권과 epoch 배열 포함 |
| 경로 예약분 | `M`; 이름, rbtree 루트, 정렬, 지표 배열 포함 |
| 최소 영역 | 네이티브 페이지 8개 |

측정한 LP64 구조(`I=184`, `H=20576`, 경로 레코드 104바이트, 통계 레코드 144바이트), 캐시 라인 64바이트, 페이지 4 KiB, 20바이트 경로 하나에서:

| 용량 | 영역 바이트 | 영역 MiB |
|---:|---:|---:|
| 512 | 1654784 | 1.58 |
| 2048 | 5373952 | 5.12 |
| 8192 | 20242432 | 19.30 |

실제 작업 저장소는 slab 반올림 전 `I + index_bytes + 1 + pool_name_bytes`입니다. 내장 인덱스는 1바이트 작업 태그와 기존 키 또는 세대 순회의 네임스페이스 이름을 담습니다. 기존 `purge_all`은 태그만 저장합니다. 용량은 대기/활성 할당의 합을 제한합니다. 네이티브 크기, 캐시 라인, `NGX_MAX_PROCESSES`, 경로가 결과를 결정하며 Windows와 32비트 구조는 다릅니다. Reload 보호 레코드는 추가 SHM을 사용하고 작업 수 한도 전에 할당이 실패할 수 있습니다.

프로세스 메모리는 별도입니다. 활성 순회마다 고정 키 버퍼, 재사용 경로 버퍼, 열린 디렉터리당 고정 프레임을 씁니다. 프레임은 조상 경로 복사 없이 접두사 길이만 보관하므로 저장소는 파일 수와 무관한 `O(depth + longest_path)`입니다. 디렉터리 스트림은 플랫폼 버퍼도 가집니다. 대기 요청에는 커서 없이 작은 타이머/정리 컨텍스트만 있습니다. 스레드 스택과 코어 풀 비용은 별도입니다.

네임스페이스 SHM 크기도 별도입니다. REFRESH 스캐너 하나는 고정 283864바이트 LP64 후보 버퍼/컨텍스트(32개 항목), 순회 프레임, HTTP 요청 풀의 최대 32개 코어 하위 요청을 사용합니다. 페이지가 끝나면 요청이 해제되고 이어받기는 새 요청을 만듭니다. 추가 대기 요청은 후보 배열을 할당하지 않습니다.

<a id="throughput-and-latency"></a>

### 처리량과 지연

계속 바쁜 worker에 유용한 근사식:

```text
B = tasks completed in a batch
T = throttle delay in milliseconds
D = elapsed batch processing time in milliseconds, including slice waits
rate_per_worker ~= 1000 * B / (D + T) tasks/s
```

`D`를 무시한 스케줄링만의 상한은 `1000 * B / T`로, 기본값에서 worker당 1000작업/초입니다. 파일 삭제율이나 실측값은 아닙니다. 작업 10개가 총 200 ms 걸리고 대기가 10 ms면 약 47.6작업/초입니다. 저장소는 공유되므로 worker 수를 곱한 처리량은 보장되지 않습니다. 유휴 worker는 새 작업 확인까지 최대 `10 * T`와 이벤트 루프 지연을 더한 시간만큼 걸릴 수 있습니다.

휴지기 사이 작업을 줄이려면 배치를 줄이거나 대기를 늘리십시오. 디스크 작업 중 worker 지연을 낮추려면 선택적 풀을 사용하십시오. 어느 설정도 단일 syscall을 제한하거나 스레드 작업을 배치로 나누지는 않습니다.

---

<a id="monitoring-and-debugging"></a>

## 모니터링과 디버깅

[상태 엔드포인트](#cache_purge_queue_status)로 대기/활성 작업, 수락 대기, 중복, 삭제 결과와 HIT/MISS를 관찰하십시오. `202 Accepted`는 수락/병합 확인이지 삭제나 즉각적인 논리 무효화의 증거는 아닙니다. 요청 지연을 큐 나이 및 저장소 경합과 함께 보십시오.

| 수준 | 조건 |
|---|---|
| `warn` | 과도한 키 길이, 제거된 풀, 제한한 설정값 |
| `error` | 접근 거부, 캐시 조회/REFRESH 프로필 실패, 정리 재시도 소진; 코어 풀 또는 Windows 읽기 실패 |
| `crit` | 파일 열기/stat/삭제 또는 디렉터리 연산 실패; POSIX 코어 읽기 실패 |
| `alert` | 파일 닫기 실패 |
| `emerg` | 잘못된 설정, 비호환 SHM, 크기 변경 또는 reload 메타데이터 할당 실패 |
| `debug` | 요청과 순회의 추가 컨텍스트 |

모듈 진단은 NGINX 네이티브 로깅 함수로 기록하며 `ngx_cache_purge: `로 시작합니다. NGINX가 수준, 시간, 프로세스/스레드 ID, errno 텍스트, 요청/설정 컨텍스트를 출력에 추가합니다. 파일 오류는 실제 실패 연산과 따옴표로 감싼 경로를, 네임스페이스/REFRESH 오류는 대상과 거부 이유를 명시합니다. 표준 NGINX 오류 로그 파서로 메시지 접두사를 필터링하십시오.

디버그는 `NGX_LOG_DEBUG_HTTP`를 사용하며 `--with-debug` 빌드와 `error_log /var/log/nginx/error.log debug;` 둘 다 필요합니다.

큐 압력은 재시도별 경고 대신 수락 대기 지표에 반영합니다. 이미 사라진 파일은 예상한 경쟁이므로 모듈 진단을 만들지 않습니다. 설정 파서, 할당기, I/O, 스레드 풀 등 코어 헬퍼 메시지는 코어 형식을 유지합니다. 실패 조사 시 전체 로그를 보십시오.

```bash
tail -f /var/log/nginx/error.log | grep 'ngx_cache_purge:'
```

검증하는 진단은 [로깅 검사](../../t/TESTING.md#logging-contracts)에 있습니다.

---

<a id="troubleshooting"></a>

## 문제 해결

| 증상 | 확인 또는 조정 |
|---|---|
| 큰 순회 중 worker 지연 | 백그라운드 큐와 디스크 I/O용 선택적 풀을 켜십시오. 정확한 키/Vary와 큐를 끈 작업은 동기식입니다. |
| 수락 대기 증가 | 도착률, 큐 나이, 풀 용량, 디스크를 보십시오. 추가 용량은 폭주를 흡수하며 크기 변경은 재시작이 필요합니다. |
| 높은 iowait | `cache_purge_batch_size`를 줄이고 `cache_purge_throttle_ms`를 늘리며 유입 삭제를 제한하십시오. |
| 느린 큐 처리 | 배치를 늘리거나 대기를 줄이기 전에 작업 시간과 저장소 경합을 측정하십시오. |
| 클라이언트가 이전 `412`/`404`를 기대함 | `cache_purge_legacy_status`를 제거하고 `200`과 `Cache-Purge-Code: no_match`를 처리하십시오. |
| Vary 변형 잔존 | `cache_purge_vary_aware`를 켜고 주 삭제 결과와 511바이트 제한을 확인하십시오. |
| 수락한 작업이 미완료로 보임 | 활성 작업, 복구, 파일 오류를 보십시오. `202`는 완료 영수증이 아니며 SHM 손상이 의심되면 재시작해야 합니다. |

---

<a id="testing"></a>

## 테스트

Docker에서 전체 모음을 빌드하고 실행하십시오:

```sh
make test-all
NGINX_VERSION=1.31.6 make test-all
```

`t/`에서 기존 Compose 서비스도 사용할 수 있습니다:

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

Compose 플러그인은 `docker-compose` 대신 `docker compose`를 사용합니다. Makefile은 둘 다 감지합니다. `nginx-test`와 `make test-all`은 CI와 동일한 `prove` 진입점으로 7개 `t/*.t` 모음을 실행합니다. 통합 이미지는 네이티브 헬퍼/구조 검사와 풀 런타임 검사를 포함합니다. 스레드 없는 엄격 빌드는 점진적 런타임을 검사합니다. 테스트 하나라도 실패하면 명령이 실패합니다.

엄격 빌드, sanitizer, Valgrind, 플랫폼 작업과 보고서는 [`t/TESTING.md`](../../t/TESTING.md)를 참고하십시오. 모든 GitHub 작업은 `ci.yml`에 있습니다.

---

<a id="migration"></a>

## 마이그레이션

<a id="queue-and-status-changes"></a>

### 큐와 상태 변경

SHM 구조 변경은 전체 재시작이 필요합니다. 메서드/ACL 구문과 프로토콜 핸들러는 유지됩니다. v4는 기본 JSON, 헤더와 본문의 동일한 결과 코드, 항목 부재 시 `200 no_match`를 사용합니다. `cache_purge_legacy_status`는 경고만 출력합니다. [HTTP 응답 규약](../HTTP_RESPONSES.md)에 맞게 클라이언트를 갱신하십시오. 용량에는 배정된 작업도 포함되며 과부하 요청은 수락을 기다립니다. 활성 중복 작업은 재실행을 예약할 수 있습니다.

상태는 기존 필드명을 유지하며 필드를 추가합니다. `requests.overflow`는 이제 수락 대기를 셉니다. 지표는 근사치이며 64비트 누적 카운터를 사용합니다. 스레드 빌드는 참조한 `default`를 자동 등록합니다. 삭제 I/O를 분리하려면 이름 있는 풀을 사용하십시오. 상태만 설정해도 SHM을 할당합니다.

v4.0 세대 무효화와 REFRESH는 명시적 opt-in입니다. 참여하는 모든 location에 `cache_purge_key`를 바인딩하고 `$cache_purge_key`를 네이티브 키 전체로 사용하십시오. 물리 파일명이 달라져 기존 파일은 콜드 상태가 되고 코어 만료나 명시적인 기존 방식 정리를 기다립니다. 상태 마커는 캐시와 함께 보존하십시오. 기존 PURGE location에 REFRESH가 자동 추가되지 않습니다. 실험적 PR60 사용자는 새 엔드포인트/프로필과 페이지화를 설정해야 합니다. PR60의 무제한 응답, 추측한 URI/Host, 암묵적 메서드 확장은 호환성 보장 대상이 아닙니다.

<a id="from-the-original-frickle-module"></a>

### 원래 FRiCKLE 모듈에서 전환

메서드/접근 목록 및 영역/키 형식은 계속 사용할 수 있습니다. 없는 항목은 `200 no_match`를 반환합니다. `cache_purge_legacy_status`를 제거하고 클라이언트를 갱신하십시오. 백그라운드 처리를 활성화하려면 다음을 추가하십시오.

```nginx
http {
    cache_purge_background_queue on;
}
```

<a id="from-nginx-selective-cache-purge-module"></a>

### nginx-selective-cache-purge-module에서 전환

모듈을 교체하십시오. 백그라운드 큐는 내장되어 있습니다. 기존 매개변수를 `cache_purge_queue_size`, `cache_purge_batch_size`, `cache_purge_throttle_ms`에 대응시키십시오.

---

<a id="temporary-directories"></a>

## 임시 디렉터리

순회는 경로 구성 요소 전체를 비교해 캐시 루트 아래의 설정된 임시 디렉터리를 건너뜁니다. `proxy_temp` 같은 이름은 임시 경로로 설정되지 않았다면 예약되지 않습니다. 이름이 정확히 32개의 16진수 문자로 된 일반 파일만 삭제 후보입니다. 숫자형/접미사형 캐시 쓰기 임시 파일은 `use_temp_path=off`에서도 건너뜁니다.

모든 순회는 네이티브 헤더 버전, 오프셋, 전체 `KEY: ` 마커, CRC32, 키/변형 파일명 해시를 고정 8192바이트 키 제한으로 검증합니다. 형식 오류, 잘림, 다른 버전, 불일치 파일은 purge-all에서도 건너뜁니다. 코어 최대값과 같은 ETag/Vary 길이는 허용합니다. 헤더 검증은 형식 오류를 찾으며 악의적 파일을 인증하지는 않습니다.

설정된 루트는 심볼릭 링크나 Windows junction일 수 있습니다. 하위 symlink/reparse point는 건너뜁니다. 완전한 디스크립터 상대 기능 검사에 통과한 POSIX는 코어 `ngx_openat_file`/`ngx_file_at_info`와 no-follow를 사용하고 열린 부모 디렉터리를 기준으로 삭제합니다. 하위 링크 교체에 의한 우회는 막지만 이미 열린 디렉터리 이동에 대한 샌드박스는 아닙니다. Windows와 구형 Unix는 코어 전체 경로 연산과 링크 확인을 쓰므로 경로 교체 경쟁이 남습니다. 기존 정확 키 조회/삭제도 전체 경로를 사용합니다. 루트를 비공개로 유지하고 신뢰하지 않는 작성자를 허용하지 마십시오.

설정 경로의 정규 표기를 일관되게 사용하십시오. 비교는 symlink, 마운트, Unicode 별칭을 해석하지 않습니다. 큐 작업은 보호 집합을 유지하고 새 중복은 대기 작업/다음 재실행을 갱신합니다. 진행 중 순회는 기존 집합을 유지하며 살아 있는 모든 worker 세대의 합집합은 아닙니다. 관련 작업이 대기/활성인 동안 경로를 다른 용도로 바꾸지 마십시오.

---

<a id="security"></a>

## 보안

- 삭제 엔드포인트를 `allow` / `deny` 또는 앞단 인증으로 제한하고 공개하지 마십시오.
- 요청 폭주에서 공유 메모리를 제한하도록 트래픽에 맞는 `cache_purge_queue_size`를 설정하십시오.
- 일반 트래픽과 별개로 PURGE 속도를 제한하려면 `limit_req` 조합을 고려하십시오.

---

<a id="license"></a>

## 라이선스

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

## 관련 자료

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [nginx 문서](https://nginx.org/en/docs/)
