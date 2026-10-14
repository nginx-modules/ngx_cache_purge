<a id="ngx_cache_purge"></a>

# ngx_cache_purge

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

Módulo NGINX para purgar las cachés `FastCGI`, `proxy`, `SCGI` y `uWSGI`. La versión 4.0 conserva la interfaz anterior e incorpora invalidación por generaciones de activación explícita antes del borrado físico diferido, además de REFRESH condicional acotado para cachés proxy.

---

<a id="contents"></a>

## Contenido

- [Funciones](#features)
- [Compatibilidad](#compatibility)
- [Instalación](#installation)
- [Modelo de ejecución](#execution-model)
- [Directivas](#directives)
- [Purga por clave parcial](#partial-key-purge)
- [Invalidación por generaciones](#generation-invalidation)
- [REFRESH condicional](#conditional-refresh)
- [Ejemplos de configuración](#sample-configurations)
- [Ajuste del rendimiento](#performance-tuning)
- [Monitorización y depuración](#monitoring-and-debugging)
- [Resolución de problemas](#troubleshooting)
- [Pruebas](#testing)
- [Migración](#migration)
- [Directorios temporales](#temporary-directories)
- [Seguridad](#security)
- [Licencia](#license)

---

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
- **REFRESH condicional** — perfil HEAD explícito, límite compartido de comprobaciones y páginas de continuación de 32 entradas

---

<a id="compatibility"></a>

## Compatibilidad

La matriz enumera los objetivos configurados en CI. Un trabajo satisfactorio valida esa configuración de fuentes y compilación, no todas las versiones de la serie.

| NGINX | CI: push / PR | CI: + full |
|---|---|---|
| 1.30.5, 1.31.6 | `test-matrix` | — |
| 1.31.6 | `build-compatibility` (GCC/Clang), `sanitizer`, `native-macos`, `native-windows` (x64/x86) | `memory-leak-test` (Valgrind) |
| 1.7.9 | `build-compatibility` (GCC) | — |
| 1.7.11, 1.7.12 | `build-compatibility` (GCC) | — |
| 1.20.2, 1.26.3, 1.28.3 | — | `test-matrix` |
| 1.11.4, 1.11.5 | — | `build-compatibility` (GCC) |

Linux utiliza las mismas imágenes Docker y punto de entrada que las pruebas locales. Los trabajos nativos de macOS y Windows ejercitan sus API de archivos y procesos; Windows compila con MSVC y comprueba HTTP, rutas y reload mediante `nginx.exe`. Los escenarios POSIX de procesos/señales se ejecutan en Linux y macOS. Windows no ejecuta esos escenarios ni la ruta opcional del pool de hilos NGINX.

Sin `--with-threads` se usa recorrido incremental. Con epoll anterior a NGINX 1.7.12 también se usa este modo por una condición de carrera en la notificación del núcleo. BSD no tiene CI nativo. Las ejecuciones normales comprueban stable/mainline y límites de API; las ampliadas añaden versiones anteriores y Valgrind. Los cambios solo de documentación omiten compilaciones. Calendario y comandos: [pruebas](../../t/TESTING.md).

El código se divide en configuración, manejadores, cola, recorrido, invalidación, REFRESH y respuestas. Las funciones comunes de rutas están en `ngx_cache_purge.h`; las operaciones de plataforma, en las cabeceras POSIX/Win32. `generation.*` pasa a llamarse `invalidation.*`; generación sigue designando la versión de las claves. Véase la [arquitectura](../ARCHITECTURE.md).

El módulo sigue el estilo NGINX compatible con C89 y usa las API de eventos, slab, rbtree y operaciones atómicas del núcleo. No requiere atomics C11 ni una API de archivos más reciente. Las compilaciones GNU89 estrictas comprueban orden de declaraciones y avisos; los resultados registran compilador y plataforma realmente utilizados.

---

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
./configure --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

---

<a id="execution-model"></a>

## Modelo de ejecución

Una **tarea** es una operación con comodín o `purge_all`. Un **lote** es el número de tareas que procesa un worker antes de una pausa. Un **paso** es una parte acotada del recorrido incremental; una tarea puede necesitar muchos pasos.

Con generaciones, un barrido físico por ruta y espacio de nombres agrupa todas las invalidaciones aceptadas. Las páginas REFRESH son operaciones HTTP separadas, no tareas de la cola de purga.

| Petición y configuración | Recorrido de archivos | Finalización HTTP |
|---|---|---|
| Clave exacta heredada, con o sin Vary | Worker de la petición; Vary añade un recorrido | Tras la operación |
| Comodín o `purge_all`, cola desactivada | Worker de la petición, recorrido síncrono | Tras el recorrido |
| Tarea en segundo plano, pool seleccionado | Una tarea de pool por worker a la vez | `202` tras admisión |
| Tarea en segundo plano sin pool utilizable | Bucle del worker, pasos incrementales | `202` tras encolar |
| Capacidad/asignación agotada | La petición espera admisión sin recorrer archivos | `202` tras admisión |
| PURGE por generaciones, cualquier clave | Barrido diferido del espacio de nombres; sin abrir/borrar archivos en la petición | `200 invalidated` tras publicación y admisión |
| Endpoint REFRESH | Exploración acotada y hasta 32 comprobaciones HEAD secuenciales | JSON `200` con campos de finalización y continuación |

Las filas sin generaciones describen el comportamiento anterior. Una petición por generaciones que espera capacidad devuelve `200` tras admisión, no `202`.

Cada paso termina tras un máximo de 64 iteraciones de directorio o al observar 5 ms transcurridos. El presupuesto incluye entradas omitidas y finales de directorio; no limita una llamada al sistema individual.

Para trasladar la E/S de fondo a hilos, compile con `--with-threads` y declare un pool del núcleo en el contexto **main**, fuera de `http`:

```nginx
thread_pool default threads=2;
```

`cache_purge_thread_pool <name>` selecciona un pool existente en `http`, `server` o `location`. Por defecto usa `default`, que NGINX crea con valores estándar si se referencia sin declararlo. Otros nombres requieren `thread_pool`. La selección se hereda; la primera petición aceptada elige el ejecutor del trabajo agrupado. Cada worker tiene pools propios. Si el pool está lleno, se reintenta el envío a los 10 ms conservando la plaza de admisión, sin iniciar recorrido síncrono. Sin soporte de hilos, en Windows, en modo de proceso único o si reload elimina el pool de una tarea antigua, se usa el cursor incremental.

REFRESH usa el pool seleccionado para un explorador adicional por worker. Las subpeticiones HTTP nativas se ejecutan en el bucle de eventos; el hilo de E/S nunca accede a su estado.

Cola e índice usan el mutex slab para cambios estructurales. Admisión y callbacks de fondo usan trylock y temporizadores de reintento. Las tareas pendientes **y en ejecución** permanecen en la rbtree de cadenas del núcleo por ruta; comparar claves completas resuelve colisiones hash. Las operaciones cuestan `O(log Q)` para `Q` tareas sin terminar; buscar la ruta es lineal respecto a las rutas registradas. Listas `ngx_queue_t` siguen el trabajo pendiente y activo. La primera duplicación activa solicita repetición; las siguientes se agrupan con ella. Tras la pasada actual, la repetición va al final de la cola para no perder un PURGE posterior. Un flujo continuo puede provocar más pasadas. La plaza se libera al finalizar definitivamente. E/S y formateo de respuestas ocurren fuera del bloqueo.

La rbtree solo deduplica limpieza física. Las reglas de generación viven en tries de bytes acotadas de otra zona SHM. Un barrido estricto tiene identidad de índice distinta de una tarea de clave heredada; REFRESH usa la misma ruta de limpieza.

Cola llena o fallo de asignación mantiene la petición esperando con temporizador de 10 ms y gestión nativa de desconexión. Peticiones y handlers de limpieza permanecen en su worker; SHM nunca guarda sus punteros. No se desactiva la lectura de toda una conexión HTTP/2 o HTTP/3. La presión de cola no genera 429/503; un PURGE corto ya leído no garantiza TCP Zero Window. Las esperas consumen memoria y conexiones: aplique timeouts y políticas de admisión del núcleo al presupuesto de conexiones.

Las tareas aceptadas conservan claves, nombres de pool y protección temporal durante reloads normales. Una duplicación nueva actualiza la protección pendiente o de la siguiente repetición. Desactivar nuevas admisiones conserva el trabajo aceptado. Un worker en cierre ordenado termina su pasada sin tomar otra tarea. La salida rápida devuelve una tarea incremental incompleta a la cola, sin completar el recorrido en el hook de salida. El cierre nativo del pool gobierna la vida de las tareas enviadas a hilos.

Épocas de PID/slot identifican asignaciones. Cada segundo, los workers comprueban hasta 64 registros activos por pasada y reencolan trabajo cuyo propietario terminó. Recupera asignaciones abandonadas; no es un diario transaccional. Un fallo al modificar listas compartidas o slab puede corromper el estado y exigir reinicio. Borrados justo antes del fallo pueden faltar en métricas. Pendientes y repeticiones necesitan un worker activo: drene antes de parar si importa la finalización. La cola no es persistente.

Los diseños internos de la cola no son una ABI de actualización. Cambiar a una compilación con otro diseño exige reinicio completo, drenando antes el trabajo que deba terminar. Reload de la misma compilación reutiliza la zona.

El borrado es best effort. `202` confirma aceptación, no borrado; una pasada completa no es una instantánea atómica. Peticiones concurrentes pueden crear entradas durante o después; los errores se registran. Comodines y purgas completas eliminan archivos sin actualizar síncronamente todos los nodos nativos. Las operaciones exactas y Vary invalidan los nodos coincidentes.

Las pasadas fallidas se repiten a los 100 ms, 1 s y 10 s, como máximo tres veces. Las tareas diferidas conservan su plaza de admisión/deduplicación y permiten otras tareas. Un PURGE nuevo reinicia la ventana. Tras agotar intentos se retira la tarea y se registra un error; consulte `cleanup.exhausted` y repita tras reparar el almacenamiento. La invalidación por generaciones sigue siendo efectiva aunque falle el borrado. Archivos ausentes son carreras esperadas; los malformados se omiten sin reintentos infinitos.

---

<a id="directives"></a>

## Directivas

<a id="proxy_cache_purge"></a>

### `proxy_cache_purge`

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

### `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Equivale a `proxy_cache_purge` para zonas FastCGI configuradas mediante `fastcgi_cache` / `fastcgi_cache_path`.

<a id="scgi_cache_purge"></a>

### `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Equivale a `proxy_cache_purge` para zonas SCGI configuradas mediante `scgi_cache` / `scgi_cache_path`.

<a id="uwsgi_cache_purge"></a>

### `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Equivale a `proxy_cache_purge` para zonas uWSGI configuradas mediante `uwsgi_cache` / `uwsgi_cache_path`.

<a id="cache_purge_response_type"></a>

### `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

Selecciona el formato de las respuestas PURGE satisfactorias; JSON es el predeterminado. `Cache-Purge-Code` coincide con el código del cuerpo. Las respuestas del módulo incluyen `Cache-Control: no-store`; los errores usan `application/problem+json` según RFC 9457. Las claves de texto se escapan; las binarias se codifican en Base64 con una indicación de codificación.

`none` omite el cuerpo PURGE: éxito completado devuelve `204`, trabajo en segundo plano aceptado conserva `202` y errores conservan 4xx/5xx. Estado y REFRESH mantienen JSON estructurado. Consulte el [contrato HTTP](../HTTP_RESPONSES.md) para los códigos, campos y límites.

<a id="cache_purge_background_queue"></a>

### `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

Activa procesamiento diferido de comodines y `purge_all`. Devuelve `202 Accepted` tras encolar o agrupar con una tarea idéntica pendiente o activa. No afecta las claves exactas.

Controla el comportamiento heredado de comodines/purga completa. Una vinculación `cache_purge_key` siempre usa borrado diferido y respuestas lógicas `200`, incluso con esta opción desactivada.

Si falta capacidad o memoria para la tarea, la petición permanece abierta hasta su admisión, sin recorrer directorios. Un duplicado pendiente/activo puede aceptarse con la cola llena. Fallar al reservar el pequeño estado de espera devuelve `500`. La limpieza por desconexión cancela el temporizador y libera el contador de espera.

Para este indicador y `cache_purge_vary_aware` prevalece el último valor declarado. Se rechazan valores inválidos. `cache_purge_legacy_status` está obsoleto y se ignora. Hilos y ciclo de vida: [modelo de ejecución](#execution-model).

<a id="cache_purge_queue_size"></a>

### `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

Máximo de tareas sin finalizar entre todas las rutas. Pendientes y asignadas consumen capacidad. Duplicados reutilizan una asignación; una repetición conserva memoria y plaza. Las tareas distintas esperan una plaza libre.

La zona compartida se crea al configurar trabajo de fondo o endpoint de estado. Cambiar capacidad exige reinicio aunque el tamaño redondeado no cambie. Reload rechaza redimensionar para preservar trabajo aceptado. Añadir raíces también puede exigir memoria y reinicio. Drene previamente las tareas pendientes/activas que deban finalizar.

Declarar un espacio de nombres también crea la cola.

La zona incluye tareas, metadatos de rutas y métricas alineadas de workers. Depende de la compilación y las rutas; consulte [dimensionamiento de memoria](#memory-sizing).

<a id="cache_purge_thread_pool"></a>

### `cache_purge_thread_pool`

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

### `cache_purge_batch_size`

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

### `cache_purge_throttle_ms`

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

### `cache_purge_queue_status`

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

`pending.full` cuenta purgas completas. El comodín de clave `*` es otro tipo de operación. `active` incluye espera por pool; se excluye de `queue.size` y `pending.total`, pero ocupa capacidad.

En un barrido agrupado, `pending.full` refleja la clasificación de la primera tarea aceptada. Exploraciones/probes REFRESH no cuentan en profundidad de cola PURGE ni errores de borrado. Rotar un reintento diferido puede hacer que `wait_time` muestre la edad de la cabeza muestreada y no la máxima.

Los contadores tienen un escritor del bucle por slot de proceso; inicio y separación se alinean a línea de caché. Los hilos devuelven resultados por eventos. El lector no toma mutex ni ejecuta atomic fetch-add, tampoco con incremento cero. En 64 bits usa cargas/almacenamientos volatile alineados normales; en 32 bits, comprobaciones acotadas de secuencia protegen lecturas de 64 bits y pueden omitir un segmento ocupado. Las cifras son aproximadas, no una transacción entre workers. La admisión utiliza estado de control bloqueado, nunca métricas.

Los acumuladores saturan en el máximo unsigned de 64 bits, sobreviven a reload y se reinician al arrancar. El indicador de espera sigue la limpieza de peticiones. Se excluyen operaciones síncronas y borrados del cache manager. Bytes estiman longitudes, no bloques ni espacio liberado: lectores abiertos y escrituras concurrentes influyen. Se cuentan resultados parciales; pueden faltar los de workers caídos. No son un registro de auditoría.

HIT/MISS solo cuenta peticiones principales con decisión de caché nativa. Se excluyen upstream ausente, estado cero, BYPASS, EXPIRED, STALE, UPDATING y REVALIDATED. Las peticiones normalmente no cacheadas no se convierten en MISS. La monitorización sola puede crear SHM sin purga de fondo. En 32 bits, la edad cubre menos de unos 24,9 días; una diferencia de reloj con signo negativa se informa como cero.

<a id="cache_purge_legacy_status"></a>

### `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

Directiva obsoleta, aceptada solo para migrar configuraciones. Ambos valores generan una advertencia y se ignoran; elimine la directiva. Los valores inválidos se rechazan. Sin coincidencias en una operación síncrona se devuelve `200 no_match`; limpieza física aceptada, `202 accepted`; invalidación de generación publicada, `200 invalidated`. Ya no se alterna entre `412` y `404`. Consulte el [contrato HTTP](../HTTP_RESPONSES.md).

<a id="cache_purge_vary_aware"></a>

### `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

Con `on`, tras borrar el archivo principal en una purga exacta correcta, recorre el directorio y elimina los restantes con la misma cadena `KEY:`. Incluye variantes `Vary` y `gzip_vary` con una clave lógica común y rutas diferentes. Comprueba `\n` inmediatamente después de la clave para evitar falsas coincidencias de prefijo.

Desactivado por defecto porque cada purge exacto añade un recorrido completo. Actívelo para varias representaciones `Vary`/`gzip_vary`. El recorrido también se realiza si falta la entrada principal. Las máscaras y `purge_all` ya incluyen las variantes. Las claves Vary exactas distinguen mayúsculas y se limitan a 511 bytes; claves más largas devuelven `400 invalid_selector` antes de borrar nada.

La invalidación por generaciones cubre todas las representaciones de la clave lógica sin esta opción.

<a id="cache_purge_namespace"></a>

### `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

Declara un espacio de nombres antes de vincular locations. El nombre admite 1–32 letras ASCII, dígitos, `_` o `-`; SHM requiere ocho páginas nativas como mínimo. Use un directorio de estado persistente dedicado fuera de raíces de caché y temporales. El núcleo crea un marcador persistente exclusivo para reservar una época de reinicio. Conserve esos archivos mientras pueda existir la caché: borrar estado conservando caché permite reutilizar épocas obsoletas. No se garantiza durabilidad frente a pérdida de energía/fsync. Reload conserva época y reglas; cambiar tamaño/ruta o retirar un espacio requiere reinicio completo. SHM del espacio y de la cola se dimensionan por separado.

<a id="cache_purge_key"></a>

### `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

Vincula el location a un espacio existente y expone `$cache_purge_key`. Use esa variable como valor completo de `proxy_cache_key`, `fastcgi_cache_key`, `scgi_cache_key` o `uwsgi_cache_key`, también en el location PURGE separado. Si la clave nativa expandida difiere de la generada, PURGE falla con `500` sin afirmar invalidación estricta. No use `$cache_purge_key` dentro de su propia expresión lógica. Las claves lógicas admiten 8096 bytes. Vincule solo locations participantes si otros necesitan semántica anterior; no hay forma `off` para anular una vinculación heredada.

<a id="proxy_cache_refresh"></a>

### `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

Crea un endpoint REFRESH explícito solo para peticiones principales; otros métodos devuelven `405`. Exige vinculación `cache_purge_key`, `$cache_purge_key` como expresión nativa y perfil refresh. La clave lógica expandida selecciona clave exacta, prefijo con `*` final o `*` para el espacio en esa ruta. Proteja el location con acceso/autenticación nativos. REFRESH solo admite caché proxy; PURGE sigue disponible para los cuatro protocolos.

<a id="cache_purge_refresh_profile"></a>

### `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

Asocia una representación conocida a un location proxy interno literal. Al quitar el prefijo exacto, sensible a mayúsculas/minúsculas, debe quedar una URI de origen iniciada por `/`; se conservan query string y escapes. El módulo no deduce hosts ni busca `://` en claves. El destino exige `internal`, caché desactivada, reenvío de cabeceras/cuerpo desactivado, método HEAD, Host literal no vacío y cabeceras de representación literales. Las cabeceras condicionales deben usar exactamente `$cache_purge_refresh_etag` y `$cache_purge_refresh_last_modified`; `$cache_purge_refresh_uri` suministra la URI a `proxy_pass`. Estas variables puras solo tienen sentido en probes. La validación ocurre en preaccess, incluidos redireccionamientos internos. El administrador debe garantizar la misma representación de origen que identifica la clave; Vary se omite porque los metadatos nativos no conservan las cabeceras originales.

Configure `proxy_intercept_errors off` en el location de probe. Se rechaza la interceptación heredada para que un error de origen no se transforme en éxito invalidante.

<a id="cache_purge_refresh_timeout"></a>

### `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

Limita el envío de nuevos recorridos/probes por página, incluida la espera del explorador del worker. Al expirar, detiene trabajo nuevo y marca respuesta incompleta. HEAD ya activo termina según `proxy_connect_timeout`, `proxy_send_timeout` y `proxy_read_timeout`: configúrelos explícitamente. No es un plazo absoluto de respuesta ni interrumpe una llamada al sistema. Desconexión y cierre ordenado detienen nuevos envíos, conservando memoria del hilo activo hasta la finalización nativa.

---

<a id="partial-key-purge"></a>

## Purga por clave parcial

Añada `*` a la **clave de caché evaluada** para buscar un prefijo sin distinguir mayúsculas. Las claves exactas y Vary sí las distinguen. El prefijo de limpieza física admite hasta 511 bytes sin el `*` final. Uno más largo se registra una vez y devuelve `400 invalid_selector` sin recorrer archivos.

Con `cache_purge_key`, se compara la expresión lógica y se aceptan hasta 8096 bytes, incluido el `*` final.

Si la expresión termina en `$uri`, esta petición selecciona un prefijo URI:

```http
PURGE /images/header*
```

El asterisco debe ser el último byte de la clave evaluada, no solo de la ruta URI. Tenga en cuenta query strings y sufijos variables de `proxy_cache_key`. Una clave compuesta solo por `*` recorre todas las entradas elegibles.

---

<a id="generation-invalidation"></a>

## Invalidación por generaciones

En un espacio vinculado, PURGE publica nueva generación y programa un barrido deduplicado de generaciones antiguas antes de responder `200` con estado `invalidated`. Esa admisión no abre ni borra archivos. Nuevas peticiones que evalúan por primera vez la clave tras la publicación no alcanzan la generación anterior. La petición fija generación por espacio/clave lógica, incluso entre redireccionamientos internos; una respuesta iniciada puede terminar, pero su escritura tardía no será hit para nuevas peticiones. Coincidencias exactas distinguen mayúsculas; prefijos ignoran caja ASCII. `purge_all` cambia todo el espacio, incluidas otras rutas que lo compartan deliberadamente. El barrido se encola para la ruta solicitada; otras rutas usan expiración nativa o purga posterior.

Una invalidación aceptada no libera disco inmediatamente. Escrituras antiguas posteriores al barrido siguen inaccesibles y se eliminan mediante limpieza posterior o expiración por inactividad. Bajo presión SHM, las reglas se colapsan en una nueva base del espacio: más claves quedan frías sin reaparecer las invalidadas. Cada invalidación posterior recupera hasta 64 nodos separados de la trie. Purgar repetidamente una clave popular no hace crecer el árbol sin límite. La complejidad depende de longitud acotada de clave e índice de tareas, no de archivos; no se promete tiempo real O(1) ni ausencia de inanición en sobrecarga. Admisión puede esperar. Use `proxy_cache_lock` y equivalentes para controlar avalanchas GET posteriores.

<a id="conditional-refresh"></a>

## REFRESH condicional

REFRESH comprueba objetos aún cacheados antes de vencer TTL; no los precarga ni prolonga TTL con `304`. Complementa `proxy_cache_revalidate` y `proxy_cache_background_update`, que gestionan tráfico normal de objetos vencidos. Si el origen ignora validadores y siempre responde `200`, REFRESH se convierte en invalidación más HEAD adicional. Solo son candidatas entradas actuales, válidas y en rutas hash canónicas. Una sustitución física en la misma generación puede invalidarse conservadoramente; comparar generaciones impide que un HEAD antiguo invalide una generación PURGE posterior.

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

Cada worker posee como máximo una página activa y un probe; el presupuesto compartido limita a 16 probes entre workers. Las demás páginas esperan sin asignar arrays de candidatos. Cada página recorre el directorio con el cursor común, conserva hasta 32 candidatos y envía HEAD nativos en serie. Así se acotan memoria y subpeticiones que comparten el pool principal. Peticiones REFRESH idénticas no se deduplican: comparten límite de concurrencia. Sus invalidaciones usan la cola PURGE deduplicada común.

Empiece sin cursor; luego envíe `X-Cache-Purge-Cursor: <next_cursor>` al mismo endpoint y con idéntico selector/perfil hasta que `complete` sea verdadero. El cursor es el último nombre de archivo finalizado de 32 caracteres hexadecimales; cabeceras inválidas o duplicadas devuelven `400`. `scan_complete` indica enumeración satisfactoria; `complete` exige además ausencia de página siguiente y decisiones terminadas. `timed_out` y `scan_error` distinguen interrupción/fallo. `undispatched` cuenta entradas seleccionadas sin terminar; un scan incompleto solo ofrece cota inferior, no total de caché. Si no termina la exploración, no hay probes, resultados vacíos y cursor original sin cambios. Se exponen hashes y estados upstream brutos, no claves ni credenciales.

Cada página vuelve a recorrer el directorio para seleccionar hashes siguientes: memoria acotada a cambio de E/S repetida, `O(files × pages)` por operación completa. La paginación es débilmente consistente ante inserciones/borrados; reinicie si necesita una instantánea posterior completa. Ajuste selectores y timeouts. Esta interfaz administrativa no sirve para explorar frecuentemente cachés activas muy grandes.

<a id="generation-and-refresh-example"></a>

### Ejemplo de generaciones y REFRESH

```nginx
thread_pool purge_io threads=2 max_queue=32;
http {
    upstream origin { server 127.0.0.1:8081; }
    proxy_cache_path /var/cache/nginx/objects levels=1:2 keys_zone=objects:10m;
    cache_purge_namespace objects 1m /var/lib/nginx/purge-epochs;
    cache_purge_thread_pool purge_io;
    cache_purge_response_type json;

    server {
        listen 8080;
        location /objects/ {
            cache_purge_key objects "origin.test$request_uri";
            proxy_cache objects;
            proxy_cache_key $cache_purge_key;
            proxy_cache_lock on;
            proxy_cache_valid 200 10m;
            proxy_set_header Host origin.test;
            proxy_pass http://origin;
            proxy_cache_purge PURGE from 127.0.0.1;
        }
        location = /admin/refresh {
            allow 127.0.0.1; deny all;
            cache_purge_key objects $http_x_cache_key;
            proxy_cache_refresh objects $cache_purge_key;
            cache_purge_refresh_profile /internal/cache-probe origin.test;
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
            proxy_set_header Host origin.test;
            proxy_set_header Authorization "";
            proxy_set_header Cookie "";
            proxy_set_header If-None-Match $cache_purge_refresh_etag;
            proxy_set_header If-Modified-Since $cache_purge_refresh_last_modified;
            proxy_pass http://origin$cache_purge_refresh_uri;
        }
    }
}
```

```sh
curl -X PURGE 'http://127.0.0.1:8080/objects/item'
curl -X REFRESH -H 'X-Cache-Key: origin.test/objects/*'      'http://127.0.0.1:8080/admin/refresh'
# Repeat with X-Cache-Purge-Cursor set to the returned next_cursor.
```

<a id="sample-configurations"></a>

## Ejemplos de configuración

<a id="basic--inline-purge-method"></a>

### Básico — método de purga integrado

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        location / {
            proxy_pass        http://backend;
            proxy_cache       main;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }
    }
}
```

```bash
curl -X PURGE http://localhost/images/logo.png
# 200 — entry purged
# 200 + Cache-Purge-Code: no_match
# 403 — client IP not in the allowed list
```

<a id="basic--separate-purge-location"></a>

### Básico — location de purga separado

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        location / {
            proxy_pass      http://backend;
            proxy_cache     main;
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

### Cola en segundo plano activada

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;
    cache_purge_queue_size        2048;
    cache_purge_batch_size        20;
    cache_purge_throttle_ms       10ms;

    server {
        location / {
            proxy_pass        http://backend;
            proxy_cache       main;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE from 127.0.0.1;
        }
    }
}
```

<a id="purge-all-entries"></a>

### Purgar todas las entradas

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    server {
        location /content {
            proxy_pass        http://backend;
            proxy_cache       main;
            proxy_cache_key   "$host$uri$is_args$args";
            proxy_cache_purge PURGE purge_all from 127.0.0.1;
        }
    }
}
```

```bash
curl -X PURGE http://localhost/content/anything
# 200 — walk completed; inspect error_log for file errors
```

<a id="json-responses-with-per-location-override"></a>

### Respuestas JSON con ajuste por location

```nginx
http {
    proxy_cache_path /var/cache/nginx keys_zone=main:10m;

    cache_purge_background_queue on;

    server {
        cache_purge_response_type json;   # default for all locations in this server
        location / {
            proxy_pass        http://backend;
            proxy_cache       main;
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

### Purga compatible con Vary

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

Tras purgar una clave, se eliminan automáticamente todas sus variantes gzip y Vary en otras rutas.

---

<a id="performance-tuning"></a>

## Ajuste del rendimiento

`queue_size` limita trabajo sin terminar, `batch_size` agrupa tareas por worker y `throttle_ms` separa lotes. Mida latencia, contención de disco y edad de cola antes y después de ajustar. Más capacidad no aumenta el ritmo de vaciado.

Estas configuraciones son puntos de partida, no afirmaciones sobre hardware. Las columnas omiten el prefijo común `cache_purge_`.

| Carga / presupuesto de almacenamiento | `queue_size` | `batch_size` | `throttle_ms` |
|---|---:|---:|---:|
| Host o almacenamiento limitado | 512 | 5 | 25ms |
| Punto de partida general | 2048 | 20 | 10ms |
| Almacenamiento rápido con margen medido | 8192 | 50 | 5ms |
| Ráfagas cortas con vaciado conservador | 8192 | 5 | 50ms |

Por ejemplo, la configuración general:

```nginx
cache_purge_background_queue on;
cache_purge_queue_size        2048;
cache_purge_batch_size        20;
cache_purge_throttle_ms       10ms;
```

Una cola mayor absorbe ráfagas, pero un ritmo sostenido de llegada superior al de finalización la llena. Estime margen adicional con `max(0, arrival_rate - completion_rate) * burst_seconds` tareas, usando tareas/s tras deduplicar. Reserve margen para duraciones desiguales y trabajo acumulado. Esperar admisión añade peticiones/conexiones abiertas; incluya memoria y FDs cuando se sature el almacenamiento.

<a id="memory-sizing"></a>

### Dimensionamiento de memoria

El tamaño de zona se calcula a partir de estructuras nativas:

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

| Componente | Memoria reservada o usada |
|---|---|
| Reserva de tareas | `Q * S`, incluyendo redondeo slab y nombres de clave/pool embebidos |
| Métricas de una ruta | `R * NGX_MAX_PROCESSES`, más alineación; 192 KiB con paso de 192 bytes y 1024 slots |
| Control de cola | `H`, incluidos propietarios de proceso y arrays de épocas |
| Reserva de rutas | `M`, incluidos nombres, raíces rbtree, alineación y arrays de métricas |
| Zona mínima | Ocho páginas nativas |

Para el diseño LP64 medido (`I=184`, `H=20576`, registro de ruta de 104 bytes y métricas de 144), línea de caché de 64 bytes, páginas de 4 KiB y una ruta de 20 bytes, resulta:

| Capacidad | Bytes de zona | MiB de zona |
|---:|---:|---:|
| 512 | 1654784 | 1.58 |
| 2048 | 5373952 | 5.12 |
| 8192 | 20242432 | 19.30 |

Cada tarea usa `I + index_bytes + 1 + pool_name_bytes` antes del redondeo slab. El índice embebido contiene una marca de acción de un byte y la clave heredada o el nombre del espacio para generaciones. `purge_all` heredado guarda solo la marca. Capacidad incluye asignaciones pendientes y activas juntas. Tamaños nativos, línea de caché, `NGX_MAX_PROCESSES` y rutas determinan el resultado; Windows y 32 bits difieren. La protección de reload añade SHM; puede fallar la asignación antes del límite de tareas.

La memoria de proceso es aparte. Cada recorrido activo usa buffer fijo de clave, buffer reutilizable de ruta y frame fijo por directorio abierto. Los frames guardan longitudes de prefijo sin copiar ancestros: espacio `O(depth + longest_path)`, independiente de archivos. Streams de directorio añaden buffers de plataforma. Peticiones en espera mantienen pequeño contexto de temporizador/limpieza sin cursor. Pilas y pools nativos tienen costes separados.

El espacio de nombres tiene tamaño SHM propio. Un explorador REFRESH usa buffer/contexto LP64 fijo de 283864 bytes (32 entradas), más frames y hasta 32 subpeticiones en su pool HTTP. Terminar página libera la petición; continuar inicia otra. Los demás solicitantes en espera no reservan candidatos.

<a id="throughput-and-latency"></a>

### Rendimiento y latencia

Para un worker continuamente ocupado, una aproximación útil es:

```text
B = tasks completed in a batch
T = throttle delay in milliseconds
D = elapsed batch processing time in milliseconds, including slice waits
rate_per_worker ~= 1000 * B / (D + T) tasks/s
```

Omitir `D` da el techo de planificación `1000 * B / T`: 1000 tareas/s por worker por defecto. No es tasa de borrado ni medición. Diez tareas que suman 200 ms y pausa de 10 ms dan unas 47,6 tareas/s. Workers comparten almacenamiento: multiplicar por su número no garantiza rendimiento. Un worker inactivo puede tardar `10 * T`, más retraso del bucle, en buscar trabajo.

Reduzca el lote o aumente la pausa para reducir trabajo entre descansos. Use el pool opcional para menor latencia del worker durante E/S. Ningún ajuste acota una syscall ni divide una tarea con hilo en lotes.

---

<a id="monitoring-and-debugging"></a>

## Monitorización y depuración

El [endpoint de estado](#cache_purge_queue_status) muestra pendientes, activos, esperas, duplicados, resultados de borrado y HIT/MISS. `202 Accepted` confirma admisión/agrupación, no borrado ni invalidación lógica inmediata. Observe latencia junto con edad de cola y contención del almacenamiento.

| Nivel | Condición |
|---|---|
| `warn` | Clave demasiado larga, pool retirado o ajuste limitado |
| `error` | Acceso denegado, fallo de búsqueda/perfil REFRESH, reintentos agotados; fallo de pool nativo o lectura Windows |
| `crit` | Fallo de apertura/stat/borrado o directorio; lectura POSIX nativa |
| `alert` | Fallo de cierre de archivo |
| `emerg` | Configuración inválida, SHM incompatible, redimensionamiento o fallo de reserva de metadatos en reload |
| `debug` | Contexto adicional de petición y recorrido |

Los diagnósticos usan funciones nativas NGINX y empiezan por `ngx_cache_purge: `. NGINX aporta nivel, tiempo, IDs de proceso/hilo, errno y contexto de petición/configuración. Los errores de archivos indican operación real y ruta entre comillas; namespace y REFRESH identifican objeto y motivo del rechazo. Use un analizador estándar de error log y filtre el prefijo.

La depuración usa `NGX_LOG_DEBUG_HTTP` y requiere tanto compilación `--with-debug` como `error_log /var/log/nginx/error.log debug;`.

La presión de cola se representa mediante métricas de espera, no avisos por reintento. Un archivo desaparecido es una carrera esperada sin diagnóstico del módulo. Los auxiliares nativos, incluido parser, asignador, E/S y pools, conservan su formato; examine el log completo al investigar fallos.

```bash
tail -f /var/log/nginx/error.log | grep 'ngx_cache_purge:'
```

Consulte [pruebas de log](../../t/TESTING.md#logging-contracts) para los diagnósticos comprobados.

---

<a id="troubleshooting"></a>

## Resolución de problemas

| Síntoma | Comprobación o ajuste |
|---|---|
| Latencia del worker en recorridos grandes | Active cola y pool opcional para E/S. Exactas/Vary y operaciones sin cola siguen síncronas. |
| Crecen las esperas de admisión | Revise llegadas, edad de cola, pool y disco. Más capacidad absorbe ráfagas; redimensionar exige reinicio. |
| iowait elevado | Reduzca `cache_purge_batch_size`, aumente `cache_purge_throttle_ms` y limite purgas entrantes. |
| Vaciado lento | Mida duración y contención antes de aumentar lote o reducir pausa. |
| El cliente espera los antiguos `412`/`404` | Elimine `cache_purge_legacy_status`; procese `200` con `Cache-Purge-Code: no_match`. |
| Quedan variantes Vary | Active `cache_purge_vary_aware`; revise resultado principal y límite de 511 bytes. |
| Trabajo aceptado parece incompleto | Revise activos, recuperación y errores. `202` no confirma finalización; posible corrupción SHM exige reinicio. |

---

<a id="testing"></a>

## Pruebas

Compile y ejecute la suite completa en Docker:

```sh
make test-all
NGINX_VERSION=1.31.6 make test-all
```

Desde `t/`, siguen disponibles los servicios Compose existentes:

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

Con el plugin Compose use `docker compose` en lugar de `docker-compose`; los Makefiles detectan ambos. `nginx-test` y `make test-all` ejecutan las siete suites `t/*.t` mediante el mismo `prove` que CI. La imagen de integración incluye auxiliares/diseños nativos y ejecución con pool. Las compilaciones estrictas sin hilos prueban ejecución incremental. Cualquier prueba fallida hace fallar el comando.

Consulte [`t/TESTING.md`](../../t/TESTING.md) para compilaciones estrictas, sanitizadores, Valgrind, plataformas e informes. Todos los trabajos GitHub están definidos en `ci.yml`.

---

<a id="migration"></a>

## Migración

<a id="queue-and-status-changes"></a>

### Cambios de cola y estado

Los cambios de estructura SHM requieren un reinicio completo. Se conservan la sintaxis método/ACL y los manejadores de protocolo. v4 usa JSON por defecto, códigos coincidentes en cabecera y cuerpo y `200 no_match` cuando falta la entrada. `cache_purge_legacy_status` solo genera una advertencia. Actualice los clientes según el [contrato HTTP](../HTTP_RESPONSES.md). La capacidad incluye tareas asignadas; la sobrecarga espera admisión. Los duplicados activos pueden programar otra pasada.

Estado añade campos sin retirar nombres existentes. `requests.overflow` cuenta ahora esperas de admisión. Métricas aproximadas usan acumuladores de 64 bits. Con hilos, referenciar `default` lo registra automáticamente; use un pool con nombre para aislar E/S. Solo configurar estado ya asigna SHM.

Generaciones y REFRESH requieren activación explícita en v4.0. Vincule `cache_purge_key` y use `$cache_purge_key` como clave nativa completa en todos los locations participantes. Cambian los nombres físicos: archivos anteriores quedan fríos hasta expiración nativa o limpieza heredada explícita. Conserve marcadores con la caché. Locations PURGE existentes no obtienen REFRESH. Usuarios de PR60 experimental deben configurar endpoint/perfil y paginar; respuesta ilimitada, deducción URI/Host y ampliación implícita de métodos de PR60 no son garantías de compatibilidad.

<a id="from-the-original-frickle-module"></a>

### Desde el módulo FRiCKLE original

Se conservan las formas método/lista de acceso y zona/clave. Las entradas ausentes devuelven `200 no_match`; elimine `cache_purge_legacy_status` y actualice los clientes. Para activar el procesamiento en segundo plano, añada:

```nginx
http {
    cache_purge_background_queue on;
}
```

<a id="from-nginx-selective-cache-purge-module"></a>

### Desde nginx-selective-cache-purge-module

Sustituya el módulo: la cola está integrada. Traslade los parámetros anteriores a `cache_purge_queue_size`, `cache_purge_batch_size` y `cache_purge_throttle_ms`.

---

<a id="temporary-directories"></a>

## Directorios temporales

Los recorridos omiten temporales configurados bajo la raíz mediante comparación de componentes completos. Nombres como `proxy_temp` no se reservan sin configuración correspondiente. Solo se borran candidatos regulares cuyo nombre tenga exactamente 32 caracteres hexadecimales. Temporales numéricos o con sufijo se omiten, también con `use_temp_path=off`.

Cada recorrido valida versión nativa, offsets, marcador `KEY: ` completo, CRC32 y hash de clave/variante del nombre, con límite fijo de clave de 8192 bytes. Se omiten archivos malformados, truncados, de otra versión o incoherentes incluso en purge-all. Se aceptan longitudes ETag/Vary iguales al máximo nativo. Validar formato no autentica archivos maliciosos.

La raíz configurada puede ser symlink o junction Windows. Se omiten symlinks/reparse points descendientes. En POSIX con detección completa de funciones relativas a descriptor, hijos usan `ngx_openat_file`/`ngx_file_at_info` nativos con no-follow; se borra respecto al padre abierto. Evita redirección por sustituir un symlink descendiente, pero no protege contra mover un directorio ya abierto. Windows y Unix antiguos usan rutas completas y comprobaciones de enlaces, conservando carrera de sustitución. Búsqueda/borrado exacto heredado también usa rutas completas. Mantenga raíces privadas sin escritores no confiables.

Escriba rutas canónicas consistentemente: la comparación no resuelve alias de symlink, montaje ni Unicode. Las tareas conservan su protección; un duplicado nuevo actualiza pendientes o próxima repetición. La pasada activa conserva la anterior; no es la unión de generaciones vivas de workers. No cambie el uso de rutas con tareas afectadas pendientes o activas.

---

<a id="security"></a>

## Seguridad

- Restrinja endpoints de purga con `allow` / `deny` o autenticación previa. No los exponga públicamente.
- Ajuste `cache_purge_queue_size` al tráfico para limitar SHM ante inundaciones de peticiones.
- Considere `limit_req` para limitar purgas independientemente del tráfico normal.

---

<a id="license"></a>

## Licencia

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

---

<a id="see-also"></a>

## Véase también

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [Documentación de nginx](https://nginx.org/en/docs/)
