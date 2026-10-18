# ngx_cache_purge

[README](README.es.md) · [Directivas](DIRECTIVES.es.md)

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

<a id="reading-cache-files"></a>

### Lectura de archivos de caché en modo de generaciones

Un explorador de caché, precargador o informe de uso no puede deducir la validez de una generación solo de los archivos: tras una invalidación pueden quedar archivos obsoletos sin sustituto. La línea `KEY:` de un archivo de generación es `p4:<namespace>:<epoch>:<generation>:<logical key>`. El [endpoint de estado](DIRECTIVES.es.md#cache_purge_queue_status) expone el estado del espacio de nombres. La generación vigente de una clave es `baseline`, salvo que una regla exacta o de prefijo la haya elevado; la respuesta no incluye esas reglas.

| Estado del espacio de nombres del archivo | Veredicto |
|---|---|
| El espacio de nombres no aparece en la lista | `unknown` |
| La época del archivo difiere de `epoch` | `stale` |
| La época coincide; la generación del archivo es inferior a `baseline` | `stale` |
| La época coincide; la generación es al menos `baseline`; `rules` es `false` | `live` |
| La época coincide; la generación es al menos `baseline`; `rules` es `true` | `unknown` |

Aquí, **live** solo significa que el archivo no está obsoleto según la generación del espacio de nombres; no confirma la frescura del contenido, la validez del TTL ni un HIT de caché de NGINX. Con `namespaces_complete=false`, las entradas devueltas siguen siendo utilizables y los espacios omitidos son desconocidos.

Un archivo de generación superior con el mismo espacio de nombres, época y clave lógica demuestra que el de generación inferior está obsoleto: la generación vigente nunca disminuye dentro de una época. En los demás casos, mantenga el veredicto unknown.

Lea el estado antes y después del recorrido, emparejando entradas por nombre del espacio de nombres. Use los veredictos basados en el estado solo si el espacio aparece en ambas respuestas y `epoch`, `baseline` y `serial` no cambian. Si falta o cambia el estado, descarte los veredictos de ese espacio. Compare las épocas solo por igualdad. Estas observaciones no impiden invalidaciones posteriores.

Una cola de limpieza vacía no demuestra validez. Una petición iniciada antes de la invalidación puede escribir un archivo de la generación antigua después de terminar la limpieza. El archivo permanece hasta otra limpieza o su expulsión por el caché nativo. Además, la limpieza solo afecta a la ruta de caché de la location de PURGE, mientras que un espacio de nombres puede abarcar varias rutas.

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

El [endpoint de estado](DIRECTIVES.es.md#cache_purge_queue_status) muestra pendientes, activos, esperas, duplicados, resultados de borrado y HIT/MISS. `202 Accepted` confirma admisión/agrupación, no borrado ni invalidación lógica inmediata. Observe latencia junto con edad de cola y contención del almacenamiento.

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

<a id="temporary-directories"></a>

## Directorios temporales

Los recorridos omiten temporales configurados bajo la raíz mediante comparación de componentes completos. Nombres como `proxy_temp` no se reservan sin configuración correspondiente. Solo se borran candidatos regulares cuyo nombre tenga exactamente 32 caracteres hexadecimales. Temporales numéricos o con sufijo se omiten, también con `use_temp_path=off`.

Cada recorrido valida versión nativa, offsets, marcador `KEY: ` completo, CRC32 y hash de clave/variante del nombre, con límite fijo de clave de 8192 bytes. Se omiten archivos malformados, truncados, de otra versión o incoherentes incluso en purge-all. Se aceptan longitudes ETag/Vary iguales al máximo nativo. Validar formato no autentica archivos maliciosos.

La raíz configurada puede ser symlink o junction Windows. Se omiten symlinks/reparse points descendientes. En POSIX con detección completa de funciones relativas a descriptor, hijos usan `ngx_openat_file`/`ngx_file_at_info` nativos con no-follow; se borra respecto al padre abierto. Evita redirección por sustituir un symlink descendiente, pero no protege contra mover un directorio ya abierto. Windows y Unix antiguos usan rutas completas y comprobaciones de enlaces, conservando carrera de sustitución. Búsqueda/borrado exacto heredado también usa rutas completas. Mantenga raíces privadas sin escritores no confiables.

Escriba rutas canónicas consistentemente: la comparación no resuelve alias de symlink, montaje ni Unicode. Las tareas conservan su protección; un duplicado nuevo actualiza pendientes o próxima repetición. La pasada activa conserva la anterior; no es la unión de generaciones vivas de workers. No cambie el uso de rutas con tareas afectadas pendientes o activas.

---

<a id="testing"></a>

## Pruebas

El [mapa de cobertura](../TEST_COVERAGE.md) (en inglés) distingue los casos probados de los pendientes.

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

Consulte [`t/TESTING.md`](../../t/TESTING.md) para compilaciones estrictas, sanitizadores, Valgrind, plataformas e informes. Todos los trabajos GitHub están definidos en `build-test-release.yml`.
