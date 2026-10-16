# ngx_cache_purge

[README](README.ko.md) · [지시어](DIRECTIVES.ko.md)

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
