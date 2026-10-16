# ngx_cache_purge

[README](README.pt-BR.md) · [Diretivas](DIRECTIVES.pt-BR.md)

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
