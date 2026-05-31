# Agent instructions

## Coding style

- Follow the Nginx core development guide https://nginx.org/en/docs/dev/development_guide.html

## Verifying changes

- Follow the Development section in [README.md](README.md): use the included development container for development, testing, and manual validation work.
- When a task requires `make`, run it inside the `dev` container instead.Prefer one of these patterns:
  - `docker compose run --rm dev make <target>` for one-off commands.
  - `make shell` first, then run `make <target>` from inside the container for multi-step interactive work.
- For non-trivial C refactors, check editor diagnostics first, then run containerized tests. Static diagnostics are useful for fast feedback, but they do not reliably catch nginx-version-specific or preprocessor-heavy regressions in this repo.

## Current ngx_cache_pilot assumptions

- `ngx_cache_pilot` currently has a single shared-memory index backend. Do not reintroduce backend selectors or store ops indirection unless a real second backend is being added.
- Indexed tag purges no longer bootstrap the index on demand inside the request path. If the target cache zone is not registered for indexing or its index is not ready, the purge declines.
- Protocol-specific purge integration is table-driven in `src/ngx_cache_pilot_module.c`. Prefer extending the shared protocol descriptor/helpers instead of copying per-protocol handler or attach logic.
- Request-level purge metrics are intentionally centralized through shared helpers in `src/ngx_cache_pilot_module.c`. Keep new `purges_*` and key-index counters routed through those helpers instead of scattering `NGX_CACHE_PILOT_METRICS_INC()` calls.

## Build notes

- On dynamic builds using `--add-dynamic-module`, a missing `ngx_modules` symbol at runtime points to a packaging or build-flow problem outside the normal nginx `auto/module` path.
- The dynamic `.so` still depends on nginx protocol modules such as fastcgi, proxy, scgi, and uwsgi; packaged deployments may require those modules to be statically linked into nginx or loaded before this module.

## Purge semantics

- Same-location `xxx_cache_purge` syntax stores a literal method token in `ngx_http_cache_pilot_conf_t.method`; it is not compiled as a complex value, so variables like `$foo` are not evaluated there.
- `ngx_http_cache_pilot_access_handler()` only activates purge when the request method exactly matches the configured token and the client address passes the optional `from` CIDR list; otherwise request handling falls through to the original upstream handler.
- `cache_purge_mode_header` values `soft`, `true`, or `1` only switch soft versus hard purge mode after a purge request has already matched; they do not enable purge conditionally.

## README hygiene

After any change that affects user-visible behaviour, known limitations, configuration options, or the public API, **review `README.md` and update it** to reflect the current state before closing the task. Pay particular attention to the **Known issues** section, which documents known limitations — keep each entry accurate and up to date.
