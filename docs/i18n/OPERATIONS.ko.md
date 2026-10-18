# ngx_cache_purge

[README](README.ko.md) · [지시어](DIRECTIVES.ko.md)

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

<a id="reading-cache-files"></a>

### 세대 모드에서 캐시 파일 읽기

캐시 브라우저, 사전 로더, 사용량 보고서는 파일만으로 세대 유효성을 판단할 수 없습니다. 무효화 후에도 대체 파일 없이 오래된 파일이 남을 수 있습니다. 세대 파일의 `KEY:` 줄은 `p4:<namespace>:<epoch>:<generation>:<logical key>`입니다. [상태 엔드포인트](DIRECTIVES.ko.md#cache_purge_queue_status)가 네임스페이스 상태를 제공합니다. 정확 일치 또는 접두사 규칙이 높이지 않았다면 키의 현재 세대는 `baseline`이며, 해당 규칙 자체는 응답에 포함되지 않습니다.

| 파일 네임스페이스의 상태 | 판정 |
|---|---|
| 목록에 네임스페이스가 없음 | unknown |
| 파일의 epoch가 `epoch`와 다름 | stale |
| Epoch가 같고 파일 세대가 `baseline`보다 작음 | stale |
| Epoch가 같고 세대가 `baseline` 이상이며 `rules`가 `false` | live |
| Epoch가 같고 세대가 `baseline` 이상이며 `rules`가 `true` | unknown |

여기서 **live**는 네임스페이스 세대 기준으로 오래되지 않았다는 뜻일 뿐입니다. 콘텐츠의 최신성, TTL 유효성, NGINX 캐시 HIT를 보장하지 않습니다. `namespaces_complete=false`여도 반환된 항목은 사용할 수 있고, 생략된 네임스페이스만 알 수 없음으로 처리합니다.

네임스페이스, epoch, 논리 키가 같고 세대가 더 높은 파일이 있으면 낮은 세대의 파일은 오래된 것으로 판단할 수 있습니다. 같은 epoch 안에서는 현재 세대가 감소하지 않기 때문입니다. 그 외에는 unknown 판정을 유지하세요.

스캔 전후에 상태를 읽고 네임스페이스 이름으로 항목을 대응시킵니다. 양쪽 응답에 네임스페이스가 있고 `epoch`, `baseline`, `serial`이 모두 같을 때만 상태에 근거한 해당 판정을 사용하세요. 상태가 없거나 바뀌면 그 네임스페이스의 판정을 폐기합니다. Epoch는 같은지 여부만 비교하세요. 이 관측은 이후의 무효화를 막지 않습니다.

정리 큐가 비었다고 유효성이 입증되지는 않습니다. 무효화 전에 시작한 요청이 정리가 끝난 뒤 이전 세대 파일을 기록할 수 있습니다. 파일은 다음 정리나 네이티브 캐시 퇴거 때까지 남습니다. 또한 정리는 PURGE location의 캐시 경로만 대상으로 하지만, 네임스페이스는 여러 경로에서 공유할 수 있습니다.

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

[상태 엔드포인트](DIRECTIVES.ko.md#cache_purge_queue_status)로 대기/활성 작업, 수락 대기, 중복, 삭제 결과와 HIT/MISS를 관찰하십시오. `202 Accepted`는 수락/병합 확인이지 삭제나 즉각적인 논리 무효화의 증거는 아닙니다. 요청 지연을 큐 나이 및 저장소 경합과 함께 보십시오.

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

<a id="temporary-directories"></a>

## 임시 디렉터리

순회는 경로 구성 요소 전체를 비교해 캐시 루트 아래의 설정된 임시 디렉터리를 건너뜁니다. `proxy_temp` 같은 이름은 임시 경로로 설정되지 않았다면 예약되지 않습니다. 이름이 정확히 32개의 16진수 문자로 된 일반 파일만 삭제 후보입니다. 숫자형/접미사형 캐시 쓰기 임시 파일은 `use_temp_path=off`에서도 건너뜁니다.

모든 순회는 네이티브 헤더 버전, 오프셋, 전체 `KEY: ` 마커, CRC32, 키/변형 파일명 해시를 고정 8192바이트 키 제한으로 검증합니다. 형식 오류, 잘림, 다른 버전, 불일치 파일은 purge-all에서도 건너뜁니다. 코어 최대값과 같은 ETag/Vary 길이는 허용합니다. 헤더 검증은 형식 오류를 찾으며 악의적 파일을 인증하지는 않습니다.

설정된 루트는 심볼릭 링크나 Windows junction일 수 있습니다. 하위 symlink/reparse point는 건너뜁니다. 완전한 디스크립터 상대 기능 검사에 통과한 POSIX는 코어 `ngx_openat_file`/`ngx_file_at_info`와 no-follow를 사용하고 열린 부모 디렉터리를 기준으로 삭제합니다. 하위 링크 교체에 의한 우회는 막지만 이미 열린 디렉터리 이동에 대한 샌드박스는 아닙니다. Windows와 구형 Unix는 코어 전체 경로 연산과 링크 확인을 쓰므로 경로 교체 경쟁이 남습니다. 기존 정확 키 조회/삭제도 전체 경로를 사용합니다. 루트를 비공개로 유지하고 신뢰하지 않는 작성자를 허용하지 마십시오.

설정 경로의 정규 표기를 일관되게 사용하십시오. 비교는 symlink, 마운트, Unicode 별칭을 해석하지 않습니다. 큐 작업은 보호 집합을 유지하고 새 중복은 대기 작업/다음 재실행을 갱신합니다. 진행 중 순회는 기존 집합을 유지하며 살아 있는 모든 worker 세대의 합집합은 아닙니다. 관련 작업이 대기/활성인 동안 경로를 다른 용도로 바꾸지 마십시오.

---

<a id="testing"></a>

## 테스트

[테스트 범위](../TEST_COVERAGE.md)(영문)에 확인된 동작과 남은 공백을 정리했습니다.

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

엄격 빌드, sanitizer, Valgrind, 플랫폼 작업과 보고서는 [`t/TESTING.md`](../../t/TESTING.md)를 참고하십시오. 모든 GitHub 작업은 `build-test-release.yml`에 있습니다.
