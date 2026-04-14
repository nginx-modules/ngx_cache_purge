2026-04-14    VERSION 2.6.0
    * feat: add soft purge support for exact-key, wildcard, purge_all, and separate-location syntax
    * fix: soft purge now expires matching cache entries in place instead of deleting them outright
    * fix: wildcard and purge_all soft purges now expire both the on-disk cache header and the shared-memory cache node consistently
    * fix: propagate soft-purge disk-header update failures instead of reporting false success
    * test/docs: add proxy soft-purge coverage, docker validation config, and updated validation commands
    * ci: switch to GitHub Actions and add docker-based test helpers

2020-06-27    VERSION 2.5.1
    * fix: empty key check - it coredumps when cache key is empty, Tuğrul Topuz
    * fix: purge report calloc fix - Report template has not cache file path but it's length is use in buffer memory allocation, Tuğrul Topuz

2018-08-04    VERSION 2.5
    * feat/docs: cache_purge_response_type directive, selecting response type (html|json|xml|text)
    * break: changed status of HTTP code 404 (Not Found) to 412 (Precondition Failed)
    * fix: remove path information of response body (#4, 3a8c08a, #11)

2020-06-27    VERSION 2.4.3
    * fix: empty key check - it coredumps when cache key is empty, Tuğrul Topuz

2017-09-28    VERSION 2.4.2
    * fix: segfault in call to `ngx_read_file` of partial key purge, Frankie Dintino
    * fix: segfault in `cplcf->conf->purge_all` with separate location syntax, Frankie Dintino

2017-02-21    VERSION 2.4.1
    * Fix compatibility with nginx-1.11.6+, Sułowicz Paweł

2016-11-20    VERSION 2.4
    * Fix compatibility with nginx-1.7.12+.
    * explain the purge logic
    * feat(purge all): Include option to purge all the cached files
      This option can be slow if a lot of content is cached, or if the
      storage used for the cache is slow. But you really should be using
      RAM as your cache storage.
    * feat(partial keys): Support partial keys to purge multiple keys.
      Put an '*' at the end of your purge cache URL.
      e.g:
          proxy_cache_key $scheme$host$uri$is_args$args$cookie_JSESSIONID;
          curl -X PURGE https://example.com/pass*
      This will remove every cached page whose key cache starting with:
          httpsexample.com/pass*
      Be careful not passing any value for the values after the $uri, or put
      it at the end of your cache key.
    * Convert a config file to build a dynamic module

2014-12-23    VERSION 2.3
    * Fix compatibility with nginx-1.7.9+.

2014-12-02    VERSION 2.2
    * Fix compatibility with nginx-1.7.8+.

2014-05-19
    * Fix build on Solaris with SunCC (Solaris Studio).
      Reported by Jussi Sallinen.

2013-03-19    VERSION 2.1
    * When enabled, cache purge will now catch all requests with
      PURGE (or specified) method, even if cache isn't configured.
      Previously, it would pass such requests to the upstream.

2012-12-07    VERSION 2.0
    * Add alternative "same location" syntax.
      From CloudFlare.

2012-07-02    VERSION 1.6
    * Fix compatibility with nginx-1.3.2+.
      Reported by MagicBear, patch from Ruslan Ermilov.

2011-12-20    VERSION 1.5
    * Fix on-disk cache size calculation.
      Since the initial release, recorded on-disk cache size was
      decreased twice for purged content (once during cache purge
      and once during subsequent cache update).
      This resulted in recorded on-disk cache size being much
      smaller than in reality, which could lead to on-disk cache
      outgrowing defined "max_size" parameter.
      Patch from Pyry Hakulinen.

2011-10-05    VERSION 1.4
    * Add AIO support.
      Requested by Emin Hasanov.

2011-05-03    VERSION 1.3
    * Fix compatibility with nginx-1.0.1.
      Reported by Sergey A. Osokin and Markus Linnala.

2010-08-29
    * Fix compatibility with nginx-0.8.0 and versions older than
      nginx-0.7.60.

2010-08-11    VERSION 1.2
    * Fix various build scenarios with disabled upstream modules.
      Reported by Johan Bergstroem.

    * Add ability to purge content from SCGI's cache.
      Requested by Johan Bergstroem.

2010-06-08    VERSION 1.1
    * Fix compatibility with nginx-0.8.40+.

    * Add ability to purge content from uWSGI's cache.

2010-01-10    VERSION 1.0
    * Initial module release.

2009-11-17
    * Fix patch compatibility with nginx-0.8.11+.
      Reported by Bing Ran.

2009-08-11
    * Initial patch release.
