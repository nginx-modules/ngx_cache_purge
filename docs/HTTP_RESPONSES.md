# HTTP responses in v4

HTTP status describes the request outcome. `Cache-Purge-Code` identifies the module result; JSON bodies contain the same value in `code`. Module responses include `Cache-Control: no-store`. Clients should branch on status and `code`, not parse a human-readable message.

## Completion and acceptance

`200 OK` means the requested synchronous operation or logical invalidation completed. `202 Accepted` means physical cleanup was admitted or coalesced; it can still fail later. Neither response promises that concurrent writers will leave the cache empty forever. These meanings follow [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html#section-15.3).

Generation invalidation returns `200 invalidated`: publication has made the old generation unreachable, while physical cleanup remains pending. Ordinary background wildcard/full purge returns `202 accepted`. A full queue leaves the request waiting for admission; it does not immediately return `202` or turn overload into a synchronous walk.

The status endpoint reports aggregate queue and cleanup counters. It is not a per-request job monitor: v4 does not issue task IDs or a task `Location`. Inspect cleanup errors and exhausted retries; an accepted request is not proof that every file was deleted.

## Result catalog

| HTTP | `Cache-Purge-Code` / JSON `code` | Meaning |
|---|---|---|
| 200 | `purged` | Matching files were deleted synchronously |
| 200 | `no_match` | The synchronous operation found no matching entry |
| 202 | `accepted` | Physical cleanup was admitted or coalesced |
| 200 | `invalidated` | Logical invalidation was published and cleanup admitted |
| 200 | `status` | Aggregate status snapshot |
| 200 | `refresh_complete` | REFRESH scan/page processing finished; inspect entry results |
| 200 | `refresh_incomplete` | REFRESH needs continuation or reached its time limit |
| 400 | `invalid_selector` | Selector exceeds the supported key length |
| 400 | `invalid_cursor` | REFRESH cursor is not a single 32-digit hexadecimal value |
| 403 | `access_denied` | Module access list rejected the client |
| 405 | `method_not_allowed` | Wrong method for a dedicated endpoint; `Allow` lists accepted methods |
| 500 | `cache_unavailable` | Cache configuration is unusable for this operation |
| 500 | `key_mismatch` | Native cache key violates the configured namespace contract |
| 500 | `delete_failed` | A synchronous deletion failed; partial deletion is possible |
| 500 | `scan_failed` | A synchronous PURGE or REFRESH scan failed; partial effects are possible |
| 500 | `internal_error` | Other module failure or an unknown internal result |

Missing entries are successful idempotent outcomes (`200 no_match`), not failed HTTP preconditions. Generation invalidation of an absent key still returns `invalidated`. A completed REFRESH can contain origin/transport errors for individual entries; inspect its existing `results` and continuation fields. See [REFRESH semantics and edge cases](REFRESH.md) for origin-status policy, metadata limitations, cancellation and retry behavior. A directory scan failure returns `500 scan_failed`; it does not return a normal continuation document.

Physical wildcard prefixes are limited to 511 bytes excluding the trailing `*`. Exact keys with Vary-aware traversal have the same 511-byte limit. Namespace selectors allow 8096 bytes including a trailing `*`. Oversized selectors are rejected before deletion; physical `purge_all` does not need a prefix. Native cache-file validation has its separate 8192-byte key limit.

The module does not translate errors generated before its handler runs by NGINX request parsing, authentication, access phases or other modules. Core body-discard errors also retain core handling. Memory exhaustion and a closed connection can prevent even an error response from being delivered. Custom filters or internal redirects can replace responses. This catalog covers module-owned terminal outcomes, not every possible NGINX/network failure.

## Bodies and formats

`cache_purge_response_type json` is the default. Successful PURGE JSON uses lowercase field names:

```json
{"key":"/items/42","key_encoding":"utf-8","code":"no_match","complete":true,"cleanup_pending":false,"deleted":0,"delete_errors":0,"scan_errors":0}
```

`complete` describes this operation, not physical cleanup: it is true for `invalidated`, whose `cleanup_pending` is also true. `accepted` has `complete: false` and `cleanup_pending: true`. The three counters are included only when this request performed a synchronous traversal/deletion; they are not fabricated for queued work.

`html`, `xml` and `text` remain explicit success formats. Each includes the key, result code and key encoding; XML uses `application/xml`. Keys are escaped for the selected representation. Binary keys, invalid UTF-8 or characters outside XML 1.0 are encoded losslessly as Base64 in every format and labelled `base64`; valid text is labelled `utf-8`.

Module errors use [RFC 9457](https://www.rfc-editor.org/rfc/rfc9457.html) `application/problem+json`, regardless of the selected success format:

```http
HTTP/1.1 405 Method Not Allowed
Allow: PURGE
Cache-Purge-Code: method_not_allowed
Cache-Control: no-store
Content-Type: application/problem+json

{"type":"about:blank","title":"Method Not Allowed","status":405,"detail":"Use a method listed in the Allow header.","code":"method_not_allowed"}
```

`type: about:blank` retains the standard HTTP problem meaning; `code` is the module-specific extension. `status` matches the HTTP status. Details do not expose filesystem paths, credentials or raw internal diagnostics. Known partial-operation counters are included on synchronous scan/deletion errors.

`cache_purge_response_type none` suppresses PURGE bodies: successful `200` becomes `204 No Content`, accepted work stays `202`, and errors retain their 4xx/5xx status. `Cache-Purge-Code` remains available. HEAD omits a body under normal HTTP rules. Status and REFRESH endpoints retain their structured JSON regardless of the PURGE success format.

## Migration from v2/v3

Remove `cache_purge_legacy_status`. Both `on` and `off` are accepted solely to make migration easier, emit a configuration warning and have no effect. Invalid values are rejected. v4 no longer uses this directive to select `412` or `404`. It does not emit HTTP `Deprecation` or `Sunset` headers for an obsolete configuration directive.

Update clients for default JSON, lowercase JSON fields, `accepted` instead of the old `queued` label, and `200 no_match`. Use an explicit success format when needed; format selection does not restore the old status policy.
