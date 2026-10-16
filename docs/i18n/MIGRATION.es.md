# ngx_cache_purge

[README](README.es.md) · [Directivas](DIRECTIVES.es.md)

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
