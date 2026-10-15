<a id="ngx_cache_purge"></a>

# ngx_cache_purge

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

Módulo NGINX para remover entradas dos caches `FastCGI`, `proxy`, `SCGI` e `uWSGI`. Na v4, as gerações de chaves impedem que novas requisições usem entradas antigas antes da exclusão dos arquivos. REFRESH consulta o servidor de origem para verificar se o conteúdo do cache proxy mudou. Os dois recursos exigem configuração explícita. A sintaxe PURGE continua disponível, mas as respostas HTTP mudaram: veja a [migração](#migration).

---

<a id="contents"></a>

## Sumário

- [Recursos](#features)
- [Escolha o comportamento necessário](#choosing-a-mode)
- [Compatibilidade](#compatibility)
- [Instalação](#installation)
- [Limpeza por chave parcial](#partial-key-purge)
- [Invalidação por gerações](#generation-invalidation)
- [Verificar o cache na origem (REFRESH)](#conditional-refresh)
- [Exemplos de configuração](#sample-configurations)
- [Diretivas](#directives)
- [Modelo de execução](#execution-model)
- [Ajuste de desempenho](#performance-tuning)
- [Monitoramento e depuração](#monitoring-and-debugging)
- [Solução de problemas](#troubleshooting)
- [Testes](#testing)
- [Migração](#migration)
- [Diretórios temporários](#temporary-directories)
- [Segurança](#security)
- [Licença](#license)

---

<a id="features"></a>

## Recursos

- **Limpeza no próprio location** — método HTTP dedicado (`PURGE`) com controle de acesso por IP
- **Location separado** — sintaxe zona/chave `proxy_cache_purge zone key` para chaves capturadas por expressão regular, sem `proxy_pass`
- **Curinga / limpeza parcial** — `*` final percorre o diretório do cache e remove as entradas correspondentes
- **`purge_all`** — remove todas as entradas da zona em uma requisição
- **Fila em segundo plano** — percurso adiado em etapas limitadas do loop de eventos ou em pool de threads NGINX opcional; lotes e pausas configuráveis
- **Suporte a Vary** — após a limpeza exata, remove as variantes em disco (gzip, cabeçalho Vary) com a mesma chave
- **Formatos de resposta** — `json` (padrão), `html`, `xml`, `text`, `none`
- **Pools nomeados** — seleção herdada, com execução incremental sem threads e no Windows
- **Deduplicação de tarefas ativas** — uma operação indexada e no máximo uma repetição pendente para limpezas recebidas durante o percurso
- **Status** — métricas de fila, remoção e HIT/MISS por cache, sem adquirir o mutex compartilhado da fila na leitura
- **Chaves de geração** — invalidação exata, por prefixo ou namespace sem I/O de disco vinculado à requisição; gravações antigas tardias permanecem inacessíveis
- [Verificar o cache na origem (REFRESH)](#conditional-refresh)

---

<a id="choosing-a-mode"></a>

## Escolha o comportamento necessário

| Objetivo | Configuração | Significado da resposta |
|---|---|---|
| Excluir entradas desatualizadas e aguardar a tentativa de exclusão | PURGE sem fila em segundo plano | `200 purged` ou `200 no_match`; 4xx/5xx em caso de erro |
| Agendar a limpeza por prefixo ou de todo o cache | PURGE com `cache_purge_background_queue on` | `202 accepted`; entradas antigas ainda podem ser servidas enquanto aguardam |
| Impedir que novas requisições usem conteúdo antigo, inclusive após gravações tardias | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`; nova geração de chave, exclusão dos arquivos depois |
| Verificar alterações na origem | Gerações + `proxy_cache_refresh` + perfil interno | `200` com resultados por entrada e continuação; não preenche o cache |

Os mecanismos se combinam: a troca de geração também agenda a limpeza. A fila sozinha não protege contra gravações tardias. Veja o [exemplo v4](#generation-and-refresh-example), os [exemplos PURGE](#sample-configurations) e as [respostas HTTP](../HTTP_RESPONSES.md) (em inglês).

---

<a id="compatibility"></a>

## Compatibilidade

A matriz lista os alvos configurados na CI. Um job bem-sucedido valida aquela configuração de código e compilação, não todas as versões da série.

| NGINX | CI: push / PR | CI: + full |
|---|---|---|
| 1.30.5, 1.31.6 | `test-matrix` | — |
| 1.31.6 | `build-compatibility` (GCC/Clang), `sanitizer`, `native-macos`, `native-windows` (x64/x86) | `memory-leak-test` (Valgrind) |
| 1.7.9 | `build-compatibility` (GCC) | — |
| 1.7.11, 1.7.12 | `build-compatibility` (GCC) | — |
| 1.20.2, 1.26.3, 1.28.3 | — | `test-matrix` |
| 1.11.4, 1.11.5 | — | `build-compatibility` (GCC) |

Os jobs Linux usam as mesmas imagens Docker e ponto de entrada dos testes locais. Os jobs nativos macOS e Windows exercitam suas próprias APIs de arquivos e processos; Windows compila com MSVC e verifica HTTP, caminhos e reload em `nginx.exe`. Os cenários POSIX de processos/sinais rodam em Linux e macOS, mas não no Windows, que também não executa o caminho opcional de pool de threads NGINX.

Sem `--with-threads`, a varredura é incremental. Com epoll anterior ao NGINX 1.7.12, esse modo também evita uma condição de corrida na notificação do núcleo. BSD não tem CI nativo. Execuções normais verificam stable/mainline e limites de API; as estendidas acrescentam versões anteriores e Valgrind. Alterações apenas na documentação não executam compilações. Agenda e comandos: [testes](../../t/TESTING.md).

O código está dividido em configuração, manipuladores, fila, varredura, invalidação, REFRESH e respostas. Funções comuns de caminhos ficam em `ngx_cache_purge.h`; operações de plataforma, nos cabeçalhos POSIX/Win32. `generation.*` passa a ser `invalidation.*`; geração continua indicando a versão das chaves. Veja a [arquitetura](../ARCHITECTURE.md).

O módulo segue o estilo NGINX compatível com C89 e usa as APIs de eventos, slab, rbtree e operações atômicas do núcleo. Não exige atomics C11 nem uma API de arquivos mais recente. Compilações GNU89 rigorosas verificam a ordem das declarações e avisos; compilador e plataforma efetivamente testados acompanham os resultados.

---

<a id="installation"></a>

## Instalação

```bash
cd /path/to/nginx-source
./configure --add-module=/path/to/ngx_cache_purge
make
make install
```

Módulo dinâmico:

```bash
./configure --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

---

<a id="partial-key-purge"></a>

## Limpeza por chave parcial

Adicione `*` à **chave de cache avaliada** para buscar um prefixo sem diferenciar maiúsculas. Chaves exatas e Vary continuam diferenciando maiúsculas. O prefixo de limpeza física aceita até 511 bytes, sem o `*` final. Um prefixo maior gera um único registro no log e retorna `400 invalid_selector` sem percorrer arquivos.

Com `cache_purge_key`, a correspondência usa a expressão lógica e aceita até 8096 bytes, incluindo `*` final.

Para uma expressão terminada em `$uri`, esta requisição seleciona um prefixo de URI:

```http
PURGE /images/header*
```

O asterisco deve ser o último byte da chave avaliada, não apenas do caminho URI. Considere query strings e sufixos variáveis em `proxy_cache_key`. Uma chave formada apenas por `*` percorre toda entrada elegível.

---

<a id="generation-invalidation"></a>

## Invalidação por gerações

Uma chave lógica identifica um objeto. Um namespace reúne chaves com regras de invalidação compartilhadas. Uma geração é uma versão acrescentada à chave de cache do NGINX. Após PURGE, novas requisições usam outra versão. Um GET anterior pode terminar, mas sua gravação tardia não gera HIT para as novas requisições.

Num namespace vinculado, PURGE publica nova geração e agenda varredura deduplicada de gerações antigas antes de retornar `200` com status `invalidated`. A admissão não abre nem remove arquivos do cache. Requisições que avaliam a chave pela primeira vez após a publicação não atingem a geração anterior. Cada requisição fixa sua geração por namespace/chave lógica, inclusive em redirecionamentos internos; uma resposta iniciada pode terminar, mas sua gravação tardia não vira hit para novas requisições. Correspondências exatas distinguem maiúsculas/minúsculas; prefixos ignoram caixa ASCII. `purge_all` altera o namespace inteiro, incluindo outros caminhos que o compartilham deliberadamente. A limpeza física é agendada para o caminho solicitado; outros caminhos dependem da expiração nativa ou de PURGE posterior.

Aceitar invalidação não garante liberação imediata de disco. Uma gravação antiga tardia continua inacessível e será removida em limpeza posterior ou expiração por inatividade do núcleo. Sob pressão de SHM, regras colapsam numa nova base do namespace: chaves adicionais ficam frias, sem reaparecerem chaves invalidadas. Cada invalidação posterior recupera até 64 nós destacados da trie. PURGE repetido de chave popular não faz a árvore crescer indefinidamente. A complexidade depende do comprimento limitado da chave e do índice de tarefas, não do número de arquivos; não se garante tempo real O(1) nem ausência de starvation em sobrecarga. Admissão pode esperar. Use `proxy_cache_lock` e controles equivalentes contra rajadas de GET após a invalidação.

<a id="conditional-refresh"></a>

## Verificar o cache na origem (REFRESH)

Use REFRESH quando o objeto estiver no cache e você não souber se ele mudou na origem, por exemplo após importar um catálogo. Se já souber que está desatualizado, use PURGE. REFRESH envia HEAD sem baixar o corpo e usa o ETag ou a data de modificação armazenados como condições HTTP. O perfil é uma location proxy interna que define a origem, Host, cabeçalhos e o mapeamento da URI.

REFRESH verifica objetos ainda cacheados antes de expirar o TTL; não pré-aquece nem estende TTL em `304`. Complementa `proxy_cache_revalidate` e `proxy_cache_background_update`, que tratam tráfego normal de objetos expirados. Origem que ignora validadores e sempre retorna `200` transforma REFRESH em invalidação mais um HEAD. Apenas entradas atuais, válidas e em caminhos hash canônicos são candidatas. Uma substituição física na mesma geração pode ser invalidada conservadoramente; a comparação de gerações impede um HEAD antigo de invalidar uma geração PURGE posterior.

| Resultado bruto da origem | Ação / resultado |
|---|---|
| `304` | Manter; `kept` |
| `200`, `404`, `410` | Invalidação condicional de geração e limpeza adiada; `invalidated` |
| Outro status | Manter; `upstream_status` |
| Sem status / falha de transporte | Manter; `transport_error` |
| Metadados Vary | Sem HEAD; `skipped_vary` |
| Mapeamento ou perfil inválido | Manter; `skipped_profile` |
| Geração substituída nesse intervalo | Sem nova invalidação; `raced_replaced` |
| Falha interna de invalidação/alocação | Manter; `invalidation_error` |
| Trabalho não despachado/concluído antes de parar a página | `undispatched`; o cursor não o ultrapassa |

Cada worker mantém no máximo uma página REFRESH ativa e um probe; o limite compartilhado é 16 probes entre workers. Outras páginas esperam sem alocar candidatos. Cada página percorre o diretório com o cursor comum, retém até 32 candidatos e emite subrequisições HEAD nativas em série, limitando memória e subrequisições no pool da requisição principal. Requisições REFRESH idênticas não são deduplicadas; compartilham o limite de concorrência. Suas invalidações usam a fila PURGE deduplicada comum.

Comece sem cursor e envie `X-Cache-Purge-Cursor: <next_cursor>` ao mesmo endpoint, com seletor lógico/perfil idênticos, até `complete` ser verdadeiro. O cursor é o último nome de arquivo concluído de 32 dígitos hexadecimais; cabeçalhos inválidos ou repetidos retornam `400`. `scan_complete` indica enumeração bem-sucedida; `complete` também exige ausência de próxima página e conclusão das decisões selecionadas. `timed_out` indica o limite de tempo; uma falha na varredura retorna `500 scan_failed`. `undispatched` conta selecionados não concluídos; scan incompleto fornece apenas um limite inferior, nunca o total do cache. Se o scan não terminar, nenhum probe roda, os resultados ficam vazios e o cursor de entrada permanece. Os resultados expõem hashes de arquivos e status upstream brutos, não chaves ou credenciais.

Cada página repete a varredura para selecionar os próximos hashes: memória limitada em troca de I/O repetido, `O(files × pages)` para a operação completa. Paginação é fracamente consistente com inserções/remoções concorrentes; recomece se precisar de um snapshot posterior completo. Ajuste seletores e timeouts. Esta interface administrativa não serve para varrer caches ativos muito grandes em alta frequência.

<a id="generation-and-refresh-example"></a>

### Exemplo de gerações e REFRESH

Este trecho pertence a main/http; mantenha seu bloco `events`. Ajuste a origem e os caminhos. Sem `--with-threads`, remova `thread_pool` e `cache_purge_thread_pool` para usar a varredura incremental.

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
            add_header X-Cache-Status $upstream_cache_status always;
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

Primeiro solicite um objeto existente que possa ser armazenado no cache. Repita GET até aparecer `X-Cache-Status: HIT` e verifique esse objeto com REFRESH. Após `kept`, o próximo GET continua HIT; após `invalidated`, consulta a origem novamente.

```sh
curl -i 'http://127.0.0.1:8080/objects/item'
curl -i -X REFRESH -H 'X-Cache-Key: origin.test/objects/item' \
  'http://127.0.0.1:8080/admin/refresh'
```

Para um prefixo, comece sem cursor. Se `complete` for `false`, repita com o valor retornado em `X-Cache-Purge-Cursor`, substituindo `<next_cursor>`. Mantenha o seletor. Resolva erros HTTP antes de continuar.

```sh
curl -i -X REFRESH -H 'X-Cache-Key: origin.test/objects/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: origin.test/objects/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

Se o objeto já estiver desatualizado, use PURGE diretamente. Com as gerações deste exemplo, `200 invalidated` impede que novas requisições usem a geração antiga, mesmo que seus arquivos ainda existam.

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/objects/item'
```

Veja [HTTP e casos extremos](../REFRESH.md) (em inglês). Uma falha na varredura retorna `500 scan_failed`, não uma página com `scan_error: true`.

<a id="sample-configurations"></a>

## Exemplos de configuração

<a id="basic--inline-purge-method"></a>

### Básico — método de limpeza no próprio location

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

### Básico — location de limpeza separado

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

### Fila em segundo plano ativada

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

### Remover todas as entradas

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

### Respostas JSON com configuração por location

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

### Limpeza com suporte a Vary

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

Após limpar uma chave, todas as variantes gzip e Vary em caminhos diferentes também são removidas automaticamente.

---

<a id="directives"></a>

## Diretivas

<a id="proxy_cache_purge"></a>

### `proxy_cache_purge`

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

### `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Equivale a `proxy_cache_purge` para zonas FastCGI configuradas com `fastcgi_cache` / `fastcgi_cache_path`.

<a id="scgi_cache_purge"></a>

### `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Equivale a `proxy_cache_purge` para zonas SCGI configuradas com `scgi_cache` / `scgi_cache_path`.

<a id="uwsgi_cache_purge"></a>

### `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Equivale a `proxy_cache_purge` para zonas uWSGI configuradas com `uwsgi_cache` / `uwsgi_cache_path`.

<a id="cache_purge_response_type"></a>

### `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

Seleciona o formato das respostas PURGE bem-sucedidas; JSON é o padrão. `Cache-Purge-Code` corresponde ao código no corpo. As respostas do módulo incluem `Cache-Control: no-store`; erros usam `application/problem+json` conforme a RFC 9457. Chaves textuais recebem escape; chaves binárias usam Base64 com indicação da codificação.

`none` suprime o corpo PURGE: sucesso concluído retorna `204`, trabalho aceito em segundo plano mantém `202` e erros mantêm 4xx/5xx. Status e REFRESH continuam com JSON estruturado. Consulte o [contrato HTTP](../HTTP_RESPONSES.md) para códigos, campos e limites.

<a id="cache_purge_background_queue"></a>

### `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

Ativa processamento adiado de curingas e `purge_all`. Retorna `202 Accepted` após enfileirar ou agregar a uma tarefa idêntica pendente ou ativa. Não altera limpezas de chave exata.

Controla o comportamento legado de curinga/cache inteiro. Uma vinculação `cache_purge_key` sempre usa limpeza física adiada e resposta lógica `200`, mesmo com essa opção desativada.

Fila cheia ou falha de alocação deixa a requisição aberta até conseguir admissão, sem percurso durante a espera. Duplicatas pendentes/ativas podem ser aceitas com a fila cheia. Falha ao alocar o pequeno estado de espera retorna `500`. A limpeza na desconexão cancela o temporizador e libera o contador de espera.

Para esta opção e `cache_purge_vary_aware`, prevalece o último valor declarado. Valores inválidos são rejeitados. `cache_purge_legacy_status` está obsoleto e é ignorado. Threads e ciclo de vida: [modelo de execução](#execution-model).

<a id="cache_purge_queue_size"></a>

### `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

Máximo de tarefas não concluídas entre todos os caminhos. Pendentes e atribuídas consomem capacidade. Duplicatas compartilham uma alocação; uma repetição mantém a alocação e a vaga. Tarefas distintas aguardam uma vaga livre.

A zona compartilhada é criada ao configurar processamento em segundo plano ou status. Alterar capacidade exige reinício mesmo quando o arredondamento produz o mesmo tamanho. Reload rejeita redimensionamento para preservar trabalho aceito. Adicionar raízes também pode exigir mais memória e reinício. Drene antes as tarefas pendentes/ativas cuja conclusão é necessária.

Declarar um namespace também cria a fila.

O tamanho da zona inclui tarefas, metadados de caminhos e métricas alinhadas dos workers. Depende da compilação e do número de caminhos; veja [dimensionamento de memória](#memory-sizing).

<a id="cache_purge_thread_pool"></a>

### `cache_purge_thread_pool`

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

### `cache_purge_batch_size`

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

### `cache_purge_throttle_ms`

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

### `cache_purge_queue_status`

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

### `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

Diretiva obsoleta, aceita apenas para migração da configuração. Ambos os valores geram um aviso e são ignorados; remova a diretiva. Valores inválidos são rejeitados. Ausência de correspondência síncrona retorna `200 no_match`; limpeza física aceita, `202 accepted`; invalidação de geração publicada, `200 invalidated`. A alternância entre `412` e `404` foi removida. Consulte o [contrato HTTP](../HTTP_RESPONSES.md).

<a id="cache_purge_vary_aware"></a>

### `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

Com `on`, após remover o arquivo principal numa limpeza exata bem-sucedida, percorre o cache e remove arquivos restantes com a mesma string `KEY:`. Inclui variantes `Vary` e `gzip_vary` em caminhos distintos com uma chave lógica comum. Verifica `\n` imediatamente após a chave para impedir correspondências falsas de prefixo.

Desativado por padrão porque cada purge exato acrescenta uma varredura completa. Ative para múltiplas representações `Vary`/`gzip_vary`. A varredura também ocorre quando a entrada principal já está ausente. Curingas e `purge_all` já abrangem variantes. Chaves Vary exatas diferenciam maiúsculas e são limitadas a 511 bytes; chaves maiores retornam `400 invalid_selector` antes de qualquer exclusão.

A invalidação por gerações cobre todas as representações da chave lógica sem esta opção.

<a id="cache_purge_namespace"></a>

### `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

Declara um namespace de gerações antes das vinculações em locations. Nomes contêm 1–32 letras ASCII, dígitos, `_` ou `-`; SHM exige pelo menos oito páginas nativas. Use um diretório de estado persistente exclusivo, fora das raízes de cache e temporários. O núcleo cria um marcador persistente exclusivo para reservar uma época de reinício. Preserve esses pequenos arquivos enquanto o cache correspondente puder existir: apagar o estado e manter o cache permite reutilizar épocas obsoletas. Não há garantia de durabilidade contra perda de energia/fsync. Reload normal preserva época e regras; mudar tamanho/caminho ou remover namespace exige reinício completo. Namespace e fila têm tamanhos SHM independentes.

<a id="cache_purge_key"></a>

### `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

Vincula o location a um namespace existente e expõe `$cache_purge_key`. Use essa variável como chave inteira em `proxy_cache_key`, `fastcgi_cache_key`, `scgi_cache_key` ou `uwsgi_cache_key`, inclusive no location PURGE separado correspondente. Se a chave nativa expandida divergir da gerada, PURGE retorna `500` em vez de afirmar invalidação estrita. Não referencie `$cache_purge_key` em sua própria expressão lógica. O limite lógico é 8096 bytes. Vincule somente locations participantes quando os demais precisarem da semântica legada; não existe forma `off` para cancelar uma vinculação herdada.

<a id="proxy_cache_refresh"></a>

### `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

Cria endpoint REFRESH explícito, exclusivo de requisições principais; outros métodos retornam `405`. Exige vinculação `cache_purge_key`, `$cache_purge_key` como expressão nativa e perfil refresh. A chave lógica expandida seleciona chave exata, prefixo com `*` final ou `*` para o namespace nesse caminho. Proteja o location com acesso/autenticação do núcleo. REFRESH suporta apenas caches proxy; PURGE continua disponível nos quatro protocolos.

<a id="cache_purge_refresh_profile"></a>

### `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

Mapeia uma representação conhecida para um location proxy interno literal. Remover o prefixo exato e sensível a maiúsculas/minúsculas da chave deve deixar URI de origem iniciada por `/`, preservando query string e escapes. O módulo não adivinha host nem procura `://` na chave. O destino exige `internal`, cache desligado, encaminhamento de cabeçalhos/corpo desligado, método HEAD, Host literal não vazio e cabeçalhos literais de representação. Os dois cabeçalhos condicionais devem usar exatamente `$cache_purge_refresh_etag` e `$cache_purge_refresh_last_modified`; `$cache_purge_refresh_uri` fornece a URI para `proxy_pass`. Essas três variáveis puras só fazem sentido nos probes. O destino é validado em preaccess, inclusive após redirecionamentos internos. O administrador deve garantir a mesma representação de origem identificada pela chave; entradas Vary são ignoradas porque os metadados nativos não guardam os cabeçalhos originais.

Defina `proxy_intercept_errors off` no location do probe. Interceptação herdada é rejeitada para impedir que um erro de origem se torne sucesso invalidante.

<a id="cache_purge_refresh_timeout"></a>

### `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

Limita o despacho de novos scans/probes da página REFRESH, incluindo espera pelo scanner do worker. Ao expirar, para novos trabalhos e marca a resposta incompleta. Um HEAD ativo termina segundo `proxy_connect_timeout`, `proxy_send_timeout` e `proxy_read_timeout` do núcleo: configure-os explicitamente. Não é prazo rígido de resposta nem interrompe chamadas de sistema. Desconexão e encerramento gracioso impedem novos despachos, preservando memória de threads ativas até a conclusão nativa.

---

<a id="execution-model"></a>

## Modelo de execução

Uma **tarefa** é uma operação com curinga ou `purge_all`. Um **lote** é o número de tarefas processadas por worker antes de uma pausa. Uma **etapa** é um passo limitado do percurso incremental; uma tarefa pode exigir muitas etapas.

Com chaves de geração, uma varredura física por caminho de cache e namespace agrega todas as invalidações aceitas. Páginas REFRESH são operações HTTP separadas, não tarefas da fila de limpeza.

| Requisição e configuração | Percurso dos arquivos | Conclusão HTTP |
|---|---|---|
| Chave exata legada, com ou sem Vary | Worker da requisição; Vary acrescenta um percurso | Após a operação |
| Curinga ou `purge_all`, fila desativada | Worker da requisição, percurso síncrono | Após o percurso |
| Tarefa em segundo plano, pool selecionado | Uma tarefa do pool por worker de cada vez | `202` após admissão |
| Tarefa em segundo plano sem pool utilizável | Loop de eventos do worker, etapas incrementais | `202` após enfileirar |
| Capacidade/alocação esgotada | A requisição aguarda admissão, sem percorrer arquivos | `202` após admissão |
| PURGE por gerações, qualquer formato de chave | Varredura adiada do namespace; sem abrir/remover arquivos no atendimento | `200 invalidated` após publicação e admissão |
| Endpoint REFRESH | Varredura limitada e até 32 verificações HEAD em série | JSON `200` com campos de conclusão e continuação |

As linhas sem chaves de geração descrevem o comportamento legado. Uma requisição por gerações que aguarda capacidade retorna `200` após admissão, não `202`.

Cada etapa incremental para após no máximo 64 iterações de diretório ou ao observar 5 ms decorridos, incluindo entradas ignoradas e finais de diretórios. Esses limites não restringem a duração de uma chamada de sistema isolada.

Para executar I/O de arquivos em threads, compile com `--with-threads` e declare um pool do núcleo no contexto **main**, fora de `http`:

```nginx
thread_pool default threads=2;
```

`cache_purge_thread_pool <name>` seleciona um pool nomeado em `http`, `server` ou `location`. O padrão é `default`; se referenciado sem declaração, NGINX o cria com seus parâmetros padrão. Outros nomes exigem declaração com `thread_pool`. A seleção é herdada; a primeira requisição aceita escolhe o executor da tarefa agregada. Cada worker tem seus próprios pools. Se o pool estiver cheio, a submissão é repetida após 10 ms, preservando a vaga de admissão, sem iniciar um percurso síncrono. Sem threads, no Windows, em modo de processo único ou após um reload remover o pool de uma tarefa antiga, utiliza-se o cursor incremental.

REFRESH usa o pool selecionado para um scanner adicional por worker. Subrequisições HTTP nativas rodam no loop de eventos; a thread de I/O nunca acessa seu estado.

Fila e índice usam o mutex slab para alterações estruturais. Admissão e callbacks de segundo plano usam trylock e repetição por temporizadores. Tarefas pendentes **e em execução** permanecem na rbtree de strings do núcleo por caminho; chaves completas resolvem colisões de hash. O índice custa `O(log Q)` para `Q` tarefas não concluídas; a busca do caminho é linear no número de caminhos registrados. Listas `ngx_queue_t` acompanham trabalho pendente e ativo. A primeira duplicata ativa solicita repetição; as seguintes se agregam a ela. Após o percurso atual, a repetição vai ao fim da fila, evitando descartar silenciosamente um PURGE posterior. Novos PURGE contínuos podem exigir mais passagens. A vaga só é liberada na conclusão final. I/O e formatação de respostas ocorrem fora do bloqueio.

A rbtree deduplica apenas limpeza física. Regras de geração ficam em tries de bytes limitadas, numa zona SHM separada. A varredura estrita de namespace tem identidade de índice distinta da tarefa de chave legada; decisões REFRESH entram nesse mesmo caminho de limpeza.

Fila cheia ou falha de alocação mantém a requisição aguardando admissão com temporizador de 10 ms e tratamento de desconexão do núcleo. Requisições e handlers de limpeza permanecem no worker proprietário; SHM não contém ponteiros de requisição. A leitura global de conexões HTTP/2 e HTTP/3 não é desativada. Pressão na fila não produz 429/503; um PURGE curto já lido não garante TCP Zero Window. Requisições pendentes consomem memória e conexões: aplique timeouts e políticas de admissão do núcleo ao orçamento de conexões.

Tarefas aceitas preservam chave, nome do pool e proteção de caminhos temporários durante reloads normais. Uma duplicata mais nova atualiza a proteção da tarefa pendente ou da próxima repetição. Desativar novas admissões preserva o trabalho aceito. Um worker em encerramento gracioso termina a passagem atual e não recebe outra tarefa. Saída rápida devolve a tarefa incremental incompleta à fila, sem concluir o percurso no hook de saída. O encerramento do pool do núcleo controla a vida das tarefas já submetidas às threads.

Épocas de PID/slot identificam atribuições. A cada segundo, workers verificam até 64 registros ativos por passagem e reenfileiram tarefas cujo proprietário terminou. Isso recupera atribuições abandonadas, não implementa um journal transacional. Uma falha durante alterações da lista compartilhada ou slab pode corromper o estado e exigir reinício; remoções imediatamente anteriores podem faltar nas métricas. Trabalho pendente e repetições exigem worker ativo: drene antes de parar quando a conclusão for necessária. A fila não é durável.

Layouts internos da fila não constituem ABI de atualização. Ao trocar por uma compilação com layout diferente, faça reinício completo, drenando previamente o trabalho que precisa terminar. Reloads da mesma compilação reutilizam a zona.

A remoção é best effort. `202` confirma aceitação, não remoção concluída; um percurso concluído não é um snapshot atômico. Requisições concorrentes podem criar entradas durante ou depois dele; erros de arquivo são registrados. Percursos com curinga e de cache inteiro removem arquivos sem atualizar sincronamente cada nó nativo. Operações exatas e Vary invalidam os nós correspondentes.

Passagens com falha são repetidas após 100 ms, 1 s e 10 s, até três vezes. Tarefas adiadas conservam a vaga de admissão/deduplicação e permitem outras tarefas. Um novo PURGE reinicia essa janela. Esgotadas as tentativas, a tarefa é retirada com erro no log; consulte `cleanup.exhausted` e repita após corrigir o armazenamento. A invalidação por gerações continua eficaz mesmo se a limpeza física falhar. Arquivos ausentes são corridas esperadas; arquivos malformados são ignorados sem repetição infinita.

---

<a id="performance-tuning"></a>

## Ajuste de desempenho

Os controles têm funções distintas: `queue_size` limita trabalho não concluído, `batch_size` agrupa tarefas por worker e `throttle_ms` espaça os lotes. Meça latência, contenção de disco e idade da fila antes/depois. Mais capacidade não aumenta a vazão de drenagem.

São configurações iniciais, não resultados de desempenho de hardware. As colunas omitem o prefixo comum `cache_purge_`.

| Carga / orçamento de armazenamento | `queue_size` | `batch_size` | `throttle_ms` |
|---|---:|---:|---:|
| Host ou armazenamento limitado | 512 | 5 | 25ms |
| Ponto de partida geral | 2048 | 20 | 10ms |
| Armazenamento rápido com capacidade livre medida | 8192 | 50 | 5ms |
| Rajadas curtas com drenagem conservadora | 8192 | 5 | 50ms |

Exemplo da configuração geral:

```nginx
cache_purge_background_queue on;
cache_purge_queue_size        2048;
cache_purge_batch_size        20;
cache_purge_throttle_ms       10ms;
```

Fila maior absorve rajadas, mas chegada sustentada acima da capacidade de conclusão acaba enchendo-a. Estime a folga adicional por `max(0, arrival_rate - completion_rate) * burst_seconds` tarefas, usando taxas em tarefas/s após deduplicação. Reserve margem para durações desiguais e backlog existente. A espera acrescenta requisições/conexões abertas: inclua sua memória e FDs no orçamento quando o armazenamento satura.

<a id="memory-sizing"></a>

### Dimensionamento de memória

O tamanho configurado da zona é calculado a partir das estruturas nativas:

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

| Componente | Memória reservada ou usada |
|---|---|
| Reserva de tarefas | `Q * S`, incluindo arredondamento slab e nomes de chave/pool embutidos |
| Métricas de um caminho | `R * NGX_MAX_PROCESSES`, mais alinhamento; 192 KiB com passo de 192 bytes e 1024 slots |
| Controle da fila | `H`, incluindo proprietário de processo e vetores de épocas |
| Reserva de caminhos | `M`, incluindo nomes, raízes rbtree, alinhamento e vetores de métricas |
| Zona mínima | Oito páginas nativas |

No layout LP64 medido (`I=184`, `H=20576`, registro de caminho 104 bytes e de métricas 144), linha de cache de 64 bytes, páginas de 4 KiB e um caminho de 20 bytes, a fórmula resulta em:

| Capacidade | Bytes da zona | MiB da zona |
|---:|---:|---:|
| 512 | 1654784 | 1.58 |
| 2048 | 5373952 | 5.12 |
| 8192 | 20242432 | 19.30 |

A alocação efetiva da tarefa é `I + index_bytes + 1 + pool_name_bytes` antes do arredondamento slab. O índice embutido contém um byte de ação e a chave legada ou o nome do namespace da varredura; `purge_all` legado guarda só a marca. Capacidade limita pendentes e ativas juntas. Estruturas nativas, linha de cache, `NGX_MAX_PROCESSES` e caminhos determinam o resultado; Windows e 32 bits diferem. Registros de proteção de reload consomem SHM adicional. A alocação pode falhar antes do limite de tarefas.

Memória de processo é separada. Cada percurso ativo usa buffer fixo de chave, buffer de caminho reutilizável e frame fixo por diretório aberto. Frames guardam comprimentos de prefixo sem copiar ancestrais: armazenamento `O(depth + longest_path)`, independente da quantidade de arquivos. Streams de diretório têm buffers da plataforma. Requisições esperando mantêm pequeno contexto de temporizador/limpeza, sem cursor. Pilhas e pools do núcleo têm custos separados.

O namespace tem tamanho SHM próprio. Um scanner REFRESH usa buffer/contexto LP64 fixo de 283864 bytes (32 entradas), mais frames e até 32 subrequisições no pool HTTP. Concluir a página libera essa requisição; continuar inicia outra. Esperas adicionais não alocam vetores de candidatos.

<a id="throughput-and-latency"></a>

### Vazão e latência

Para um worker continuamente ocupado, uma aproximação útil é:

```text
B = tasks completed in a batch
T = throttle delay in milliseconds
D = elapsed batch processing time in milliseconds, including slice waits
rate_per_worker ~= 1000 * B / (D + T) tasks/s
```

Ignorar `D` produz o teto apenas de agendamento `1000 * B / T`: 1000 tarefas/s por worker nos padrões. Não é taxa de remoção de arquivos nem medição. Dez tarefas que levam 200 ms no total com pausa de 10 ms resultam em cerca de 47,6 tarefas/s. Workers compartilham armazenamento; multiplicar pela quantidade de workers não garante vazão. Worker ocioso pode esperar até `10 * T`, mais atraso do loop, para verificar novas tarefas.

Reduza o lote ou aumente a pausa para reduzir trabalho entre descansos. Para menor latência durante I/O, use o pool opcional. Nenhum ajuste limita uma chamada de sistema nem divide uma tarefa em thread em lotes.

---

<a id="monitoring-and-debugging"></a>

## Monitoramento e depuração

Use o [endpoint de status](#cache_purge_queue_status) para observar pendências, tarefas ativas, espera, duplicatas, remoções e HIT/MISS. `202 Accepted` confirma admissão/agregação, não remoção nem invalidação lógica imediata. Monitore latência junto com idade da fila e contenção de armazenamento.

| Nível | Condição |
|---|---|
| `warn` | Chave excessiva, pool removido ou parâmetro limitado |
| `error` | Acesso negado, falha de busca no cache/perfil REFRESH, tentativas de limpeza esgotadas; falha de pool do núcleo ou leitura Windows |
| `crit` | Falha de abertura/stat/remoção ou operação de diretório; leitura POSIX do núcleo |
| `alert` | Falha ao fechar arquivo |
| `emerg` | Configuração inválida, SHM incompatível, redimensionamento ou falha de alocação de metadados no reload |
| `debug` | Contexto adicional de requisição/percurso |

Diagnósticos usam funções nativas de log NGINX e começam com `ngx_cache_purge: `. NGINX fornece nível, timestamp, IDs de processo/thread, texto errno e contexto de requisição/configuração. Mensagens de arquivos indicam a operação real e o caminho entre aspas; falhas de namespace/REFRESH identificam objeto e motivo da rejeição. Use parser padrão do error log NGINX e filtre o prefixo.

Mensagens de depuração usam `NGX_LOG_DEBUG_HTTP` e exigem compilação `--with-debug` e `error_log /var/log/nginx/error.log debug;`.

Pressão na fila aparece nas métricas de espera, sem aviso por tentativa. Arquivo já desaparecido é corrida esperada, sem diagnóstico do módulo. Mensagens de auxiliares do núcleo — parser, alocador, I/O e pools — conservam o formato nativo; consulte o log completo ao diagnosticar falhas.

```bash
tail -f /var/log/nginx/error.log | grep 'ngx_cache_purge:'
```

Veja [verificações de log](../../t/TESTING.md#logging-contracts) para diagnósticos testados.

---

<a id="troubleshooting"></a>

## Solução de problemas

| Sintoma | Verificação ou ajuste |
|---|---|
| Latência do worker em percursos grandes | Ative a fila e o pool opcional para I/O. Operações exatas/Vary e sem fila continuam síncronas. |
| Mais requisições esperando admissão | Verifique chegada, idade da fila, pool e disco. Capacidade extra absorve rajadas; redimensionar exige reinício. |
| iowait alto | Reduza `cache_purge_batch_size`, aumente `cache_purge_throttle_ms` e limite PURGE de entrada. |
| Drenagem lenta | Meça duração das tarefas e contenção antes de aumentar lotes ou reduzir pausas. |
| O cliente espera os antigos `412`/`404` | Remova `cache_purge_legacy_status`; trate `200` com `Cache-Purge-Code: no_match`. |
| Variantes Vary restantes | Ative `cache_purge_vary_aware`; verifique resultado principal e limite de 511 bytes. |
| Trabalho aceito parece incompleto | Verifique tarefas ativas, recuperação e erros. `202` não confirma conclusão; suspeita de corrupção SHM exige reinício. |

---

<a id="testing"></a>

## Testes

O [mapa de cobertura](../TEST_COVERAGE.md) (em inglês) separa os casos testados das lacunas restantes.

Compile e execute a suíte completa em Docker:

```sh
make test-all
NGINX_VERSION=1.31.6 make test-all
```

A partir de `t/`, os serviços Compose existentes permanecem disponíveis:

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

Com o plugin Compose, substitua `docker-compose` por `docker compose`; os Makefiles detectam ambos. `nginx-test` e `make test-all` executam as sete suítes `t/*.t` pelo mesmo `prove` da CI. A imagem de integração inclui verificações nativas de auxiliares/layout e execução com pool. Compilações rigorosas sem threads exercitam o modo incremental. Cada teste com falha faz o comando falhar.

Veja [`t/TESTING.md`](../../t/TESTING.md) para compilações rigorosas, sanitizadores, Valgrind, jobs de plataforma e relatórios. Todos os jobs GitHub estão em `ci.yml`.

---

<a id="migration"></a>

## Migração

<a id="queue-and-status-changes"></a>

### Mudanças na fila e no status

Mudanças na estrutura SHM exigem reinício completo. A sintaxe método/ACL e os manipuladores de protocolo são preservados. v4 usa JSON por padrão, códigos correspondentes no cabeçalho e no corpo e `200 no_match` para entradas ausentes. `cache_purge_legacy_status` apenas gera um aviso. Atualize os clientes conforme o [contrato HTTP](../HTTP_RESPONSES.md). A capacidade inclui tarefas atribuídas; a sobrecarga aguarda admissão. Duplicatas ativas podem agendar outra passagem.

Status acrescenta campos e mantém os nomes existentes. `requests.overflow` agora conta esperas de admissão. Métricas são aproximadas, com acumuladores de 64 bits. Compilação com threads registra automaticamente `default` quando referenciado; use pool nomeado para isolar I/O. Configuração apenas de status aloca SHM.

Gerações e REFRESH são opções explícitas na v4.0. Vincule `cache_purge_key` e use `$cache_purge_key` como chave nativa inteira em todos os locations participantes. Isso muda nomes físicos; arquivos legados ficam frios, aguardando expiração nativa ou limpeza legada explícita. Preserve marcadores de estado junto com o cache. Locations PURGE existentes não ganham REFRESH. Usuários do PR60 experimental precisam configurar endpoint/perfil e paginação; resposta ilimitada, inferência URI/Host e expansão implícita de métodos do PR60 não são compromissos de compatibilidade.

<a id="from-the-original-frickle-module"></a>

### Do módulo FRiCKLE original

As formas método/lista de acesso e zona/chave continuam disponíveis. Entradas ausentes retornam `200 no_match`; remova `cache_purge_legacy_status` e atualize os clientes. Para ativar o processamento em segundo plano, adicione:

```nginx
http {
    cache_purge_background_queue on;
}
```

<a id="from-nginx-selective-cache-purge-module"></a>

### De nginx-selective-cache-purge-module

Substitua o módulo; a fila já está integrada. Mapeie parâmetros anteriores para `cache_purge_queue_size`, `cache_purge_batch_size` e `cache_purge_throttle_ms`.

---

<a id="temporary-directories"></a>

## Diretórios temporários

Percursos ignoram diretórios temporários configurados dentro da raiz, comparando componentes completos. Nomes como `proxy_temp` não são reservados se não forem caminhos temporários configurados. Apenas arquivos regulares com nome de exatamente 32 dígitos hexadecimais podem ser removidos. Temporários numéricos ou com sufixo são ignorados, inclusive com `use_temp_path=off`.

Cada percurso valida versão nativa do cabeçalho, offsets, marcador `KEY: ` completo, CRC32 e hash chave/variante do nome, com limite fixo de 8192 bytes de chave. Arquivos malformados, truncados, de outra versão ou inconsistentes são ignorados mesmo em purge-all. Comprimentos ETag/Vary iguais ao máximo do núcleo são aceitos. A validação detecta erros de formato; não autentica arquivos maliciosos.

A raiz pode ser symlink ou junction Windows; symlinks/reparse points descendentes são ignorados. Em POSIX com detecção completa de operações relativas a descritores, filhos usam `ngx_openat_file`/`ngx_file_at_info` do núcleo com flags no-follow; a remoção é relativa ao diretório pai mantido aberto. Isso impede redirecionamento por symlink descendente substituído, mas não protege contra mover um diretório já aberto. Windows e Unix antigos usam operações nativas de caminho completo com verificação de links, preservando uma corrida de substituição. Busca/remoção exata legada também usa caminhos completos. Mantenha raízes privadas, sem escritores não confiáveis.

Use grafia canônica consistente nos caminhos. Comparações não resolvem aliases de symlink, montagem ou Unicode. Tarefas conservam sua proteção; duplicata nova atualiza pendências ou próxima repetição. A passagem ativa mantém a proteção antiga; não é a união de todas as gerações vivas de workers. Não reutilize caminhos para outro fim enquanto tarefas afetadas estiverem pendentes ou ativas.

---

<a id="security"></a>

## Segurança

- Restrinja endpoints PURGE com `allow` / `deny` ou autenticação anterior. Nunca os exponha publicamente.
- Dimensione `cache_purge_queue_size` conforme o tráfego para limitar SHM sob inundação de requisições.
- Considere `limit_req` nos locations de limpeza para limitar PURGE independentemente do tráfego normal.

---

<a id="license"></a>

## Licença

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

## Veja também

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [Documentação nginx](https://nginx.org/en/docs/)
