<a id="ngx_cache_purge"></a>

# ngx_cache_purge

`FastCGI`, `proxy`, `SCGI`, `uWSGI` 캐시 항목을 삭제하는 NGINX 모듈입니다. v4의 키 세대 기능은 파일을 삭제하기 전에 새 요청이 이전 항목을 사용하지 못하게 합니다. REFRESH는 프록시 캐시의 내용이 바뀌었는지 원본 서버에 확인합니다. 두 기능 모두 명시적인 설정이 필요합니다. PURGE 구문은 계속 사용할 수 있지만 HTTP 응답은 변경됩니다. [마이그레이션](README.ko.md#migration)을 참고하세요.

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

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
- [원본 서버에서 캐시 내용 확인하기 (REFRESH)](#conditional-refresh)

<a id="choosing-a-mode"></a>

## 목적에 맞는 동작 선택

| 목적 | 설정 | 응답 의미 |
|---|---|---|
| 오래된 항목을 삭제하고 삭제 시도가 끝날 때까지 대기 | 백그라운드 큐를 끄고 PURGE | `200 purged` 또는 `200 no_match`; 오류는 4xx/5xx |
| 접두사별 또는 전체 캐시 정리 예약 | PURGE와 `cache_purge_background_queue on` | `202 accepted`; 대기 중에는 이전 항목이 제공될 수 있음 |
| 늦게 기록된 파일을 포함해 이전 내용을 새 요청에서 사용하지 못하게 차단 | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`; 새 키 세대를 사용하고 파일은 나중에 삭제 |
| 원본 내용의 변경 여부 확인 | 키 세대 + `proxy_cache_refresh` + 내부 프로필 | 항목별 결과와 이어서 처리할 정보를 담은 `200`; 캐시를 미리 채우지 않음 |

이 기능들은 함께 사용할 수 있습니다. 세대 변경도 파일 정리를 예약합니다. 큐만으로는 늦은 쓰기로 이전 내용이 다시 사용되는 문제를 막을 수 없습니다. [v4 예제](README.ko.md#generation-and-refresh-example), [PURGE 예제](#sample-configurations), [HTTP 응답](../HTTP_RESPONSES.md) (영문)을 참고하세요.

<a id="compatibility"></a>

## 호환성

운영 환경에서는 유지보수 중인 NGINX 버전을 사용하세요. POSIX/UNIX가 주 대상이며 Windows 지원은 실험적입니다. 스레드는 선택 사항입니다. [NGINX / CI](../../t/TESTING.md#compatibility-matrix).

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
./configure --with-compat --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

서버에 설치된 NGINX와 같은 버전의 소스 코드를 사용하세요. `--with-compat`는 임의의 NGINX 버전이나 C 라이브러리 간 호환성을 보장하지 않습니다. 자세한 내용은 [동적 모듈 및 릴리스 아카이브](../BINARY_MODULES.md)(영문)를 참고하세요.

<a id="generation-and-refresh-example"></a>

## 예제: 상품 카탈로그

이 전체 설정은 로컬 8080 포트에서 요청을 받아 8081 포트의 애플리케이션으로 전달합니다. 애플리케이션은 Host `shop.example.com`에 대해 `/products/42`를 제공해야 합니다. 애플리케이션 주소, 도메인, 쓰기 가능한 캐시 및 상태 디렉터리를 바꾸세요.

`shop.example.com`은 HTTP Host 도메인이며 디렉터리가 아닙니다. `/products/42`는 상품 URI입니다. `catalog:`는 고정 캐시 키 접두사이며 호스트 이름이 아닙니다. 쿼리 문자열을 포함한 전체 URI와 합쳐 `catalog:/products/42` 같은 논리 키를 만듭니다. REFRESH 프로필은 `catalog:`를 제거하고 같은 애플리케이션의 `/products/42`를 확인합니다.

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx/catalog levels=1:2
                     keys_zone=catalog:10m;
    cache_purge_namespace catalog 1m /var/lib/nginx/cache-purge;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        location /products/ {
            cache_purge_key catalog "catalog:$request_uri";
            proxy_cache catalog;
            proxy_cache_key $cache_purge_key;
            proxy_cache_lock on;
            proxy_cache_valid 200 10m;
            add_header X-Cache-Status $upstream_cache_status always;
            proxy_set_header Host shop.example.com;
            proxy_pass http://catalog_backend;
            proxy_cache_purge PURGE from 127.0.0.1;
        }

        location = /admin/refresh {
            allow 127.0.0.1;
            deny all;
            cache_purge_key catalog $http_x_cache_key;
            proxy_cache_refresh catalog $cache_purge_key;
            cache_purge_refresh_profile /internal/cache-probe catalog:;
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
            proxy_set_header Host shop.example.com;
            proxy_set_header Authorization "";
            proxy_set_header Cookie "";
            proxy_set_header If-None-Match $cache_purge_refresh_etag;
            proxy_set_header If-Modified-Since
                             $cache_purge_refresh_last_modified;
            proxy_pass http://catalog_backend$cache_purge_refresh_uri;
        }
    }
}
```

존재하는 캐시 가능한 상품을 GET하고 `X-Cache-Status: HIT`가 나올 때까지 반복한 다음 REFRESH를 보내세요:

```sh
curl -i 'http://127.0.0.1:8080/products/42'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/42' \
  'http://127.0.0.1:8080/admin/refresh'
```

상품이 변경되었다면 PURGE를 보내세요. 이 설정에서 `200 invalidated`는 새 요청이 이전 세대를 사용할 수 없다는 뜻이며, 다음 GET은 애플리케이션에 접근합니다.

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/products/42'
```

전체 상품 검사는 `catalog:/products/*`를 사용하세요. `complete: false`이면 선택 조건을 유지하고 반환값으로 `<next_cursor>`를 바꿔 계속합니다. HTTP 오류는 먼저 해결하세요.

```sh
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

<a id="partial-key-purge"></a>

## 부분 키 삭제

**평가된 캐시 키** 뒤에 `*`를 붙이면 대소문자를 구분하지 않는 접두사 일치를 사용합니다. 정확 키와 Vary는 대소문자를 구분합니다. 물리적 정리 접두사는 마지막 `*`를 제외하고 최대 511바이트입니다. 더 길면 한 번 로그를 남기고 파일 순회 없이 `400 invalid_selector`를 반환합니다.

`cache_purge_key`에서는 논리 식으로 일치시키며 마지막 `*`를 포함해 최대 8096바이트를 허용합니다.

`$uri`로 끝나는 키 식에서 다음 요청은 URI 접두사를 선택합니다:

```http
PURGE /images/header*
```

별표는 URI 경로만이 아니라 평가된 전체 키의 마지막 바이트여야 합니다. `proxy_cache_key`의 쿼리나 변수 접미사를 고려하십시오. 키가 `*` 하나면 모든 적격 항목을 순회합니다.

<a id="generation-invalidation"></a>

## 세대 무효화

키 세대를 사용하면 PURGE 완료 후 새 요청이 이전 콘텐츠를 HIT하지 않습니다. 늦게 기록된 이전 응답도 새 요청에 사용되지 않습니다. 파일 삭제는 별도로 진행됩니다. 네임스페이스는 무효화 규칙을 공유하는 단위이며, 재시작할 때 상태 디렉터리를 보존해야 합니다.

<a id="conditional-refresh"></a>

## 원본 서버에서 캐시 내용 확인하기 (REFRESH)

REFRESH는 저장된 ETag와 Last-Modified로 동일한 원본 표현에 HEAD를 보냅니다. 상품 페이지가 변경된 사실을 안다면 PURGE를 사용하세요. REFRESH는 새 본문을 받거나 빈 캐시를 채우거나 TTL을 연장하지 않습니다. [HTTP / REFRESH](../REFRESH.md).

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

<a id="sample-configurations"></a>

## 설정 예제

처음 다섯 코드 블록은 각각 독립적인 전체 `nginx.conf` 설정 예입니다. 마지막 블록은 기존 `http` 영역에 Vary 처리를 추가합니다. NGINX는 로컬 포트 8080에서 요청을 받습니다. catalog_backend는 127.0.0.1:8081에서 실행되는 애플리케이션의 업스트림 그룹 이름이며, shop.example.com은 애플리케이션에 전달하는 HTTP Host입니다. 배포 환경에 맞게 주소와 NGINX가 쓸 수 있는 캐시 디렉터리를 변경하세요.

<a id="basic--inline-purge-method"></a>

### 기본 — 인라인 삭제 메서드

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        location / {
            proxy_pass        http://catalog_backend;
            proxy_cache       main;
            proxy_cache_valid 200 10m;
            proxy_set_header Host shop.example.com;
            add_header X-Cache-Status $upstream_cache_status always;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }
    }
}
```

```bash
curl -i -X PURGE -H 'Host: shop.example.com' \
  'http://127.0.0.1:8080/images/logo.png'
# 200 — entry purged
# 200 + Cache-Purge-Code: no_match
# 403 — client IP not in the allowed list
```

<a id="basic--separate-purge-location"></a>

### 기본 — 별도 삭제 location

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        location / {
            proxy_pass      http://catalog_backend;
            proxy_cache     main;
            proxy_cache_valid 200 10m;
            proxy_set_header Host shop.example.com;
            add_header X-Cache-Status $upstream_cache_status always;
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
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;
    cache_purge_queue_size        2048;
    cache_purge_batch_size        20;
    cache_purge_throttle_ms       10ms;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        location / {
            proxy_pass        http://catalog_backend;
            proxy_cache       main;
            proxy_cache_valid 200 10m;
            proxy_set_header Host shop.example.com;
            add_header X-Cache-Status $upstream_cache_status always;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }
    }
}
```

<a id="purge-all-entries"></a>

### 전체 항목 삭제

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        location /content {
            proxy_pass        http://catalog_backend;
            proxy_cache       main;
            proxy_cache_valid 200 10m;
            proxy_set_header Host shop.example.com;
            add_header X-Cache-Status $upstream_cache_status always;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE purge_all from 127.0.0.1;
        }
    }
}
```

```bash
curl -i -X PURGE -H 'Host: shop.example.com' \
  'http://127.0.0.1:8080/content/anything'
# 200 — walk completed; inspect error_log for file errors
```

<a id="json-responses-with-per-location-override"></a>

### Location별로 재정의하는 JSON 응답

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        # Default for all locations in this server.
        cache_purge_response_type json;
        location / {
            proxy_pass        http://catalog_backend;
            proxy_cache       main;
            proxy_cache_valid 200 10m;
            proxy_set_header Host shop.example.com;
            add_header X-Cache-Status $upstream_cache_status always;
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

<a id="migration"></a>

## 마이그레이션

SHM 구조 변경은 전체 재시작이 필요합니다. 메서드/ACL 구문과 프로토콜 핸들러는 유지됩니다. v4는 기본 JSON, 헤더와 본문의 동일한 결과 코드, 항목 부재 시 `200 no_match`를 사용합니다. `cache_purge_legacy_status`는 경고만 출력합니다. [HTTP 응답 규약](../HTTP_RESPONSES.md)에 맞게 클라이언트를 갱신하십시오. 용량에는 배정된 작업도 포함되며 과부하 요청은 수락을 기다립니다. 활성 중복 작업은 재실행을 예약할 수 있습니다.

<a id="security"></a>

## 보안

- 삭제 엔드포인트를 `allow` / `deny` 또는 앞단 인증으로 제한하고 공개하지 마십시오.
- 요청 폭주에서 공유 메모리를 제한하도록 트래픽에 맞는 `cache_purge_queue_size`를 설정하십시오.
- 일반 트래픽과 별개로 PURGE 속도를 제한하려면 `limit_req` 조합을 고려하십시오.

<a id="documentation"></a>

## 문서

- [지시어 참고 문서](DIRECTIVES.ko.md)
- [추가 설정 예제](README.ko.md#sample-configurations)
- [운영과 문제 해결](OPERATIONS.ko.md)
- [HTTP와 REFRESH 제한 (영문)](../REFRESH.md)
- [v4 이전 안내](MIGRATION.ko.md)
- [구현 구조 (영문)](../ARCHITECTURE.md)
- [테스트](OPERATIONS.ko.md#testing)
- [개발과 테스트 (영문)](../../t/TESTING.md)

<a id="see-also"></a>

## 관련 자료

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [nginx 문서](https://nginx.org/en/docs/)

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
