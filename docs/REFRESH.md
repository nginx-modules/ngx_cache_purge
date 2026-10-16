# REFRESH: HTTP contract and operational limits

REFRESH checks existing proxy-cache entries and may invalidate them. It does not fetch replacement bodies or refresh their expiry time. Use PURGE when the content is already known to be obsolete. Use ordinary NGINX cache revalidation when an expired GET response needs updating.

Start with the [configuration example](../README.md#generation-and-refresh-example). Protect the administrative endpoint and keep its origin mapping explicit.

## Relationship to HTTP standards

`REFRESH` is a module-specific method, absent from the [IANA method registry](https://www.iana.org/assignments/http-methods/). It changes cache state and must be treated as an unsafe administrative operation. Allow it explicitly in any intermediary or method allowlist. The method does not define a request-body format; send the configured selector in the URI or header used by `cache_purge_key`.

The origin probe uses HEAD and stored `If-None-Match` / `If-Modified-Since` validators. These are defined by [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html#section-13). The origin evaluates the conditions; ETag takes precedence over the date. Weak ETags are valid for this check. Conditional HEAD is allowed, although [RFC 9110 §13.2.1](https://www.rfc-editor.org/rfc/rfc9110.html#section-13.2.1) notes that it offers little response-size benefit over ordinary HEAD. Here the conditional status supplies the module's keep/invalidate decision.

This is **not a complete implementation of cache revalidation** from [RFC 9111 §4.3.4–4.3.5](https://www.rfc-editor.org/rfc/rfc9111.html#section-4.3.4). That mechanism includes selecting stored responses and updating metadata. The v4 probe keeps a file unchanged on `304`; it does not merge new `Cache-Control`, `Vary`, validators or other response headers. It also does not compare a `200` HEAD's metadata to retain an otherwise unchanged object. Do not rely on REFRESH to apply a changed caching policy or extend freshness. If those are required, use normal core revalidation or explicitly PURGE and refill the entry.

The administrative response uses `200` for a processed page, including a page with per-entry errors or a continuation. It uses `400` for a bad selector/cursor and `500 scan_failed` for a failed directory scan. Errors use the problem format from [RFC 9457](https://www.rfc-editor.org/rfc/rfc9457.html), with a matching HTTP `status`, `type: about:blank`, and a module `code` extension. See the [full response contract](HTTP_RESPONSES.md).

## Cases a client must handle

| Situation | Current behavior | Operational consequence |
|---|---|---|
| Origin returns `304` | `kept`; file and TTL unchanged | Content stays cached under its existing policy |
| Origin returns `200` | `invalidated` | Also happens if validators are absent or ignored; the next GET refills |
| Origin returns `404` or `410` | `invalidated` | Verify that the profile reaches the same representation; an authentication-masking 404 can cause an unnecessary purge |
| Origin returns a redirect, `401`, `403`, `412`, `429` or `5xx` | `upstream_status`; entry retained | Redirects are not followed; inspect the raw status and fix the cause |
| Origin does not return a usable status | `transport_error`; entry retained | Connection/TLS/timeouts are not evidence that content changed |
| Entry has Vary metadata | `skipped_vary`; no probe | The stored file cannot reconstruct the original representation headers |
| No current matching file | Empty `results` | A cold or still-filling cache is not prewarmed; an empty result does not validate the profile |
| Profile is unsafe, or URI mapping fails | `skipped_profile`; no invalidation | Configuration failures log the rejected setting; mapping failures may only appear in the result |
| PURGE races with a HEAD | `raced_replaced` for the old generation | The stale decision does not invalidate the later PURGE generation |
| File changes within the same generation | Conservative invalidation is possible | Generation protection is not a byte-for-byte compare-and-swap on a file |
| Page deadline expires | Stop new work; `refresh_incomplete` | An active HEAD still obeys core proxy timeouts; this is not a hard response deadline |
| Client disconnects | Stop dispatching; retire active resources through NGINX callbacks | Previous invalidations remain; cancellation is not a rollback |
| Storage scan fails | `500 scan_failed` problem response | Inspect the error log; do not expect continuation JSON for this failure |

`complete: true` means all selected work and pages are finished, not that every entry was kept or successfully invalidated. Read each result. For `200` responses with `complete: false`, repeat the same selector and profile with `next_cursor` as `X-Cache-Purge-Cursor`. A scan that times out before finishing returns its input cursor unchanged and sends no probes. Detect a non-advancing cursor and resolve the time/storage limit instead of retrying forever.

A cursor is not a durable job ID, a snapshot token, or proof of authorization. Each page rescans the directory, with at most 32 candidates; concurrent file changes can affect enumeration. There is one page/probe per worker and a shared 16-probe limit. Identical REFRESH calls are not deduplicated. Retrying after a lost response can repeat origin checks and invalidations; no exactly-once execution or stable result set is promised.

## Configuration assumptions

The profile must target the same host, URI, query and representation as the cached GET. The module cannot prove that an administrator's static mapping is correct. Use it for a known representation, with explicit literal headers. Do not depend on the administrative client's cookies, credentials, language or Range headers. The guard rejects dynamic representation headers, forwarding of request headers/body, caching, error interception, a non-HEAD method, a public location and internal redirects away from the configured profile.

For an HTTPS origin, configure NGINX's upstream certificate verification, trust store and SNI for that origin. REFRESH does not add an independent TLS policy. The internal profile must remain inaccessible to external requests.

The [coverage map](../t/COVERAGE.md) distinguishes runtime checks, helper-level checks and remaining stress/platform gaps. These limits are part of the public contract; a successful CI run does not remove them.
