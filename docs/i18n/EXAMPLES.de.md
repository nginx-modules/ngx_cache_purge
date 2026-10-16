[ngx_cache_purge](README.de.md)

<a id="sample-configurations"></a>

# Beispielkonfigurationen

Die ersten fünf Blöcke sind jeweils vollständige, alternative `nginx.conf`-Beispiele. Der letzte Block ergänzt die Vary-Behandlung in einem vorhandenen `http`-Abschnitt. NGINX nimmt lokale Anfragen auf Port 8080 an; `catalog_backend` bezeichnet die Upstream-Gruppe für die Anwendung unter `127.0.0.1:8081`. `shop.example.com` ist der an die Anwendung gesendete HTTP-Host. Passen Sie die Adressen und das für NGINX beschreibbare Cache-Verzeichnis an Ihre Umgebung an.

<a id="basic--inline-purge-method"></a>

### Basis — Inline-Purge-Methode

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

### Basis — separate Purge-Location

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

### Aktivierte Hintergrundwarteschlange

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

### Alle Einträge löschen

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

### JSON-Antworten mit Überschreibung je Location

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

### Purge mit Vary-Unterstützung

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

Nach dem Purge eines Schlüssels werden auch seine gzip- und Vary-Varianten an anderen Dateipfaden automatisch entfernt.
