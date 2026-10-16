<a id="ngx_cache_purge"></a>

# ngx_cache_purge

Módulo NGINX para remover entradas dos caches `FastCGI`, `proxy`, `SCGI` e `uWSGI`. Na v4, as gerações de chaves impedem que novas requisições usem entradas antigas antes da exclusão dos arquivos. REFRESH consulta o servidor de origem para verificar se o conteúdo do cache proxy mudou. Os dois recursos exigem configuração explícita. A sintaxe PURGE continua disponível, mas as respostas HTTP mudaram: veja a [migração](README.pt-BR.md#migration).

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

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

<a id="choosing-a-mode"></a>

## Escolha o comportamento necessário

| Objetivo | Configuração | Significado da resposta |
|---|---|---|
| Excluir entradas desatualizadas e aguardar a tentativa de exclusão | PURGE sem fila em segundo plano | `200 purged` ou `200 no_match`; 4xx/5xx em caso de erro |
| Agendar a limpeza por prefixo ou de todo o cache | PURGE com `cache_purge_background_queue on` | `202 accepted`; entradas antigas ainda podem ser servidas enquanto aguardam |
| Impedir que novas requisições usem conteúdo antigo, inclusive após gravações tardias | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`; nova geração de chave, exclusão dos arquivos depois |
| Verificar alterações na origem | Gerações + `proxy_cache_refresh` + perfil interno | `200` com resultados por entrada e continuação; não preenche o cache |

Os mecanismos se combinam: a troca de geração também agenda a limpeza. A fila sozinha não protege contra gravações tardias. Veja o [exemplo v4](README.pt-BR.md#generation-and-refresh-example), os [exemplos PURGE](#sample-configurations) e as [respostas HTTP](../HTTP_RESPONSES.md) (em inglês).

<a id="compatibility"></a>

## Compatibilidade

Em produção, use uma versão do NGINX que ainda receba manutenção. POSIX/UNIX é o alvo principal; o suporte ao Windows é experimental. Threads são opcionais. [NGINX / CI](../../t/TESTING.md#compatibility-matrix).

<a id="distribution-packages"></a>

## Pacotes de distribuições

Diversas distribuições e projetos de ports empacotam este fork, mas não necessariamente a v4. Antes de instalar, verifique a versão do pacote e a ABI do NGINX. Consulte os [pacotes e suas fontes](../DISTRIBUTIONS.md).

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
./configure --with-compat --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

Use o código-fonte da mesma versão do NGINX instalada no servidor. `--with-compat` não garante compatibilidade entre quaisquer versões do NGINX ou bibliotecas C. Consulte [módulos dinâmicos e arquivos de release](../BINARY_MODULES.md) (em inglês).

<a id="generation-and-refresh-example"></a>

## Exemplo: catálogo de produtos

Este exemplo completo recebe requisições na porta local 8080 e as encaminha à aplicação na porta 8081. A aplicação deve servir `/products/42` com Host `shop.example.com`. Ajuste o endereço, o domínio e os diretórios graváveis de cache e estado.

`shop.example.com` é o domínio do cabeçalho HTTP Host, não um diretório. `/products/42` é o URI do produto. `catalog:` é um prefixo fixo de chave, não um hostname. O prefixo mais o URI completo, incluindo a query string, forma a chave lógica, como `catalog:/products/42`. O perfil REFRESH remove `catalog:` e verifica `/products/42` na mesma aplicação.

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

Faça GET de um produto existente e armazenável em cache até obter `X-Cache-Status: HIT`; depois envie REFRESH:

```sh
curl -i 'http://127.0.0.1:8080/products/42'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/42' \
  'http://127.0.0.1:8080/admin/refresh'
```

Se o produto mudou, envie PURGE. Aqui, `200 invalidated` impede que novas requisições usem a geração antiga; o próximo GET acessa a aplicação.

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/products/42'
```

Para verificar todos os produtos, use `catalog:/products/*`. Com `complete: false`, mantenha o seletor e substitua `<next_cursor>` pelo valor recebido. Resolva erros HTTP antes de continuar.

```sh
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

<a id="partial-key-purge"></a>

## Limpeza por chave parcial

Adicione `*` à **chave de cache avaliada** para buscar um prefixo sem diferenciar maiúsculas. Chaves exatas e Vary continuam diferenciando maiúsculas. O prefixo de limpeza física aceita até 511 bytes, sem o `*` final. Um prefixo maior gera um único registro no log e retorna `400 invalid_selector` sem percorrer arquivos.

Com `cache_purge_key`, a correspondência usa a expressão lógica e aceita até 8096 bytes, incluindo `*` final.

Para uma expressão terminada em `$uri`, esta requisição seleciona um prefixo de URI:

```http
PURGE /images/header*
```

O asterisco deve ser o último byte da chave avaliada, não apenas do caminho URI. Considere query strings e sufixos variáveis em `proxy_cache_key`. Uma chave formada apenas por `*` percorre toda entrada elegível.

<a id="generation-invalidation"></a>

## Invalidação por gerações

Use gerações de chaves para impedir que novas requisições obtenham conteúdo antigo após o PURGE, inclusive respostas gravadas com atraso. Os arquivos são removidos separadamente. Um namespace compartilha regras de invalidação; preserve seu diretório de estado entre reinicializações.

<a id="conditional-refresh"></a>

## Verificar o cache na origem (REFRESH)

REFRESH envia HEAD para a mesma representação na origem usando ETag e Last-Modified do cache. Se a página do produto já está desatualizada, use PURGE. REFRESH não baixa outro corpo, não preenche um cache vazio nem estende o TTL. [HTTP / REFRESH](../REFRESH.md).

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

<a id="sample-configurations"></a>

## Exemplos de configuração

Os cinco primeiros blocos são exemplos completos e independentes de `nginx.conf`. O último acrescenta o tratamento de Vary a uma seção `http` existente. O NGINX recebe conexões locais na porta 8080; `catalog_backend` é o grupo da aplicação em `127.0.0.1:8081`. `shop.example.com` é o HTTP Host enviado à aplicação. Ajuste os endereços e use um diretório de cache com permissão de escrita para o NGINX.

<a id="basic--inline-purge-method"></a>

### Básico — método de limpeza no próprio location

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

### Básico — location de limpeza separado

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

### Fila em segundo plano ativada

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

### Remover todas as entradas

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

### Respostas JSON com configuração por location

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

### Limpeza com suporte a Vary

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

Após limpar uma chave, todas as variantes gzip e Vary em caminhos diferentes também são removidas automaticamente.

<a id="migration"></a>

## Migração

Mudanças na estrutura SHM exigem reinício completo. A sintaxe método/ACL e os manipuladores de protocolo são preservados. v4 usa JSON por padrão, códigos correspondentes no cabeçalho e no corpo e `200 no_match` para entradas ausentes. `cache_purge_legacy_status` apenas gera um aviso. Atualize os clientes conforme o [contrato HTTP](../HTTP_RESPONSES.md). A capacidade inclui tarefas atribuídas; a sobrecarga aguarda admissão. Duplicatas ativas podem agendar outra passagem.

<a id="security"></a>

## Segurança

- Restrinja endpoints PURGE com `allow` / `deny` ou autenticação anterior. Nunca os exponha publicamente.
- Dimensione `cache_purge_queue_size` conforme o tráfego para limitar SHM sob inundação de requisições.
- Considere `limit_req` nos locations de limpeza para limitar PURGE independentemente do tráfego normal.

<a id="documentation"></a>

## Documentação

- [Referência de diretivas](DIRECTIVES.pt-BR.md)
- [Outros exemplos de configuração](README.pt-BR.md#sample-configurations)
- [Operação e diagnóstico](OPERATIONS.pt-BR.md)
- [HTTP e limites de REFRESH (inglês)](../REFRESH.md)
- [Migração para v4](MIGRATION.pt-BR.md)
- [Arquitetura da implementação (inglês)](../ARCHITECTURE.md)
- [Testes](OPERATIONS.pt-BR.md#testing)
- [Desenvolvimento e testes (inglês)](../../t/TESTING.md)

<a id="see-also"></a>

## Veja também

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [Documentação nginx](https://nginx.org/en/docs/)

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
