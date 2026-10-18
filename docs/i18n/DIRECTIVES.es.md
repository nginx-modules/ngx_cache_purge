[ngx_cache_purge](README.es.md)

<a id="directives"></a>

# Directivas

<a id="proxy_cache_purge"></a>

## `proxy_cache_purge`

```
Syntax:  proxy_cache_purge on | off;
         proxy_cache_purge <method> [purge_all] from all | <cidr> ...;
         proxy_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

**Forma integrada** (`from …`) — intercepta el método indicado en un location proxy y purga su entrada. `on` abrevia `PURGE`; `off` desactiva la purga. Restrinja mediante CIDR o permita todos con `from all`. `purge_all` antes de `from` vacía la zona completa sin depender de la URI.

**Location separado** (zona y clave) — solo se permite en un location de purga, normalmente una regex que captura la clave. Busca la zona por nombre y purga la expresión de clave compilada. Incompatible con `proxy_cache` y `proxy_pass` en el mismo location.

<a id="fastcgi_cache_purge"></a>

## `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Equivale a `proxy_cache_purge` para zonas FastCGI configuradas mediante `fastcgi_cache` / `fastcgi_cache_path`.

<a id="scgi_cache_purge"></a>

## `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Equivale a `proxy_cache_purge` para zonas SCGI configuradas mediante `scgi_cache` / `scgi_cache_path`.

<a id="uwsgi_cache_purge"></a>

## `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Equivale a `proxy_cache_purge` para zonas uWSGI configuradas mediante `uwsgi_cache` / `uwsgi_cache_path`.

<a id="cache_purge_response_type"></a>

## `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

Selecciona el formato de las respuestas PURGE satisfactorias; JSON es el predeterminado. `Cache-Purge-Code` coincide con el código del cuerpo. Las respuestas del módulo incluyen `Cache-Control: no-store`; los errores usan `application/problem+json` según RFC 9457. Las claves de texto se escapan; las binarias se codifican en Base64 con una indicación de codificación.

`none` omite el cuerpo PURGE: éxito completado devuelve `204`, trabajo en segundo plano aceptado conserva `202` y errores conservan 4xx/5xx. Estado y REFRESH mantienen JSON estructurado. Consulte el [contrato HTTP](../HTTP_RESPONSES.md) para los códigos, campos y límites.

<a id="cache_purge_background_queue"></a>

## `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

Activa procesamiento diferido de comodines y `purge_all`. Devuelve `202 Accepted` tras encolar o agrupar con una tarea idéntica pendiente o activa. No afecta las claves exactas.

Controla el comportamiento heredado de comodines/purga completa. Una vinculación `cache_purge_key` siempre usa borrado diferido y respuestas lógicas `200`, incluso con esta opción desactivada.

Si falta capacidad o memoria para la tarea, la petición permanece abierta hasta su admisión, sin recorrer directorios. Un duplicado pendiente/activo puede aceptarse con la cola llena. Fallar al reservar el pequeño estado de espera devuelve `500`. La limpieza por desconexión cancela el temporizador y libera el contador de espera.

Para este indicador y `cache_purge_vary_aware` prevalece el último valor declarado. Se rechazan valores inválidos. `cache_purge_legacy_status` está obsoleto y se ignora. Hilos y ciclo de vida: [modelo de ejecución](../ARCHITECTURE.md#execution-model).

<a id="cache_purge_queue_size"></a>

## `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

Máximo de tareas sin finalizar entre todas las rutas. Pendientes y asignadas consumen capacidad. Duplicados reutilizan una asignación; una repetición conserva memoria y plaza. Las tareas distintas esperan una plaza libre.

La zona compartida se crea al configurar trabajo de fondo o endpoint de estado. Cambiar capacidad exige reinicio aunque el tamaño redondeado no cambie. Reload rechaza redimensionar para preservar trabajo aceptado. Añadir raíces también puede exigir memoria y reinicio. Drene previamente las tareas pendientes/activas que deban finalizar.

Declarar un espacio de nombres también crea la cola.

La zona incluye tareas, metadatos de rutas y métricas alineadas de workers. Depende de la compilación y las rutas; consulte [dimensionamiento de memoria](../OPERATIONS.md#memory-sizing).

<a id="cache_purge_thread_pool"></a>

## `cache_purge_thread_pool`

```
Syntax:  cache_purge_thread_pool <name>;
Default: default
Context: http, server, location
```

Selecciona el ejecutor de las tareas de fondo aceptadas, con herencia normal. Ejemplo:

```nginx
thread_pool purge_io threads=2 max_queue=128;
http {
    cache_purge_background_queue on;
    cache_purge_thread_pool purge_io;
}
```

NGINX valida pools y entrega finalización mediante el evento nativo; no se copian diseños privados ni colas de finalización. Con hilos, los nombres personalizados deben existir. Sin hilos o en Windows, se acepta la directiva y se usan pasos acotados del bucle. Un pool lleno retrasa la ejecución sin devolver E/S al bucle. Pools dedicados añaden pilas y cambios de contexto.

<a id="cache_purge_batch_size"></a>

## `cache_purge_batch_size`

```
Syntax:  cache_purge_batch_size <number>;
Default: 10
Minimum: 1
Maximum: 64
Context: http
```

Máximo de tareas por worker entre pausas. El avance se conserva entre pasos y finalizaciones de hilos. Cola vacía termina el lote. Valores superiores a 64 se limitan con aviso; cero se rechaza.

Controla tareas, no archivos ni duración del callback. Valores menores introducen más pausas entre tareas, sin interrumpir un recorrido activo en hilo. Ajuste junto con `cache_purge_throttle_ms` para regular el disco.

<a id="cache_purge_throttle_ms"></a>

## `cache_purge_throttle_ms`

```
Syntax:  cache_purge_throttle_ms <time>;
Default: 10ms
Minimum: 1ms
Context: http
```

Pausa entre lotes. Acepta `10ms`, `500ms`, `1s` y compuestos entre comillas como `"2s 500ms"`. Un entero sin unidad significa milisegundos: `10` y `10ms` equivalen. La ausencia de unidad genera aviso; especificarla lo evita.

Un worker inactivo vuelve a comprobar tras diez veces ese intervalo. Pasos incompletos y mutex ocupado usan temporizadores de 1 ms. Son intervalos mínimos de planificación, no plazos. Se rechazan cero y valores fuera del rango seguro de la plataforma.

Admisión, presión del pool y reintentos diferidos usan sondeo de 10 ms.

<a id="cache_purge_queue_status"></a>

## `cache_purge_queue_status`

```
Syntax:  cache_purge_queue_status <cache_path>;
Default: not configured
Context: location
```

Sirve una instantánea JSON GET/HEAD con `Cache-Control: no-store`. La ruta debe identificar una raíz configurada; una desconocida impide cargar la configuración incluso sin trabajo de fondo. Restrinja acceso al endpoint:

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

`queue` describe la cola compartida entre todas las rutas:

| Campo | Tipo / unidad | Significado |
|---|---|---|
| `size` | Entero / tareas | Tareas esperando worker |
| `running` | Entero / tareas | Asignadas, incluidas las que esperan pool |
| `max_size` | Entero / tareas | Capacidad conjunta de pendientes y asignadas |
| `wait_time` | Entero / milisegundos | Edad de la pendiente más antigua; cero si vacía |
| `full` | Booleano | La muestra de pendientes más asignadas alcanza la capacidad |

`cache` describe la raíz seleccionada por este location:

| Campo | Tipo / unidad | Significado |
|---|---|---|
| `pending.total` | Entero / tareas | Pendientes de esta caché |
| `pending.full` | Entero / tareas | `purge_all` pendientes, incluidas en `pending.total` |
| `active` | Entero / tareas | Asignadas hasta terminar la contabilización |
| `requests.overflow` | Entero / peticiones | Peticiones que esperaron admisión, incluso por bloqueo/asignación |
| `requests.deduplicated` | Entero / peticiones | Agrupadas con trabajo pendiente o activo |
| `requests.waiting` | Entero / peticiones | Esperas actuales de admisión para esta caché |
| `files.deleted` | Entero / archivos | Borrados de fondo correctos comunicados al acabar la pasada |
| `files.errors` | Entero / errores | Fallos de borrado, excepto archivo ya ausente |
| `files.bytes` | Entero / bytes | Tamaño lógico estimado de archivos borrados |
| `responses.hit`, `responses.miss` | Entero / peticiones principales | Decisiones HIT/MISS nativas observadas en fase de log |
| `responses.hit_ratio` | Número / 0..1 | HIT / (HIT + MISS), o cero sin observaciones |
| `logical.invalidations` | Entero / decisiones | Invalidaciones publicadas, incluidos duplicados y REFRESH |
| `cleanup.scan_errors` | Entero / errores | Fallos de metadatos/apertura/lectura/cierre/asignación de fondo |
| `cleanup.retries` | Entero / pasadas | Pasadas adicionales por fallo de limpieza |
| `cleanup.exhausted` | Entero / tareas | Tareas retiradas tras tres reintentos fallidos |

`namespaces` incluye las entradas `cache_purge_namespace` configuradas, con independencia del caché elegido por esta location: todas las rutas de un espacio de nombres comparten su estado de generaciones. Cada entrada se copia bajo el bloqueo de su espacio de nombres. Las entradas se leen por separado; la lista no es una instantánea global.

| Campo | Tipo / unidad | Significado |
|---|---|---|
| `name` | Cadena | Nombre del espacio de nombres en el prefijo de clave `p4:<name>:` |
| `epoch` | Cadena / 10 dígitos | Época actual del espacio de nombres; un reinicio completo crea otra |
| `baseline` | Cadena / 16 dígitos hexadecimales en minúsculas | Generación de las claves que ninguna regla ha elevado; una invalidación de todo el espacio la establece en `serial` |
| `serial` | Cadena / 16 dígitos hexadecimales en minúsculas | Número de secuencia que empieza en 1 y aumenta con cada invalidación publicada en esta época |
| `rules` | Booleano | `true` si existen reglas exactas o de prefijo; una invalidación de todo el espacio las elimina, incluso cuando la falta de memoria compartida amplía la invalidación |

`baseline` y `serial` tienen el mismo ancho fijo que la generación de una clave y se comparan como cadenas. `namespaces_complete` es `false` cuando la lista se recorta para caber en la respuesta. Las entradas devueltas siguen siendo utilizables; un espacio de nombres ausente se considera desconocido. Véase [lectura de archivos de caché en modo de generaciones](OPERATIONS.es.md#reading-cache-files).

`pending.full` cuenta purgas completas. El comodín de clave `*` es otro tipo de operación. `active` incluye espera por pool; se excluye de `queue.size` y `pending.total`, pero ocupa capacidad.

En un barrido agrupado, `pending.full` refleja la clasificación de la primera tarea aceptada. Exploraciones/probes REFRESH no cuentan en profundidad de cola PURGE ni errores de borrado. Rotar un reintento diferido puede hacer que `wait_time` muestre la edad de la cabeza muestreada y no la máxima.

Los contadores tienen un escritor del bucle por slot de proceso; inicio y separación se alinean a línea de caché. Los hilos devuelven resultados por eventos. El lector no toma mutex ni ejecuta atomic fetch-add, tampoco con incremento cero. En 64 bits usa cargas/almacenamientos volatile alineados normales; en 32 bits, comprobaciones acotadas de secuencia protegen lecturas de 64 bits y pueden omitir un segmento ocupado. Las cifras son aproximadas, no una transacción entre workers. La admisión utiliza estado de control bloqueado, nunca métricas.

Los acumuladores saturan en el máximo unsigned de 64 bits, sobreviven a reload y se reinician al arrancar. El indicador de espera sigue la limpieza de peticiones. Se excluyen operaciones síncronas y borrados del cache manager. Bytes estiman longitudes, no bloques ni espacio liberado: lectores abiertos y escrituras concurrentes influyen. Se cuentan resultados parciales; pueden faltar los de workers caídos. No son un registro de auditoría.

HIT/MISS solo cuenta peticiones principales con decisión de caché nativa. Se excluyen upstream ausente, estado cero, BYPASS, EXPIRED, STALE, UPDATING y REVALIDATED. Las peticiones normalmente no cacheadas no se convierten en MISS. La monitorización sola puede crear SHM sin purga de fondo. En 32 bits, la edad cubre menos de unos 24,9 días; una diferencia de reloj con signo negativa se informa como cero.

<a id="cache_purge_legacy_status"></a>

## `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

Directiva obsoleta, aceptada solo para migrar configuraciones. Ambos valores generan una advertencia y se ignoran; elimine la directiva. Los valores inválidos se rechazan. Sin coincidencias en una operación síncrona se devuelve `200 no_match`; limpieza física aceptada, `202 accepted`; invalidación de generación publicada, `200 invalidated`. Ya no se alterna entre `412` y `404`. Consulte el [contrato HTTP](../HTTP_RESPONSES.md).

<a id="cache_purge_vary_aware"></a>

## `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

Con `on`, tras borrar el archivo principal en una purga exacta correcta, recorre el directorio y elimina los restantes con la misma cadena `KEY:`. Incluye variantes `Vary` y `gzip_vary` con una clave lógica común y rutas diferentes. Comprueba `\n` inmediatamente después de la clave para evitar falsas coincidencias de prefijo.

Desactivado por defecto porque cada purge exacto añade un recorrido completo. Actívelo para varias representaciones `Vary`/`gzip_vary`. El recorrido también se realiza si falta la entrada principal. Las máscaras y `purge_all` ya incluyen las variantes. Las claves Vary exactas distinguen mayúsculas y se limitan a 511 bytes; claves más largas devuelven `400 invalid_selector` antes de borrar nada.

La invalidación por generaciones cubre todas las representaciones de la clave lógica sin esta opción.

<a id="cache_purge_namespace"></a>

## `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

Declara un espacio de nombres antes de vincular locations. El nombre admite 1–32 letras ASCII, dígitos, `_` o `-`; SHM requiere ocho páginas nativas como mínimo. Use un directorio de estado persistente dedicado fuera de raíces de caché y temporales. El núcleo crea un marcador persistente exclusivo para reservar una época de reinicio. Conserve esos archivos mientras pueda existir la caché: borrar estado conservando caché permite reutilizar épocas obsoletas. No se garantiza durabilidad frente a pérdida de energía/fsync. Reload conserva época y reglas; cambiar tamaño/ruta o retirar un espacio requiere reinicio completo. SHM del espacio y de la cola se dimensionan por separado.

<a id="cache_purge_key"></a>

## `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

Vincula el location a un espacio existente y expone `$cache_purge_key`. Use esa variable como valor completo de `proxy_cache_key`, `fastcgi_cache_key`, `scgi_cache_key` o `uwsgi_cache_key`, también en el location PURGE separado. Si la clave nativa expandida difiere de la generada, PURGE falla con `500` sin afirmar invalidación estricta. No use `$cache_purge_key` dentro de su propia expresión lógica. Las claves lógicas admiten 8096 bytes. Vincule solo locations participantes si otros necesitan semántica anterior; no hay forma `off` para anular una vinculación heredada.

<a id="proxy_cache_refresh"></a>

## `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

Crea un endpoint REFRESH explícito solo para peticiones principales; otros métodos devuelven `405`. Exige vinculación `cache_purge_key`, `$cache_purge_key` como expresión nativa y perfil refresh. La clave lógica expandida selecciona clave exacta, prefijo con `*` final o `*` para el espacio en esa ruta. Proteja el location con acceso/autenticación nativos. REFRESH solo admite caché proxy; PURGE sigue disponible para los cuatro protocolos.

<a id="cache_purge_refresh_profile"></a>

## `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

Asocia una representación conocida a un location proxy interno literal. Al quitar el prefijo exacto, sensible a mayúsculas/minúsculas, debe quedar una URI de origen iniciada por `/`; se conservan query string y escapes. El módulo no deduce hosts ni busca `://` en claves. El destino exige `internal`, caché desactivada, reenvío de cabeceras/cuerpo desactivado, método HEAD, Host literal no vacío y cabeceras de representación literales. Las cabeceras condicionales deben usar exactamente `$cache_purge_refresh_etag` y `$cache_purge_refresh_last_modified`; `$cache_purge_refresh_uri` suministra la URI a `proxy_pass`. Estas variables puras solo tienen sentido en probes. La validación ocurre en preaccess, incluidos redireccionamientos internos. El administrador debe garantizar la misma representación de origen que identifica la clave; Vary se omite porque los metadatos nativos no conservan las cabeceras originales.

Configure `proxy_intercept_errors off` en el location de probe. Se rechaza la interceptación heredada para que un error de origen no se transforme en éxito invalidante.

<a id="cache_purge_refresh_timeout"></a>

## `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

Limita el envío de nuevos recorridos/probes por página, incluida la espera del explorador del worker. Al expirar, detiene trabajo nuevo y marca respuesta incompleta. HEAD ya activo termina según `proxy_connect_timeout`, `proxy_send_timeout` y `proxy_read_timeout`: configúrelos explícitamente. No es un plazo absoluto de respuesta ni interrumpe una llamada al sistema. Desconexión y cierre ordenado detienen nuevos envíos, conservando memoria del hilo activo hasta la finalización nativa.
