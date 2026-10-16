[ngx_cache_purge](README.pt-BR.md)

<a id="directives"></a>

# Diretivas

<a id="proxy_cache_purge"></a>

## `proxy_cache_purge`

```
Syntax:  proxy_cache_purge on | off;
         proxy_cache_purge <method> [purge_all] from all | <cidr> ...;
         proxy_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

**Forma no próprio location** (`from …`) — intercepta o método HTTP indicado no location proxy e remove a entrada correspondente. `on` equivale ao método `PURGE`; `off` desativa a limpeza. Restrinja por CIDR ou permita qualquer endereço com `from all`. `purge_all` antes de `from` esvazia a zona inteira independentemente da URI.

**Forma com location separado** (zona e chave) — permitida apenas no location de limpeza, geralmente com captura da chave por regex. Localiza a zona pelo nome e limpa conforme a expressão de chave compilada. É incompatível com `proxy_cache` e `proxy_pass` no mesmo location.

<a id="fastcgi_cache_purge"></a>

## `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Equivale a `proxy_cache_purge` para zonas FastCGI configuradas com `fastcgi_cache` / `fastcgi_cache_path`.

<a id="scgi_cache_purge"></a>

## `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Equivale a `proxy_cache_purge` para zonas SCGI configuradas com `scgi_cache` / `scgi_cache_path`.

<a id="uwsgi_cache_purge"></a>

## `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Equivale a `proxy_cache_purge` para zonas uWSGI configuradas com `uwsgi_cache` / `uwsgi_cache_path`.

<a id="cache_purge_response_type"></a>

## `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

Seleciona o formato das respostas PURGE bem-sucedidas; JSON é o padrão. `Cache-Purge-Code` corresponde ao código no corpo. As respostas do módulo incluem `Cache-Control: no-store`; erros usam `application/problem+json` conforme a RFC 9457. Chaves textuais recebem escape; chaves binárias usam Base64 com indicação da codificação.

`none` suprime o corpo PURGE: sucesso concluído retorna `204`, trabalho aceito em segundo plano mantém `202` e erros mantêm 4xx/5xx. Status e REFRESH continuam com JSON estruturado. Consulte o [contrato HTTP](../HTTP_RESPONSES.md) para códigos, campos e limites.

<a id="cache_purge_background_queue"></a>

## `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

Ativa processamento adiado de curingas e `purge_all`. Retorna `202 Accepted` após enfileirar ou agregar a uma tarefa idêntica pendente ou ativa. Não altera limpezas de chave exata.

Controla o comportamento legado de curinga/cache inteiro. Uma vinculação `cache_purge_key` sempre usa limpeza física adiada e resposta lógica `200`, mesmo com essa opção desativada.

Fila cheia ou falha de alocação deixa a requisição aberta até conseguir admissão, sem percurso durante a espera. Duplicatas pendentes/ativas podem ser aceitas com a fila cheia. Falha ao alocar o pequeno estado de espera retorna `500`. A limpeza na desconexão cancela o temporizador e libera o contador de espera.

Para esta opção e `cache_purge_vary_aware`, prevalece o último valor declarado. Valores inválidos são rejeitados. `cache_purge_legacy_status` está obsoleto e é ignorado. Threads e ciclo de vida: [modelo de execução](../ARCHITECTURE.md#execution-model).

<a id="cache_purge_queue_size"></a>

## `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

Máximo de tarefas não concluídas entre todos os caminhos. Pendentes e atribuídas consomem capacidade. Duplicatas compartilham uma alocação; uma repetição mantém a alocação e a vaga. Tarefas distintas aguardam uma vaga livre.

A zona compartilhada é criada ao configurar processamento em segundo plano ou status. Alterar capacidade exige reinício mesmo quando o arredondamento produz o mesmo tamanho. Reload rejeita redimensionamento para preservar trabalho aceito. Adicionar raízes também pode exigir mais memória e reinício. Drene antes as tarefas pendentes/ativas cuja conclusão é necessária.

Declarar um namespace também cria a fila.

O tamanho da zona inclui tarefas, metadados de caminhos e métricas alinhadas dos workers. Depende da compilação e do número de caminhos; veja [dimensionamento de memória](../OPERATIONS.md#memory-sizing).

<a id="cache_purge_thread_pool"></a>

## `cache_purge_thread_pool`

```
Syntax:  cache_purge_thread_pool <name>;
Default: default
Context: http, server, location
```

Seleciona o executor das tarefas aceitas em segundo plano, com herança normal. Exemplo:

```nginx
thread_pool purge_io threads=2 max_queue=128;
http {
    cache_purge_background_queue on;
    cache_purge_thread_pool purge_io;
}
```

NGINX valida os pools nomeados e entrega a conclusão pelo evento nativo da tarefa; não se copiam layouts privados nem filas de conclusão. Com threads, pools personalizados devem existir. Sem threads ou no Windows, a diretiva é aceita e usa etapas limitadas do loop. Pool cheio adia a execução, sem devolver I/O ao loop. Pools dedicados acrescentam pilhas de threads e trocas de contexto.

<a id="cache_purge_batch_size"></a>

## `cache_purge_batch_size`

```
Syntax:  cache_purge_batch_size <number>;
Default: 10
Minimum: 1
Maximum: 64
Context: http
```

Máximo de tarefas por worker entre pausas. O progresso é preservado entre etapas e conclusões de threads. Fila vazia encerra o lote. Valores acima de 64 são limitados com aviso de configuração; zero é rejeitado.

Controla tarefas, não arquivos nem duração do callback. Valores menores acrescentam pausas entre tarefas, mas não interrompem um percurso ativo em thread. Combine com `cache_purge_throttle_ms` ao ajustar a carga de disco.

<a id="cache_purge_throttle_ms"></a>

## `cache_purge_throttle_ms`

```
Syntax:  cache_purge_throttle_ms <time>;
Default: 10ms
Minimum: 1ms
Context: http
```

Pausa entre lotes. Aceita `10ms`, `500ms`, `1s` e valores compostos entre aspas, como `"2s 500ms"`. Inteiros sem unidade são milissegundos: `10` equivale a `10ms`. Sem unidade há aviso de configuração; uma unidade explícita o evita.

Um worker ocioso verifica novamente após dez vezes esse intervalo. Etapas incompletas e mutex ocupado usam temporizadores de 1 ms. São intervalos mínimos de agendamento, não prazos garantidos. Zero e valores além do intervalo seguro de temporizadores da plataforma são rejeitados.

Admissão, pressão no pool e novas tentativas adiadas usam polling de 10 ms.

<a id="cache_purge_queue_status"></a>

## `cache_purge_queue_status`

```
Syntax:  cache_purge_queue_status <cache_path>;
Default: not configured
Context: location
```

Fornece snapshot JSON GET/HEAD com `Cache-Control: no-store`. O caminho deve identificar uma raiz configurada; caminhos desconhecidos impedem carregar a configuração, mesmo com execução em segundo plano desativada. Restrinja o acesso:

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

`queue` descreve a fila compartilhada de todos os caminhos:

| Campo | Tipo / unidade | Significado |
|---|---|---|
| `size` | Inteiro / tarefas | Tarefas aguardando worker |
| `running` | Inteiro / tarefas | Tarefas atribuídas, inclusive aguardando pool |
| `max_size` | Inteiro / tarefas | Capacidade conjunta de pendentes e atribuídas |
| `wait_time` | Inteiro / milissegundos | Idade da pendência mais antiga; zero se vazia |
| `full` | Booleano | A amostra de pendentes mais atribuídas atingiu a capacidade |

`cache` descreve a raiz selecionada pelo location:

| Campo | Tipo / unidade | Significado |
|---|---|---|
| `pending.total` | Inteiro / tarefas | Pendências deste cache |
| `pending.full` | Inteiro / tarefas | Tarefas `purge_all` pendentes, incluídas em `pending.total` |
| `active` | Inteiro / tarefas | Atribuídas até concluir a contabilização |
| `requests.overflow` | Inteiro / requisições | Requisições que esperaram admissão, inclusive por bloqueio/alocação |
| `requests.deduplicated` | Inteiro / requisições | Requisições agregadas a trabalho pendente ou ativo |
| `requests.waiting` | Inteiro / requisições | Esperas atuais de admissão para este cache |
| `files.deleted` | Inteiro / arquivos | Remoções em segundo plano bem-sucedidas informadas ao fim da passagem |
| `files.errors` | Inteiro / erros | Falhas de remoção, exceto arquivo já ausente |
| `files.bytes` | Inteiro / bytes | Tamanho lógico estimado dos arquivos removidos |
| `responses.hit`, `responses.miss` | Inteiro / requisições principais | Decisões HIT/MISS do núcleo observadas na fase de log |
| `responses.hit_ratio` | Número / 0..1 | HIT / (HIT + MISS), ou zero sem observações |
| `logical.invalidations` | Inteiro / decisões | Invalidações de geração publicadas, inclusive duplicatas e REFRESH |
| `cleanup.scan_errors` | Inteiro / erros | Falhas de metadados/abertura/leitura/fechamento/alocação em segundo plano |
| `cleanup.retries` | Inteiro / passagens | Passagens adicionais agendadas após falha de limpeza |
| `cleanup.exhausted` | Inteiro / tarefas | Tarefas retiradas após três novas tentativas malsucedidas |

`pending.full` conta operações de cache inteiro; um curinga cuja chave é `*` constitui outro tipo. `active` inclui espera por pool, excluída de `queue.size` e `pending.total`, mas que ocupa capacidade de admissão.

Em varreduras agregadas de namespace, `pending.full` reflete a classificação da primeira tarefa aceita. Scans/probes REFRESH não entram na profundidade da fila PURGE nem nos erros de remoção. Reposicionar uma tentativa adiada pode fazer `wait_time` refletir a idade da cabeça amostrada, não a maior idade pendente.

Cada slot de processo tem um escritor no loop de eventos; início e espaçamento são alinhados à linha de cache. Threads retornam resultados em eventos de conclusão. O leitor de status não usa mutex da fila nem atomic fetch-add, mesmo com incremento zero. Em 64 bits, usa leituras/escritas volatile alinhadas comuns; em 32 bits, verificações de sequência limitadas protegem leituras de 64 bits, podendo omitir um segmento ocupado da amostra. Os campos são aproximados, não uma transação entre workers. Admissão usa estado de controle bloqueado, nunca métricas.

Contadores acumulados saturam no máximo unsigned de 64 bits, persistem em reloads e zeram no reinício. O contador de espera acompanha a limpeza de requisições. Operações síncronas e remoções pelo cache manager não entram nos totais de segundo plano. Bytes estimam comprimentos, não blocos nem espaço efetivamente liberado; leitores abertos e gravações concorrentes alteram o resultado. Resultados parciais contam; resultados de workers que falharam podem faltar. Não são logs de auditoria.

HIT/MISS conta apenas requisições principais com decisão nativa de cache. Ausência de upstream, status zero, BYPASS, EXPIRED, STALE, UPDATING e REVALIDATED são excluídos da razão. Requisições normalmente não cacheadas não viram MISS. Só o monitoramento pode criar SHM com limpeza em segundo plano desligada. Em 32 bits, a idade da fila cobre menos de aproximadamente 24,9 dias; diferença de relógio com sinal negativa vira zero.

<a id="cache_purge_legacy_status"></a>

## `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

Diretiva obsoleta, aceita apenas para migração da configuração. Ambos os valores geram um aviso e são ignorados; remova a diretiva. Valores inválidos são rejeitados. Ausência de correspondência síncrona retorna `200 no_match`; limpeza física aceita, `202 accepted`; invalidação de geração publicada, `200 invalidated`. A alternância entre `412` e `404` foi removida. Consulte o [contrato HTTP](../HTTP_RESPONSES.md).

<a id="cache_purge_vary_aware"></a>

## `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

Com `on`, após remover o arquivo principal numa limpeza exata bem-sucedida, percorre o cache e remove arquivos restantes com a mesma string `KEY:`. Inclui variantes `Vary` e `gzip_vary` em caminhos distintos com uma chave lógica comum. Verifica `\n` imediatamente após a chave para impedir correspondências falsas de prefixo.

Desativado por padrão porque cada purge exato acrescenta uma varredura completa. Ative para múltiplas representações `Vary`/`gzip_vary`. A varredura também ocorre quando a entrada principal já está ausente. Curingas e `purge_all` já abrangem variantes. Chaves Vary exatas diferenciam maiúsculas e são limitadas a 511 bytes; chaves maiores retornam `400 invalid_selector` antes de qualquer exclusão.

A invalidação por gerações cobre todas as representações da chave lógica sem esta opção.

<a id="cache_purge_namespace"></a>

## `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

Declara um namespace de gerações antes das vinculações em locations. Nomes contêm 1–32 letras ASCII, dígitos, `_` ou `-`; SHM exige pelo menos oito páginas nativas. Use um diretório de estado persistente exclusivo, fora das raízes de cache e temporários. O núcleo cria um marcador persistente exclusivo para reservar uma época de reinício. Preserve esses pequenos arquivos enquanto o cache correspondente puder existir: apagar o estado e manter o cache permite reutilizar épocas obsoletas. Não há garantia de durabilidade contra perda de energia/fsync. Reload normal preserva época e regras; mudar tamanho/caminho ou remover namespace exige reinício completo. Namespace e fila têm tamanhos SHM independentes.

<a id="cache_purge_key"></a>

## `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

Vincula o location a um namespace existente e expõe `$cache_purge_key`. Use essa variável como chave inteira em `proxy_cache_key`, `fastcgi_cache_key`, `scgi_cache_key` ou `uwsgi_cache_key`, inclusive no location PURGE separado correspondente. Se a chave nativa expandida divergir da gerada, PURGE retorna `500` em vez de afirmar invalidação estrita. Não referencie `$cache_purge_key` em sua própria expressão lógica. O limite lógico é 8096 bytes. Vincule somente locations participantes quando os demais precisarem da semântica legada; não existe forma `off` para cancelar uma vinculação herdada.

<a id="proxy_cache_refresh"></a>

## `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

Cria endpoint REFRESH explícito, exclusivo de requisições principais; outros métodos retornam `405`. Exige vinculação `cache_purge_key`, `$cache_purge_key` como expressão nativa e perfil refresh. A chave lógica expandida seleciona chave exata, prefixo com `*` final ou `*` para o namespace nesse caminho. Proteja o location com acesso/autenticação do núcleo. REFRESH suporta apenas caches proxy; PURGE continua disponível nos quatro protocolos.

<a id="cache_purge_refresh_profile"></a>

## `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

Mapeia uma representação conhecida para um location proxy interno literal. Remover o prefixo exato e sensível a maiúsculas/minúsculas da chave deve deixar URI de origem iniciada por `/`, preservando query string e escapes. O módulo não adivinha host nem procura `://` na chave. O destino exige `internal`, cache desligado, encaminhamento de cabeçalhos/corpo desligado, método HEAD, Host literal não vazio e cabeçalhos literais de representação. Os dois cabeçalhos condicionais devem usar exatamente `$cache_purge_refresh_etag` e `$cache_purge_refresh_last_modified`; `$cache_purge_refresh_uri` fornece a URI para `proxy_pass`. Essas três variáveis puras só fazem sentido nos probes. O destino é validado em preaccess, inclusive após redirecionamentos internos. O administrador deve garantir a mesma representação de origem identificada pela chave; entradas Vary são ignoradas porque os metadados nativos não guardam os cabeçalhos originais.

Defina `proxy_intercept_errors off` no location do probe. Interceptação herdada é rejeitada para impedir que um erro de origem se torne sucesso invalidante.

<a id="cache_purge_refresh_timeout"></a>

## `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

Limita o despacho de novos scans/probes da página REFRESH, incluindo espera pelo scanner do worker. Ao expirar, para novos trabalhos e marca a resposta incompleta. Um HEAD ativo termina segundo `proxy_connect_timeout`, `proxy_send_timeout` e `proxy_read_timeout` do núcleo: configure-os explicitamente. Não é prazo rígido de resposta nem interrompe chamadas de sistema. Desconexão e encerramento gracioso impedem novos despachos, preservando memória de threads ativas até a conclusão nativa.
