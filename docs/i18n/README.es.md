

<a id="contents"></a>

## Contenido

- [Funciones](#features)
- [Compatibilidad](#compatibility)
- [Paquetes de distribuciones](#distribution-packages)
- [Instalación](#installation)
- [Uso](#usage)
  - [Purga por clave parcial](#partial-key-purge)
  - [Invalidación por generaciones](#generation-invalidation)
  - **REFRESH** — [Comprobar la caché con el origen (REFRESH)](#conditional-refresh)
- [Ejemplos de configuración](#localized-examples)
- [Seguridad](#security)
- [Migración](#migration)
- [Documentación](#documentation)
- [Véase también](#see-also)
- [Licencia](#license)

<a id="features"></a>

## Funciones

- **Purga integrada** — método HTTP específico (`PURGE`) con control de acceso por IP
- **Location separado** — sintaxis zona/clave `proxy_cache_purge zone key` para claves capturadas mediante expresiones regulares, sin `proxy_pass`
- **Comodines / purga parcial** — un `*` final recorre el directorio y elimina las entradas coincidentes
- **`purge_all`** — elimina todas las entradas de la zona en una petición
- **Cola en segundo plano** — recorrido diferido en pasos acotados del bucle de eventos o mediante un pool opcional de hilos NGINX; lotes y pausas configurables
- **Compatibilidad con Vary** — tras una purga exacta, elimina todas las variantes en disco con la misma clave, incluidas gzip y Vary
- **Formatos de respuesta** — `json` (predeterminado), `html`, `xml`, `text`, `none`
- **Pools con nombre** — selección heredada y ejecución incremental sin hilos y en Windows
- **Deduplicación activa** — una operación indexada y como máximo una repetición pendiente para purgas recibidas durante el recorrido
- **Estado** — métricas de cola, borrado y HIT/MISS por caché, sin tomar el mutex compartido al leer
- **Claves de generación** — invalidación exacta, por prefijo o espacio de nombres sin E/S de disco ligada a la petición; las escrituras antiguas tardías siguen inaccesibles
- **[Comprobar la caché con el origen (REFRESH)](#conditional-refresh)**

<a id="compatibility"></a>

## Compatibilidad

En producción, use una versión de NGINX que siga recibiendo mantenimiento. POSIX/UNIX es la plataforma principal; Windows es experimental. Los hilos son opcionales. [NGINX / CI](../../t/TESTING.md#compatibility-matrix).

<a id="distribution-packages"></a>

## Paquetes de distribuciones

Varias distribuciones y proyectos de ports empaquetan este fork, pero no necesariamente v4. Antes de instalar, comprueba la versión del paquete y la ABI de NGINX. Consulta los [paquetes y sus fuentes](../DISTRIBUTIONS.md).

<a id="installation"></a>

## Instalación

```bash
cd /path/to/nginx-source
./configure --add-module=/path/to/ngx_cache_purge
make
make install
```

Módulo dinámico:

```bash
./configure --with-compat --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

Use el código fuente de la misma versión de NGINX instalada en el servidor. `--with-compat` no garantiza la compatibilidad entre versiones arbitrarias de NGINX o bibliotecas C. Consulte [módulos dinámicos y archivos de publicación](../BINARY_MODULES.md) (en inglés).

<a id="usage"></a>

## Uso

[→ Ejemplos de configuración](#localized-examples)

<a id="partial-key-purge"></a>

### Purga por clave parcial

Añada `*` a la **clave de caché evaluada** para buscar un prefijo sin distinguir mayúsculas. Las claves exactas y Vary sí las distinguen. El prefijo de limpieza física admite hasta 511 bytes sin el `*` final. Uno más largo se registra una vez y devuelve `400 invalid_selector` sin recorrer archivos.

Con `cache_purge_key`, se compara la expresión lógica y se aceptan hasta 8096 bytes, incluido el `*` final.

Si la expresión termina en `$uri`, esta petición selecciona un prefijo URI:

```http
PURGE /images/header*
```

El asterisco debe ser el último byte de la clave evaluada, no solo de la ruta URI. Tenga en cuenta query strings y sufijos variables de `proxy_cache_key`. Una clave compuesta solo por `*` recorre todas las entradas elegibles.

<a id="generation-invalidation"></a>

### Invalidación por generaciones

Las generaciones de claves impiden que nuevas solicitudes obtengan contenido antiguo después de PURGE, incluso si una respuesta se escribe tarde. Los archivos se eliminan por separado. Un espacio de nombres comparte reglas de invalidación; conserve su directorio de estado entre reinicios.

<a id="conditional-refresh"></a>

### Comprobar la caché con el origen (REFRESH)

REFRESH envía HEAD para la misma representación en el origen con los valores ETag y Last-Modified guardados. Si la página del producto ya está obsoleta, use PURGE. REFRESH no descarga otro cuerpo, no llena una caché vacía ni prolonga el TTL. [HTTP / REFRESH](../REFRESH.md).

| Resultado bruto del origen | Acción / resultado |
|---|---|
| `304` | Conservar; `kept` |
| `200`, `404`, `410` | Invalidación condicional de generación y limpieza diferida; `invalidated` |
| Otro estado | Conservar; `upstream_status` |
| Sin estado / fallo de transporte | Conservar; `transport_error` |
| Metadatos Vary | Sin HEAD; `skipped_vary` |
| Mapeo o perfil inválido | Conservar; `skipped_profile` |
| Generación reemplazada entretanto | Sin nueva invalidación; `raced_replaced` |
| Fallo interno de invalidación/asignación | Conservar; `invalidation_error` |
| Trabajo no enviado/terminado al detenerse la página | `undispatched`; el cursor no lo supera |

<a id="localized-examples"></a>

## Ejemplos de configuración

Los ejemplos completos incluyen el catálogo con REFRESH y las configuraciones clásicas de PURGE. [Ejemplos de configuración en inglés](../EXAMPLES.md).


<a id="choosing-a-mode"></a>

### Elegir el comportamiento adecuado

| Objetivo | Configuración | Significado de la respuesta |
|---|---|---|
| Eliminar entradas obsoletas y esperar al intento de borrado | PURGE sin cola en segundo plano | `200 purged` o `200 no_match`; 4xx/5xx si falla |
| Programar la limpieza por prefijo o de toda la caché | PURGE con `cache_purge_background_queue on` | `202 accepted`; las entradas antiguas aún pueden servirse mientras esperan |
| Evitar que nuevas solicitudes usen contenido antiguo, incluso tras escrituras tardías | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`; nueva generación de clave, archivos eliminados después |
| Comprobar cambios en el origen | Generaciones + `proxy_cache_refresh` + perfil interno | `200` con resultados por entrada y continuación; no precarga contenido |

Los mecanismos se combinan: cambiar la generación también programa la limpieza. La cola por sí sola no protege de escrituras tardías. Consulte el [ejemplo v4](#localized-examples), los [ejemplos PURGE](#localized-examples) y las [respuestas HTTP](../HTTP_RESPONSES.md) (en inglés).

<details>
<summary>Ejemplos de configuración — 7 ejemplos</summary>

<a id="generation-and-refresh-example"></a>

### Ejemplo: catálogo de productos

Este ejemplo completo escucha localmente en el puerto 8080 y reenvía las solicitudes a la aplicación del puerto 8081. La aplicación debe servir `/products/42` con Host `shop.example.com`. Ajuste la dirección, el dominio y los directorios de caché y estado con permisos de escritura.

`shop.example.com` es el dominio de la cabecera HTTP Host, no un directorio. `/products/42` es el URI del producto. `catalog:` es un prefijo fijo de clave, no un nombre de host. El prefijo y el URI completo, incluida la cadena de consulta, forman la clave lógica, como `catalog:/products/42`. El perfil REFRESH quita `catalog:` y comprueba `/products/42` en la misma aplicación.

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

Solicite por GET un producto existente y almacenable en caché hasta obtener `X-Cache-Status: HIT`; después envíe REFRESH:

```sh
curl -i 'http://127.0.0.1:8080/products/42'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/42' \
  'http://127.0.0.1:8080/admin/refresh'
```

Si el producto ha cambiado, envíe PURGE. Aquí `200 invalidated` impide que las nuevas solicitudes usen la generación antigua; el siguiente GET consulta la aplicación.

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/products/42'
```

Para comprobar todos los productos, use `catalog:/products/*`. Con `complete: false`, mantenga el selector y sustituya `<next_cursor>` por el valor recibido. Resuelva los errores HTTP antes de continuar.

```sh
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

<a id="sample-configurations"></a>

### Ejemplos de configuración

Los cinco primeros bloques son ejemplos completos e independientes de `nginx.conf`. El último añade el tratamiento de Vary a una sección `http` existente. NGINX recibe conexiones locales en el puerto 8080; `catalog_backend` identifica el grupo de servidores de la aplicación en `127.0.0.1:8081`. `shop.example.com` es el HTTP Host enviado a la aplicación. Adapte las direcciones y use un directorio de caché en el que NGINX pueda escribir.

<a id="basic--inline-purge-method"></a>

#### Básico — método de purga integrado

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
# 200 — entrada purgada
# 200 + Cache-Purge-Code: no_match — no hay entrada de caché coincidente
# 403 — IP del cliente no permitida
```

<a id="basic--separate-purge-location"></a>

#### Básico — location de purga separado

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

        # Capturar la clave desde el URI y pasarla como zona/clave.
        # Aquí no se requiere proxy_pass ni proxy_cache.
        location ~ ^/purge(/.*) {
            allow             127.0.0.1;
            deny              all;
            proxy_cache_purge main "$host$1$is_args$args";
        }
    }
}
```

<a id="background-queue-enabled"></a>

#### Cola en segundo plano activada

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

#### Purgar todas las entradas

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
# 200 — recorrido completado; revisar error_log si hay errores de archivo
```

<a id="json-responses-with-per-location-override"></a>

#### Respuestas JSON con ajuste por location

```nginx
events {}
http {
    upstream catalog_backend { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;

    server {
        listen 127.0.0.1:8080;
        server_name shop.example.com;

        # Valor predeterminado para todas las location del servidor.
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
            # Cambiar a texto sin formato para scripts
            cache_purge_response_type text;
            proxy_cache_purge         main "$arg_key";
        }
    }
}
```

<a id="vary-aware-purge"></a>

#### Purga compatible con Vary

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

Tras purgar una clave, se eliminan automáticamente todas sus variantes gzip y Vary en otras rutas.

</details>

<a id="security"></a>

## Seguridad

- Restrinja endpoints de purga con `allow` / `deny` o autenticación previa. No los exponga públicamente.
- Ajuste `cache_purge_queue_size` al tráfico para limitar SHM ante inundaciones de peticiones.
- Considere `limit_req` para limitar purgas independientemente del tráfico normal.

<a id="migration"></a>

## Migración

Los cambios de estructura SHM requieren un reinicio completo. Se conservan la sintaxis método/ACL y los manejadores de protocolo. v4 usa JSON por defecto, códigos coincidentes en cabecera y cuerpo y `200 no_match` cuando falta la entrada. `cache_purge_legacy_status` solo genera una advertencia. Actualice los clientes según el [contrato HTTP](../HTTP_RESPONSES.md). La capacidad incluye tareas asignadas; la sobrecarga espera admisión. Los duplicados activos pueden programar otra pasada.

<a id="documentation"></a>

## Documentación

- [Referencia de directivas](DIRECTIVES.es.md)
- [Más ejemplos de configuración](#localized-examples)
- [Operación y diagnóstico](OPERATIONS.es.md)
- [HTTP y límites de REFRESH (inglés)](../REFRESH.md)
- [Migración a v4](MIGRATION.es.md)
- [Arquitectura de implementación (inglés)](../ARCHITECTURE.md)
- [Pruebas](OPERATIONS.es.md#testing)
- [Desarrollo y pruebas (inglés)](../../t/TESTING.md)

<a id="see-also"></a>

## Véase también

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [Documentación de nginx](https://nginx.org/en/docs/)

<a id="license"></a>

## Licencia

```
Copyright (c) 2009-2014, FRiCKLE <info@frickle.com>
Copyright (c) 2009-2014, Piotr Sikora <piotr.sikora@frickle.com>
Copyright (C) 2016-2026, Denys Denysov

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
