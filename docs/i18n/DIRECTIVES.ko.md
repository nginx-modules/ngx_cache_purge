[ngx_cache_purge](README.ko.md)

<a id="directives"></a>

# 지시어

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

이 플래그와 `cache_purge_vary_aware`는 마지막으로 선언한 값을 사용합니다. 잘못된 값은 거부됩니다. `cache_purge_legacy_status`는 폐기되어 무시됩니다. 스레드와 수명 주기는 [실행 모델](../ARCHITECTURE.md#execution-model)을 참조하십시오.

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

영역 크기는 작업, 경로 메타데이터, 정렬된 worker 지표를 포함합니다. 빌드와 경로 수에 따라 다릅니다. [메모리 산정](../OPERATIONS.md#memory-sizing)을 참고하십시오.

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
