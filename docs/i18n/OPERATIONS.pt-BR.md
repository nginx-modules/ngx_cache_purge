# ngx_cache_purge

[README](README.pt-BR.md) · [Diretivas](DIRECTIVES.pt-BR.md)

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

<a id="reading-cache-files"></a>

### Leitura de arquivos de cache no modo de gerações

Um navegador de cache, pré-carregador ou relatório de uso não pode deduzir a validade da geração apenas pelos arquivos: arquivos obsoletos podem permanecer após a invalidação, mesmo sem substituto. A linha `KEY:` de um arquivo de geração é `p4:<namespace>:<epoch>:<generation>:<logical key>`. O [endpoint de status](DIRECTIVES.pt-BR.md#cache_purge_queue_status) expõe o estado do namespace. A geração vigente de uma chave é `baseline`, salvo se uma regra exata ou de prefixo a elevou; essas regras não aparecem na resposta.

| Estado do namespace do arquivo | Resultado |
|---|---|
| Namespace ausente da lista | `unknown` |
| A época do arquivo difere de `epoch` | `stale` |
| A época coincide; a geração do arquivo é inferior a `baseline` | `stale` |
| A época coincide; a geração é pelo menos `baseline`; `rules` é `false` | `live` |
| A época coincide; a geração é pelo menos `baseline`; `rules` é `true` | `unknown` |

Aqui, **live** significa apenas que o arquivo não está obsoleto pela geração do namespace; não confirma a atualidade do conteúdo, a validade do TTL nem um HIT no cache NGINX. Com `namespaces_complete=false`, as entradas retornadas continuam utilizáveis e os namespaces omitidos são desconhecidos.

Um arquivo de geração superior com o mesmo namespace, época e chave lógica prova que o arquivo de geração inferior está obsoleto: a geração vigente nunca diminui dentro de uma época. Nos demais casos, mantenha o resultado unknown.

Leia o status antes e depois da varredura, associando as entradas pelo nome do namespace. Use os resultados baseados no status somente se o namespace estiver nas duas respostas e `epoch`, `baseline` e `serial` não mudarem. Estado ausente ou alterado invalida os resultados daquele namespace. Compare épocas apenas por igualdade. Essas observações não impedem invalidações posteriores.

Uma fila de limpeza vazia não comprova validade. Uma requisição iniciada antes da invalidação pode gravar um arquivo da geração antiga após o término da limpeza. O arquivo permanece até outra limpeza ou sua remoção pelo cache nativo. A limpeza também atinge apenas o caminho de cache da location de PURGE, enquanto um namespace pode ser compartilhado entre vários caminhos.

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

Use o [endpoint de status](DIRECTIVES.pt-BR.md#cache_purge_queue_status) para observar pendências, tarefas ativas, espera, duplicatas, remoções e HIT/MISS. `202 Accepted` confirma admissão/agregação, não remoção nem invalidação lógica imediata. Monitore latência junto com idade da fila e contenção de armazenamento.

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

<a id="temporary-directories"></a>

## Diretórios temporários

Percursos ignoram diretórios temporários configurados dentro da raiz, comparando componentes completos. Nomes como `proxy_temp` não são reservados se não forem caminhos temporários configurados. Apenas arquivos regulares com nome de exatamente 32 dígitos hexadecimais podem ser removidos. Temporários numéricos ou com sufixo são ignorados, inclusive com `use_temp_path=off`.

Cada percurso valida versão nativa do cabeçalho, offsets, marcador `KEY: ` completo, CRC32 e hash chave/variante do nome, com limite fixo de 8192 bytes de chave. Arquivos malformados, truncados, de outra versão ou inconsistentes são ignorados mesmo em purge-all. Comprimentos ETag/Vary iguais ao máximo do núcleo são aceitos. A validação detecta erros de formato; não autentica arquivos maliciosos.

A raiz pode ser symlink ou junction Windows; symlinks/reparse points descendentes são ignorados. Em POSIX com detecção completa de operações relativas a descritores, filhos usam `ngx_openat_file`/`ngx_file_at_info` do núcleo com flags no-follow; a remoção é relativa ao diretório pai mantido aberto. Isso impede redirecionamento por symlink descendente substituído, mas não protege contra mover um diretório já aberto. Windows e Unix antigos usam operações nativas de caminho completo com verificação de links, preservando uma corrida de substituição. Busca/remoção exata legada também usa caminhos completos. Mantenha raízes privadas, sem escritores não confiáveis.

Use grafia canônica consistente nos caminhos. Comparações não resolvem aliases de symlink, montagem ou Unicode. Tarefas conservam sua proteção; duplicata nova atualiza pendências ou próxima repetição. A passagem ativa mantém a proteção antiga; não é a união de todas as gerações vivas de workers. Não reutilize caminhos para outro fim enquanto tarefas afetadas estiverem pendentes ou ativas.

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

Veja [`t/TESTING.md`](../../t/TESTING.md) para compilações rigorosas, sanitizadores, Valgrind, jobs de plataforma e relatórios. Todos os jobs GitHub estão em `build-test-release.yml`.
