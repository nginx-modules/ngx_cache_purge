[ngx_cache_purge](README.fr.md)

<a id="directives"></a>

# Directives

<a id="proxy_cache_purge"></a>

## `proxy_cache_purge`

```
Syntax:  proxy_cache_purge on | off;
         proxy_cache_purge <method> [purge_all] from all | <cidr> ...;
         proxy_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

**Forme intégrée** (`from …`) — intercepte la méthode indiquée dans le location proxy et purge l’entrée. `on` abrège `PURGE`, `off` désactive. Restreignez par CIDR ou autorisez tous avec `from all`. `purge_all` avant `from` vide toute la zone indépendamment de l’URI.

**Location séparé** (zone et clé) — autorisé seulement dans un location de purge, généralement une regex capturant la clé. Recherche la zone par nom et purge l’expression compilée. Incompatible avec `proxy_cache` et `proxy_pass` dans ce même location.

<a id="fastcgi_cache_purge"></a>

## `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Équivalent à `proxy_cache_purge` pour les zones FastCGI définies par `fastcgi_cache` / `fastcgi_cache_path`.

<a id="scgi_cache_purge"></a>

## `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Équivalent à `proxy_cache_purge` pour les zones SCGI définies par `scgi_cache` / `scgi_cache_path`.

<a id="uwsgi_cache_purge"></a>

## `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Équivalent à `proxy_cache_purge` pour les zones uWSGI définies par `uwsgi_cache` / `uwsgi_cache_path`.

<a id="cache_purge_response_type"></a>

## `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

Choisit le format des réponses PURGE réussies ; JSON est utilisé par défaut. `Cache-Purge-Code` correspond au code du corps. Les réponses du module incluent `Cache-Control: no-store` ; les erreurs utilisent `application/problem+json` selon la RFC 9457. Les clés textuelles sont échappées ; les clés binaires sont encodées en Base64 avec indication de l’encodage.

`none` supprime le corps PURGE : une opération réussie et terminée renvoie `204`, un travail accepté en arrière-plan conserve `202` et les erreurs conservent 4xx/5xx. État et REFRESH gardent leur JSON structuré. Voir le [contrat HTTP](../HTTP_RESPONSES.md) pour les codes, champs et limites.

<a id="cache_purge_background_queue"></a>

## `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

Active le traitement différé des jokers et de `purge_all`. Renvoie `202 Accepted` après mise en file ou regroupement avec une tâche identique en attente/active. Les purges exactes ne changent pas.

Cette option contrôle les purges historiques par joker/totales. Une liaison `cache_purge_key` utilise toujours nettoyage différé et réponse logique `200`, même option désactivée.

File pleine ou allocation échouée maintient la requête ouverte jusqu’à admission, sans parcours pendant l’attente. Un doublon peut être admis à capacité maximale. Échouer à allouer le petit état d’attente renvoie `500`. Le nettoyage de déconnexion annule le temporisateur et libère la jauge d’attente.

Pour cet indicateur et `cache_purge_vary_aware`, la dernière valeur déclarée prévaut. Les valeurs invalides sont rejetées. `cache_purge_legacy_status` est obsolète et ignoré. Threads et cycle de vie : [modèle d’exécution](../ARCHITECTURE.md#execution-model).

<a id="cache_purge_queue_size"></a>

## `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

Maximum de tâches inachevées sur toutes les racines. Attentes et tâches attribuées consomment la capacité. Les doublons partagent une allocation ; une répétition conserve allocation et place. Les tâches distinctes attendent une place libre.

La zone partagée est créée avec le traitement de fond ou le statut. Changer la capacité exige un redémarrage même si la taille arrondie reste identique. Reload refuse le redimensionnement pour préserver le travail. Ajouter des racines peut aussi nécessiter mémoire et redémarrage. Drainez d’abord les tâches devant finir.

Déclarer un espace de noms crée également la file.

La zone inclut tâches, métadonnées de chemins et métriques alignées des workers. Elle dépend de la compilation et des chemins ; voir [dimensionnement mémoire](../OPERATIONS.md#memory-sizing).

<a id="cache_purge_thread_pool"></a>

## `cache_purge_thread_pool`

```
Syntax:  cache_purge_thread_pool <name>;
Default: default
Context: http, server, location
```

Sélectionne l’exécuteur des tâches de fond admises, avec héritage normal. Exemple :

```nginx
thread_pool purge_io threads=2 max_queue=128;
http {
    cache_purge_background_queue on;
    cache_purge_thread_pool purge_io;
}
```

NGINX valide les pools et signale la fin par l’événement natif de tâche ; aucune structure privée ni file de fin n’est copiée. Avec threads, les pools personnalisés doivent exister. Sans threads ou sous Windows, la directive est acceptée avec tranches bornées. Un pool plein retarde l’exécution sans ramener l’E/S dans la boucle. Les pools dédiés ajoutent piles et changements de contexte.

<a id="cache_purge_batch_size"></a>

## `cache_purge_batch_size`

```
Syntax:  cache_purge_batch_size <number>;
Default: 10
Minimum: 1
Maximum: 64
Context: http
```

Maximum de tâches par worker entre pauses. L’avancement survit aux tranches et aux fins de threads. Une file vide termine le lot. Les valeurs supérieures à 64 sont plafonnées avec avertissement ; zéro est rejeté.

Contrôle des tâches, pas des fichiers ni la durée des callbacks. Réduire ajoute des pauses sans interrompre le parcours actif d’un thread. Combinez avec `cache_purge_throttle_ms` pour régler la charge disque.

<a id="cache_purge_throttle_ms"></a>

## `cache_purge_throttle_ms`

```
Syntax:  cache_purge_throttle_ms <time>;
Default: 10ms
Minimum: 1ms
Context: http
```

Pause entre lots. Accepte `10ms`, `500ms`, `1s` et des composés entre guillemets comme `"2s 500ms"`. Un entier sans unité indique des millisecondes : `10` équivaut à `10ms`. Il produit un avertissement évité par une unité explicite.

Un worker inactif vérifie après dix fois cet intervalle. Tranches incomplètes et mutex occupé utilisent 1 ms. Ce sont des intervalles minimaux de planification, pas des échéances. Zéro et valeurs hors plage sûre de la plateforme sont refusés.

Admission, saturation du pool et reprises différées sont sondées toutes les 10 ms.

<a id="cache_purge_queue_status"></a>

## `cache_purge_queue_status`

```
Syntax:  cache_purge_queue_status <cache_path>;
Default: not configured
Context: location
```

Sert un instantané JSON GET/HEAD avec `Cache-Control: no-store`. Le chemin doit correspondre à une racine configurée ; sinon le chargement échoue, même sans traitement de fond. Restreignez l’accès :

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

`queue` décrit la file partagée de tous les chemins :

| Champ | Type / unité | Sens |
|---|---|---|
| `size` | Entier / tâches | Tâches attendant un worker |
| `running` | Entier / tâches | Tâches attribuées, y compris l’attente de pool |
| `max_size` | Entier / tâches | Capacité totale en attente et attribuée |
| `wait_time` | Entier / millisecondes | Âge de la plus ancienne attente ; zéro si vide |
| `full` | Booléen | L’échantillon en attente plus attribué atteint la capacité |

`cache` décrit la racine choisie par ce location :

| Champ | Type / unité | Sens |
|---|---|---|
| `pending.total` | Entier / tâches | Attentes de ce cache |
| `pending.full` | Entier / tâches | `purge_all` en attente, inclus dans `pending.total` |
| `active` | Entier / tâches | Attribuées jusqu’à fin de comptabilisation |
| `requests.overflow` | Entier / requêtes | Attentes d’admission, y compris blocage/allocation |
| `requests.deduplicated` | Entier / requêtes | Regroupées avec du travail en attente ou actif |
| `requests.waiting` | Entier / requêtes | Attentes d’admission actuelles pour ce cache |
| `files.deleted` | Entier / fichiers | Suppressions de fond réussies rapportées en fin de passage |
| `files.errors` | Entier / erreurs | Échecs de suppression hors fichier déjà absent |
| `files.bytes` | Entier / octets | Tailles logiques estimées des fichiers supprimés |
| `responses.hit`, `responses.miss` | Entier / requêtes principales | Décisions natives HIT/MISS observées en phase de log |
| `responses.hit_ratio` | Nombre / 0..1 | HIT / (HIT + MISS), ou zéro sans observation |
| `logical.invalidations` | Entier / décisions | Invalidations publiées, doublons et REFRESH compris |
| `cleanup.scan_errors` | Entier / erreurs | Échecs de métadonnées/ouverture/lecture/fermeture/allocation de fond |
| `cleanup.retries` | Entier / passages | Passages supplémentaires après échec du nettoyage |
| `cleanup.exhausted` | Entier / tâches | Tâches retirées après trois reprises échouées |

`namespaces` présente les entrées `cache_purge_namespace` configurées, quel que soit le cache choisi par cette location : tous les chemins de cache d’un espace de noms partagent son état de générations. Chaque entrée est copiée sous le verrou de son espace de noms. Les entrées sont relevées séparément ; la liste n’est pas un instantané global.

| Champ | Type / unité | Signification |
|---|---|---|
| `name` | Chaîne | Nom de l’espace de noms dans le préfixe de clé `p4:<name>:` |
| `epoch` | Chaîne / 10 chiffres | Époque actuelle de l’espace de noms ; un redémarrage complet en crée une nouvelle |
| `baseline` | Chaîne / 16 chiffres hexadécimaux en minuscules | Génération des clés qu’aucune règle n’a augmentée ; une invalidation de tout l’espace la fixe à `serial` |
| `serial` | Chaîne / 16 chiffres hexadécimaux en minuscules | Numéro de séquence démarrant à 1, augmenté à chaque invalidation publiée dans cette époque |
| `rules` | Booléen | `true` si des règles exactes ou de préfixe existent ; une invalidation de tout l’espace les efface, y compris lorsqu’un manque de mémoire partagée élargit l’invalidation |

`baseline` et `serial` ont la même largeur fixe que la génération d’une clé et se comparent comme des chaînes. `namespaces_complete` vaut `false` lorsque la liste est tronquée pour tenir dans la réponse. Les entrées renvoyées restent utilisables ; un espace de noms absent est inconnu. Voir [lire les fichiers de cache en mode générations](OPERATIONS.fr.md#reading-cache-files).

`pending.full` compte les purges complètes. Le joker de clé `*` est une autre opération. `active` comprend les attentes de pool ; exclu de `queue.size` et `pending.total`, il consomme néanmoins la capacité.

Pour un balayage regroupé, `pending.full` reflète la classification de la première tâche admise. Scans/sondes REFRESH ne comptent ni dans la profondeur de file ni dans les erreurs de suppression. Une reprise différée déplacée peut faire de `wait_time` l’âge de la tête échantillonnée plutôt que l’attente maximale.

Chaque slot de processus a un écrivain dans la boucle ; débuts et pas sont alignés sur les lignes de cache. Les threads retournent par événements de fin. Le lecteur ne prend ni mutex ni atomic fetch-add, même avec zéro. En 64 bits, lectures/écritures volatile alignées ordinaires ; en 32 bits, contrôles de séquence bornés protégeant les lectures 64 bits, avec omission possible d’un segment occupé. Les champs sont approximatifs, non transactionnels entre workers. L’admission utilise l’état de contrôle verrouillé, jamais les métriques.

Les cumuls saturent au maximum unsigned 64 bits, persistent au reload et repartent à zéro au redémarrage. La jauge d’attente suit le nettoyage des requêtes. Opérations synchrones et suppressions du cache manager sont exclues. Les octets estiment des longueurs, pas les blocs ni l’espace récupéré : lecteurs ouverts et écritures concurrentes influent. Les résultats partiels comptent ; ceux d’un worker planté peuvent manquer. Ce n’est pas un journal d’audit.

HIT/MISS compte uniquement les requêtes principales avec décision native. Upstream absent, statut nul, BYPASS, EXPIRED, STALE, UPDATING et REVALIDATED sont exclus du ratio. Une requête normalement non cachée ne devient pas MISS. La supervision seule peut créer SHM sans purge de fond. En 32 bits, l’âge couvre moins d’environ 24,9 jours ; une différence d’horloge signée négative est ramenée à zéro.

<a id="cache_purge_legacy_status"></a>

## `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

Directive obsolète, acceptée uniquement pour la migration des configurations. Les deux valeurs produisent un avertissement et sont ignorées ; supprimez la directive. Les valeurs invalides sont rejetées. Une absence de correspondance synchrone renvoie `200 no_match`, un nettoyage physique accepté `202 accepted`, une invalidation de génération publiée `200 invalidated`. Le choix entre `412` et `404` disparaît. Voir le [contrat HTTP](../HTTP_RESPONSES.md).

<a id="cache_purge_vary_aware"></a>

## `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

Avec `on`, une purge exacte réussie supprime le fichier principal puis parcourt le cache pour supprimer les fichiers portant la même chaîne `KEY:`. Cela couvre `Vary` et `gzip_vary`, sur des chemins distincts partageant la clé logique. L’octet suivant doit être `\n`, évitant une simple correspondance de préfixe.

Désactivé par défaut car chaque purge exacte ajoute un parcours complet. À activer pour plusieurs représentations `Vary`/`gzip_vary`. Le parcours a aussi lieu si l’entrée principale est déjà absente. Les jokers et `purge_all` couvrent déjà les variantes. Les clés Vary exactes sont sensibles à la casse et limitées à 511 octets ; au-delà, `400 invalid_selector` est renvoyé avant toute suppression.

L’invalidation par générations couvre toutes les représentations de la clé sans cette option.

<a id="cache_purge_namespace"></a>

## `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

Déclare un espace de générations avant ses liaisons aux locations. Nom de 1–32 lettres ASCII, chiffres, `_` ou `-` ; SHM d’au moins huit pages natives. Choisissez un répertoire d’état persistant dédié hors racines de cache et temporaires. Le noyau crée un marqueur persistant exclusif pour allouer une époque de redémarrage. Gardez ces petits fichiers tant que le cache associé peut exister : supprimer l’état seul permet de réutiliser une époque obsolète. Aucune garantie de durabilité coupure électrique/fsync. Reload préserve époque et règles ; changer taille/chemin ou retirer l’espace impose un redémarrage. Espace et file ont des tailles SHM séparées.

<a id="cache_purge_key"></a>

## `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

Lie le location à un espace existant et expose `$cache_purge_key`. Cette variable doit être la totalité de `proxy_cache_key`, `fastcgi_cache_key`, `scgi_cache_key` ou `uwsgi_cache_key`, y compris dans le location PURGE distinct. Une clé native développée différente de la clé générée fait échouer PURGE avec `500`, sans prétendre à une invalidation stricte. Ne référencez pas `$cache_purge_key` dans sa propre expression. Maximum logique : 8096 octets. Ne liez que les locations participants si les autres gardent l’ancien fonctionnement ; aucun `off` n’annule une liaison héritée.

<a id="proxy_cache_refresh"></a>

## `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

Crée un endpoint REFRESH explicite réservé aux requêtes principales ; autres méthodes : `405`. Nécessite liaison `cache_purge_key`, expression native `$cache_purge_key` et profil refresh. La clé logique développée sélectionne une clé exacte, un préfixe terminé par `*` ou `*` pour l’espace sur ce chemin. Protégez le location par accès/authentification natifs. REFRESH ne couvre que proxy ; PURGE conserve les quatre protocoles.

<a id="cache_purge_refresh_profile"></a>

## `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

Associe une représentation connue à un location proxy interne littéral. Retirer le préfixe exact sensible à la casse doit laisser une URI d’origine commençant par `/`, paramètres et échappements conservés. Le module ne devine pas l’hôte et ne cherche pas `://` dans la clé. Le destinataire doit avoir `internal`, cache désactivé, transmission des en-têtes/corps désactivée, méthode HEAD, Host littéral non vide et en-têtes de représentation littéraux. Les deux en-têtes conditionnels utilisent exactement `$cache_purge_refresh_etag` et `$cache_purge_refresh_last_modified` ; `$cache_purge_refresh_uri` fournit l’URI à `proxy_pass`. Ces trois variables pures ne sont pertinentes que dans les sondes. Les vérifications passent en preaccess, redirections internes comprises. L’administrateur garantit la représentation d’origine correspondant à la clé ; Vary est ignoré, faute d’en-têtes originaux dans les métadonnées natives.

Configurez `proxy_intercept_errors off` dans la sonde ; une interception héritée est rejetée afin qu’une erreur d’origine ne devienne pas un succès invalidant.

<a id="cache_purge_refresh_timeout"></a>

## `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

Borne l’envoi de nouveaux scans/sondes par page, attente du scanner comprise. À expiration, le nouveau travail cesse et la réponse devient incomplète. Un HEAD actif termine selon `proxy_connect_timeout`, `proxy_send_timeout` et `proxy_read_timeout`, à configurer explicitement. Ce n’est ni une échéance absolue de réponse ni une interruption de syscall. Déconnexion et arrêt gracieux cessent les envois, en gardant la mémoire du thread actif jusqu’à sa fin native.
