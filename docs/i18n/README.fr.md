<a id="ngx_cache_purge"></a>

# ngx_cache_purge

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

Module NGINX permettant de supprimer des entrées des caches `FastCGI`, `proxy`, `SCGI` et `uWSGI`. En v4, les générations de clés empêchent les nouvelles requêtes de retrouver une ancienne entrée, avant même la suppression de son fichier. REFRESH vérifie auprès du serveur d’origine si le contenu du cache proxy a changé. Ces deux fonctions doivent être configurées explicitement. La syntaxe PURGE reste disponible, mais les réponses HTTP changent : voir la [migration](#migration).

---

<a id="contents"></a>

## Sommaire

- [Fonctionnalités](#features)
- [Choisir le comportement adapté](#choosing-a-mode)
- [Compatibilité](#compatibility)
- [Installation](#installation)
- [Purge par clé partielle](#partial-key-purge)
- [Invalidation par générations](#generation-invalidation)
- [Vérifier le cache auprès de l’origine (REFRESH)](#conditional-refresh)
- [Exemples de configuration](#sample-configurations)
- [Directives](#directives)
- [Modèle d’exécution](#execution-model)
- [Réglage des performances](#performance-tuning)
- [Supervision et débogage](#monitoring-and-debugging)
- [Dépannage](#troubleshooting)
- [Tests](#testing)
- [Migration](#migration)
- [Répertoires temporaires](#temporary-directories)
- [Sécurité](#security)
- [Licence](#license)

---

<a id="features"></a>

## Fonctionnalités

- **Purge intégrée** — méthode HTTP dédiée (`PURGE`) et contrôle d’accès IP
- **Location de purge distinct** — syntaxe zone/clé `proxy_cache_purge zone key` pour une clé capturée par expression régulière, sans `proxy_pass`
- **Joker / purge partielle** — `*` final parcourt le cache et supprime les entrées correspondantes
- **`purge_all`** — supprime toute la zone en une requête
- **File d’attente en arrière-plan** — parcours différé par tranches bornées de la boucle événementielle ou pool de threads NGINX facultatif ; lots et pauses configurables
- **Prise en charge de Vary** — après une purge exacte, supprime les variantes sur disque (gzip, Vary) partageant la même clé
- **Formats de réponse** — `json` (par défaut), `html`, `xml`, `text`, `none`
- **Pools nommés** — sélection héritée, repli incrémental sans threads et sous Windows
- **Déduplication des tâches actives** — une opération indexée et au plus une répétition en attente pour les purges reçues pendant le parcours
- **État** — métriques de file, suppression et HIT/MISS par cache, sans verrouiller le mutex partagé à la lecture
- **Clés de génération** — invalidation exacte, par préfixe ou espace de noms sans E/S disque liée à la requête ; les écritures anciennes tardives restent inaccessibles
- [Vérifier le cache auprès de l’origine (REFRESH)](#conditional-refresh)

---

<a id="choosing-a-mode"></a>

## Choisir le comportement adapté

| Objectif | Configuration | Sens de la réponse |
|---|---|---|
| Supprimer des entrées périmées et attendre la tentative de suppression | PURGE, file d’attente désactivée | `200 purged` ou `200 no_match` ; 4xx/5xx en cas d’erreur |
| Planifier une purge par préfixe ou de tout le cache | PURGE avec `cache_purge_background_queue on` | `202 accepted` ; les anciennes entrées peuvent encore être servies en attendant |
| Empêcher toute nouvelle requête d’utiliser une ancienne entrée, même écrite tardivement | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated` ; nouvelle génération de clé, puis suppression des fichiers |
| Vérifier si le contenu a changé à l’origine | Générations + `proxy_cache_refresh` + profil interne | `200` avec résultats par entrée et continuation ; aucun préchauffage |

Ces mécanismes se combinent : une invalidation par génération programme aussi le nettoyage. La file seule ne protège pas des écritures tardives. Voir l’[exemple v4](#generation-and-refresh-example), les [exemples PURGE](#sample-configurations) et les [réponses HTTP](../HTTP_RESPONSES.md) (en anglais).

---

<a id="compatibility"></a>

## Compatibilité

La matrice présente les cibles configurées en CI. Un job réussi valide cette configuration de sources et de compilation, pas toutes les versions d’une série.

| NGINX | CI: push / PR | CI: + full |
|---|---|---|
| 1.30.5, 1.31.6 | `test-matrix` | — |
| 1.31.6 | `build-compatibility` (GCC/Clang), `sanitizer`, `native-macos`, `native-windows` (x64/x86) | `memory-leak-test` (Valgrind) |
| 1.7.9 | `build-compatibility` (GCC) | — |
| 1.7.11, 1.7.12 | `build-compatibility` (GCC) | — |
| 1.20.2, 1.26.3, 1.28.3 | — | `test-matrix` |
| 1.11.4, 1.11.5 | — | `build-compatibility` (GCC) |

Les jobs Linux emploient les mêmes images Docker et point d’entrée que les tests locaux. Les jobs natifs macOS et Windows exercent leurs API de fichiers et processus ; Windows compile avec MSVC et teste HTTP, chemins et reload dans `nginx.exe`. Les scénarios POSIX processus/signaux tournent sous Linux et macOS. Windows n’exécute ni ces scénarios ni la branche facultative du pool de threads NGINX.

Sans `--with-threads`, le parcours est incrémental. Avec epoll avant NGINX 1.7.12, ce mode évite aussi une course lors de la notification du cœur. BSD n’a pas de CI natif. Les exécutions normales vérifient stable/mainline et les limites d’API ; les exécutions étendues ajoutent d’anciennes versions et Valgrind. Les changements purement documentaires ne lancent pas de compilation. Calendrier et commandes : [tests](../../t/TESTING.md).

Le code est réparti entre configuration, gestionnaires, file, parcours, invalidation, REFRESH et réponses. Les fonctions communes de chemins sont dans `ngx_cache_purge.h`, les opérations de plateforme dans les en-têtes POSIX/Win32. `generation.*` devient `invalidation.*` ; génération désigne toujours la version des clés. Voir l’[architecture](../ARCHITECTURE.md).

Le module suit le style NGINX compatible C89 et ses API d’événements, slab, rbtree et opérations atomiques. Il n’exige ni atomiques C11 ni API de fichiers récente. Les compilations GNU89 strictes vérifient l’ordre des déclarations et les avertissements ; compilateur et plateforme réellement testés accompagnent les résultats.

---

<a id="installation"></a>

## Installation

```bash
cd /path/to/nginx-source
./configure --add-module=/path/to/ngx_cache_purge
make
make install
```

Module dynamique :

```bash
./configure --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

---

<a id="partial-key-purge"></a>

## Purge par clé partielle

Ajoutez `*` à la **clé de cache évaluée** pour rechercher un préfixe sans tenir compte de la casse. Les clés exactes et Vary restent sensibles à la casse. Un préfixe de nettoyage physique accepte 511 octets au maximum, hors `*` final. Au-delà, un message est journalisé et `400 invalid_selector` est renvoyé sans parcours des fichiers.

Avec `cache_purge_key`, la comparaison porte sur l’expression logique, jusqu’à 8096 octets, `*` final compris.

Pour une expression finissant par `$uri`, cette requête choisit un préfixe URI :

```http
PURGE /images/header*
```

L’astérisque doit terminer la clé évaluée, pas seulement le chemin URI. Tenez compte des paramètres et suffixes variables de `proxy_cache_key`. Une clé limitée à `*` parcourt toutes les entrées admissibles.

---

<a id="generation-invalidation"></a>

## Invalidation par générations

Une clé logique identifie un objet. Un espace de noms regroupe les clés soumises aux mêmes règles. Une génération est une version ajoutée à la clé NGINX : après PURGE, les nouvelles requêtes utilisent une autre version. Un GET commencé avant PURGE peut finir, mais son fichier tardif ne redevient pas un HIT pour les nouvelles requêtes.

Dans un espace lié, PURGE publie une nouvelle génération et programme un balayage dédupliqué des anciennes avant `200` et le statut `invalidated`. Cette admission n’ouvre ni ne supprime de fichier. Une nouvelle requête évaluant la clé pour la première fois après publication ne touche pas l’ancienne génération. Chaque requête fixe sa génération par espace/clé, redirections internes comprises ; une réponse commencée peut finir, mais sa tardive écriture ne devient pas un hit pour les nouvelles requêtes. L’exact est sensible à la casse, les préfixes ignorent la casse ASCII. `purge_all` modifie tout l’espace, autres chemins le partageant volontairement compris. Le nettoyage vise le chemin demandé ; les autres dépendent de l’expiration native ou d’une purge suivante.

L’admission ne garantit pas une libération immédiate du disque. Une écriture ancienne après balayage reste inaccessible, puis disparaît au nettoyage suivant ou à expiration d’inactivité. Sous pression SHM, les règles se replient sur une nouvelle base de l’espace : des clés supplémentaires deviennent froides, sans réapparition des invalidées. Chaque invalidation ultérieure récupère au plus 64 nœuds détachés du trie. Purger souvent une même clé ne fait pas croître l’arbre indéfiniment. La complexité dépend de la longueur bornée de clé et de l’index, pas du nombre de fichiers ; aucune promesse O(1) en temps réel ni d’absence de famine sous surcharge. L’admission peut attendre. Utilisez `proxy_cache_lock` et équivalents pour limiter les ruées GET après invalidation.

<a id="conditional-refresh"></a>

## Vérifier le cache auprès de l’origine (REFRESH)

Utilisez REFRESH quand une entrée est en cache mais que vous ignorez si sa source a changé, par exemple après un import de catalogue. Si vous savez déjà que le contenu est périmé, utilisez PURGE. REFRESH envoie un HEAD sans télécharger le corps, avec l’ETag ou la date de modification du cache dans les en-têtes de validation HTTP. Le « profil » est une location proxy interne qui fixe le serveur, Host, les en-têtes et le chemin à vérifier.

REFRESH vérifie les objets encore cachés avant expiration du TTL, sans préchauffage ni prolongation sur `304`. Il complète `proxy_cache_revalidate` et `proxy_cache_background_update`, chargés des objets expirés ordinaires. Si l’origine ignore les validateurs et renvoie toujours `200`, REFRESH devient invalidation plus HEAD supplémentaire. Seules les entrées courantes, valides et de chemin hash canonique sont candidates. Une substitution physique dans la même génération peut être invalidée par prudence ; comparer les générations empêche un ancien HEAD d’invalider une génération PURGE ultérieure.

| Résultat brut d’origine | Action / résultat |
|---|---|
| `304` | Conserver ; `kept` |
| `200`, `404`, `410` | Invalidation conditionnelle et nettoyage différé ; `invalidated` |
| Autre statut | Conserver ; `upstream_status` |
| Aucun statut / échec transport | Conserver ; `transport_error` |
| Métadonnées Vary | Aucun HEAD ; `skipped_vary` |
| Association ou profil invalide | Conserver ; `skipped_profile` |
| Génération remplacée entre-temps | Aucune nouvelle invalidation ; `raced_replaced` |
| Échec interne invalidation/allocation | Conserver ; `invalidation_error` |
| Travail non envoyé/achevé à l’arrêt de page | `undispatched` ; le curseur ne le dépasse pas |

Un worker détient au plus une page active et une sonde ; quota partagé de 16 sondes entre workers. Les autres pages attendent sans tableau de candidats. Une page parcourt avec le curseur commun, conserve au plus 32 candidats puis émet les HEAD natifs en série, bornant mémoire et sous-requêtes du pool principal. Les requêtes REFRESH identiques ne sont pas dédupliquées mais partagent le quota. Leurs invalidations utilisent la file PURGE dédupliquée commune.

Commencez sans curseur, puis envoyez `X-Cache-Purge-Cursor: <next_cursor>` au même endpoint avec mêmes sélecteur/profil jusqu’à `complete` vrai. Le curseur est le dernier nom de fichier terminé de 32 caractères hexadécimaux ; en-têtes invalides ou dupliqués : `400`. `scan_complete` indique une énumération réussie ; `complete` exige aussi aucune page suivante et toutes décisions terminées. `timed_out` signale la limite de temps ; un échec du parcours renvoie `500 scan_failed`. `undispatched` compte les sélectionnés inachevés ; un scan incomplet donne une borne inférieure, pas la taille totale du cache. Si incomplet, aucune sonde, résultats vides et curseur d’entrée inchangé. Les résultats exposent hashes et statuts upstream bruts, pas clés ni identifiants secrets.

Chaque page reparcourt le répertoire pour choisir les hashes suivants : mémoire bornée contre E/S répétée, `O(files × pages)` au total. La pagination est faiblement cohérente avec les insertions/suppressions concurrentes ; recommencez pour un instantané ultérieur complet. Adaptez sélecteurs et délais. Cette interface administrative ne convient pas aux scans très fréquents de caches actifs très volumineux.

<a id="generation-and-refresh-example"></a>

### Exemple de générations et REFRESH

Ce fragment s’intègre aux contextes main/http ; conservez votre bloc `events`. Adaptez l’origine et les chemins. Sans compilation avec `--with-threads`, retirez `thread_pool` et `cache_purge_thread_pool` pour utiliser le parcours incrémental.

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

Demandez d’abord un objet existant et pouvant être mis en cache. Répétez GET jusqu’à obtenir `X-Cache-Status: HIT`, puis vérifiez cet objet avec REFRESH. Après `kept`, le GET suivant reste un HIT ; après `invalidated`, il retourne à l’origine.

```sh
curl -i 'http://127.0.0.1:8080/objects/item'
curl -i -X REFRESH -H 'X-Cache-Key: origin.test/objects/item' \
  'http://127.0.0.1:8080/admin/refresh'
```

Pour un préfixe, commencez sans curseur. Si `complete` vaut `false`, répétez avec la valeur retournée dans `X-Cache-Purge-Cursor`, à la place de `<next_cursor>`. Gardez le même sélecteur. Résolvez les erreurs HTTP avant de continuer.

```sh
curl -i -X REFRESH -H 'X-Cache-Key: origin.test/objects/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: origin.test/objects/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

Si l’objet est déjà connu comme périmé, utilisez PURGE. Avec les générations de cet exemple, `200 invalidated` empêche les nouvelles requêtes d’atteindre l’ancienne génération, même si ses fichiers existent encore.

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/objects/item'
```

Voir [HTTP et cas limites](../REFRESH.md) (en anglais). Un échec du parcours renvoie `500 scan_failed`, pas une page de continuation avec `scan_error: true`.

<a id="sample-configurations"></a>

## Exemples de configuration

<a id="basic--inline-purge-method"></a>

### Base — méthode de purge intégrée

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

### Base — location de purge distinct

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

### File en arrière-plan activée

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

### Purger toutes les entrées

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

### Réponses JSON avec réglage par location

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

### Purge tenant compte de Vary

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

Après purge d’une clé, ses variantes gzip et Vary sur d’autres chemins sont également supprimées automatiquement.

---

<a id="directives"></a>

## Directives

<a id="proxy_cache_purge"></a>

### `proxy_cache_purge`

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

### `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Équivalent à `proxy_cache_purge` pour les zones FastCGI définies par `fastcgi_cache` / `fastcgi_cache_path`.

<a id="scgi_cache_purge"></a>

### `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Équivalent à `proxy_cache_purge` pour les zones SCGI définies par `scgi_cache` / `scgi_cache_path`.

<a id="uwsgi_cache_purge"></a>

### `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Équivalent à `proxy_cache_purge` pour les zones uWSGI définies par `uwsgi_cache` / `uwsgi_cache_path`.

<a id="cache_purge_response_type"></a>

### `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

Choisit le format des réponses PURGE réussies ; JSON est utilisé par défaut. `Cache-Purge-Code` correspond au code du corps. Les réponses du module incluent `Cache-Control: no-store` ; les erreurs utilisent `application/problem+json` selon la RFC 9457. Les clés textuelles sont échappées ; les clés binaires sont encodées en Base64 avec indication de l’encodage.

`none` supprime le corps PURGE : une opération réussie et terminée renvoie `204`, un travail accepté en arrière-plan conserve `202` et les erreurs conservent 4xx/5xx. État et REFRESH gardent leur JSON structuré. Voir le [contrat HTTP](../HTTP_RESPONSES.md) pour les codes, champs et limites.

<a id="cache_purge_background_queue"></a>

### `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

Active le traitement différé des jokers et de `purge_all`. Renvoie `202 Accepted` après mise en file ou regroupement avec une tâche identique en attente/active. Les purges exactes ne changent pas.

Cette option contrôle les purges historiques par joker/totales. Une liaison `cache_purge_key` utilise toujours nettoyage différé et réponse logique `200`, même option désactivée.

File pleine ou allocation échouée maintient la requête ouverte jusqu’à admission, sans parcours pendant l’attente. Un doublon peut être admis à capacité maximale. Échouer à allouer le petit état d’attente renvoie `500`. Le nettoyage de déconnexion annule le temporisateur et libère la jauge d’attente.

Pour cet indicateur et `cache_purge_vary_aware`, la dernière valeur déclarée prévaut. Les valeurs invalides sont rejetées. `cache_purge_legacy_status` est obsolète et ignoré. Threads et cycle de vie : [modèle d’exécution](#execution-model).

<a id="cache_purge_queue_size"></a>

### `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

Maximum de tâches inachevées sur toutes les racines. Attentes et tâches attribuées consomment la capacité. Les doublons partagent une allocation ; une répétition conserve allocation et place. Les tâches distinctes attendent une place libre.

La zone partagée est créée avec le traitement de fond ou le statut. Changer la capacité exige un redémarrage même si la taille arrondie reste identique. Reload refuse le redimensionnement pour préserver le travail. Ajouter des racines peut aussi nécessiter mémoire et redémarrage. Drainez d’abord les tâches devant finir.

Déclarer un espace de noms crée également la file.

La zone inclut tâches, métadonnées de chemins et métriques alignées des workers. Elle dépend de la compilation et des chemins ; voir [dimensionnement mémoire](#memory-sizing).

<a id="cache_purge_thread_pool"></a>

### `cache_purge_thread_pool`

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

### `cache_purge_batch_size`

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

### `cache_purge_throttle_ms`

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

### `cache_purge_queue_status`

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

`pending.full` compte les purges complètes. Le joker de clé `*` est une autre opération. `active` comprend les attentes de pool ; exclu de `queue.size` et `pending.total`, il consomme néanmoins la capacité.

Pour un balayage regroupé, `pending.full` reflète la classification de la première tâche admise. Scans/sondes REFRESH ne comptent ni dans la profondeur de file ni dans les erreurs de suppression. Une reprise différée déplacée peut faire de `wait_time` l’âge de la tête échantillonnée plutôt que l’attente maximale.

Chaque slot de processus a un écrivain dans la boucle ; débuts et pas sont alignés sur les lignes de cache. Les threads retournent par événements de fin. Le lecteur ne prend ni mutex ni atomic fetch-add, même avec zéro. En 64 bits, lectures/écritures volatile alignées ordinaires ; en 32 bits, contrôles de séquence bornés protégeant les lectures 64 bits, avec omission possible d’un segment occupé. Les champs sont approximatifs, non transactionnels entre workers. L’admission utilise l’état de contrôle verrouillé, jamais les métriques.

Les cumuls saturent au maximum unsigned 64 bits, persistent au reload et repartent à zéro au redémarrage. La jauge d’attente suit le nettoyage des requêtes. Opérations synchrones et suppressions du cache manager sont exclues. Les octets estiment des longueurs, pas les blocs ni l’espace récupéré : lecteurs ouverts et écritures concurrentes influent. Les résultats partiels comptent ; ceux d’un worker planté peuvent manquer. Ce n’est pas un journal d’audit.

HIT/MISS compte uniquement les requêtes principales avec décision native. Upstream absent, statut nul, BYPASS, EXPIRED, STALE, UPDATING et REVALIDATED sont exclus du ratio. Une requête normalement non cachée ne devient pas MISS. La supervision seule peut créer SHM sans purge de fond. En 32 bits, l’âge couvre moins d’environ 24,9 jours ; une différence d’horloge signée négative est ramenée à zéro.

<a id="cache_purge_legacy_status"></a>

### `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

Directive obsolète, acceptée uniquement pour la migration des configurations. Les deux valeurs produisent un avertissement et sont ignorées ; supprimez la directive. Les valeurs invalides sont rejetées. Une absence de correspondance synchrone renvoie `200 no_match`, un nettoyage physique accepté `202 accepted`, une invalidation de génération publiée `200 invalidated`. Le choix entre `412` et `404` disparaît. Voir le [contrat HTTP](../HTTP_RESPONSES.md).

<a id="cache_purge_vary_aware"></a>

### `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

Avec `on`, une purge exacte réussie supprime le fichier principal puis parcourt le cache pour supprimer les fichiers portant la même chaîne `KEY:`. Cela couvre `Vary` et `gzip_vary`, sur des chemins distincts partageant la clé logique. L’octet suivant doit être `\n`, évitant une simple correspondance de préfixe.

Désactivé par défaut car chaque purge exacte ajoute un parcours complet. À activer pour plusieurs représentations `Vary`/`gzip_vary`. Le parcours a aussi lieu si l’entrée principale est déjà absente. Les jokers et `purge_all` couvrent déjà les variantes. Les clés Vary exactes sont sensibles à la casse et limitées à 511 octets ; au-delà, `400 invalid_selector` est renvoyé avant toute suppression.

L’invalidation par générations couvre toutes les représentations de la clé sans cette option.

<a id="cache_purge_namespace"></a>

### `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

Déclare un espace de générations avant ses liaisons aux locations. Nom de 1–32 lettres ASCII, chiffres, `_` ou `-` ; SHM d’au moins huit pages natives. Choisissez un répertoire d’état persistant dédié hors racines de cache et temporaires. Le noyau crée un marqueur persistant exclusif pour allouer une époque de redémarrage. Gardez ces petits fichiers tant que le cache associé peut exister : supprimer l’état seul permet de réutiliser une époque obsolète. Aucune garantie de durabilité coupure électrique/fsync. Reload préserve époque et règles ; changer taille/chemin ou retirer l’espace impose un redémarrage. Espace et file ont des tailles SHM séparées.

<a id="cache_purge_key"></a>

### `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

Lie le location à un espace existant et expose `$cache_purge_key`. Cette variable doit être la totalité de `proxy_cache_key`, `fastcgi_cache_key`, `scgi_cache_key` ou `uwsgi_cache_key`, y compris dans le location PURGE distinct. Une clé native développée différente de la clé générée fait échouer PURGE avec `500`, sans prétendre à une invalidation stricte. Ne référencez pas `$cache_purge_key` dans sa propre expression. Maximum logique : 8096 octets. Ne liez que les locations participants si les autres gardent l’ancien fonctionnement ; aucun `off` n’annule une liaison héritée.

<a id="proxy_cache_refresh"></a>

### `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

Crée un endpoint REFRESH explicite réservé aux requêtes principales ; autres méthodes : `405`. Nécessite liaison `cache_purge_key`, expression native `$cache_purge_key` et profil refresh. La clé logique développée sélectionne une clé exacte, un préfixe terminé par `*` ou `*` pour l’espace sur ce chemin. Protégez le location par accès/authentification natifs. REFRESH ne couvre que proxy ; PURGE conserve les quatre protocoles.

<a id="cache_purge_refresh_profile"></a>

### `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

Associe une représentation connue à un location proxy interne littéral. Retirer le préfixe exact sensible à la casse doit laisser une URI d’origine commençant par `/`, paramètres et échappements conservés. Le module ne devine pas l’hôte et ne cherche pas `://` dans la clé. Le destinataire doit avoir `internal`, cache désactivé, transmission des en-têtes/corps désactivée, méthode HEAD, Host littéral non vide et en-têtes de représentation littéraux. Les deux en-têtes conditionnels utilisent exactement `$cache_purge_refresh_etag` et `$cache_purge_refresh_last_modified` ; `$cache_purge_refresh_uri` fournit l’URI à `proxy_pass`. Ces trois variables pures ne sont pertinentes que dans les sondes. Les vérifications passent en preaccess, redirections internes comprises. L’administrateur garantit la représentation d’origine correspondant à la clé ; Vary est ignoré, faute d’en-têtes originaux dans les métadonnées natives.

Configurez `proxy_intercept_errors off` dans la sonde ; une interception héritée est rejetée afin qu’une erreur d’origine ne devienne pas un succès invalidant.

<a id="cache_purge_refresh_timeout"></a>

### `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

Borne l’envoi de nouveaux scans/sondes par page, attente du scanner comprise. À expiration, le nouveau travail cesse et la réponse devient incomplète. Un HEAD actif termine selon `proxy_connect_timeout`, `proxy_send_timeout` et `proxy_read_timeout`, à configurer explicitement. Ce n’est ni une échéance absolue de réponse ni une interruption de syscall. Déconnexion et arrêt gracieux cessent les envois, en gardant la mémoire du thread actif jusqu’à sa fin native.

---

<a id="execution-model"></a>

## Modèle d’exécution

Une **tâche** est une opération joker ou `purge_all`. Un **lot** compte les tâches traitées par un worker avant une pause. Une **tranche** est une étape bornée du parcours incrémental ; une tâche peut en nécessiter plusieurs.

Avec les générations, un balayage physique par chemin et espace de noms regroupe les invalidations acceptées. Les pages REFRESH sont des opérations HTTP distinctes, pas des tâches de la file de purge.

| Requête et configuration | Parcours des fichiers | Fin HTTP |
|---|---|---|
| Clé exacte historique, avec ou sans Vary | Worker de la requête ; Vary ajoute un parcours | Après l’opération |
| Joker ou `purge_all`, file désactivée | Worker de la requête, parcours synchrone | Après le parcours |
| Tâche en arrière-plan, pool choisi | Une tâche de pool par worker à la fois | `202` après admission |
| Tâche en arrière-plan sans pool utilisable | Boucle du worker, tranches incrémentales | `202` après mise en file |
| Capacité/allocation épuisée | Attente d’admission, sans parcours lié à la requête | `202` après admission |
| PURGE par générations, toute clé | Balayage différé de l’espace ; aucune ouverture/suppression de cache dans la requête | `200 invalidated` après publication et admission |
| Endpoint REFRESH | Balayage borné et jusqu’à 32 sondes HEAD séquentielles | JSON `200` avec fin et continuation |

Les lignes sans générations décrivent le fonctionnement historique. Une requête par générations attendant de la capacité renvoie `200` après admission, pas `202`.

Une tranche s’arrête après au plus 64 itérations ou dès qu’elle observe 5 ms écoulées, entrées ignorées et fins de répertoire comprises. Cela ne borne pas la durée d’un appel système isolé.

Pour transférer les E/S de fond aux threads, compilez avec `--with-threads` et déclarez un pool du noyau en contexte **main**, hors de `http` :

```nginx
thread_pool default threads=2;
```

`cache_purge_thread_pool <name>` choisit un pool existant dans `http`, `server` ou `location`. Par défaut, `default` est créé par NGINX avec ses paramètres standards s’il est référencé sans déclaration. Les autres noms nécessitent `thread_pool`. Le choix est hérité ; la première requête admise choisit l’exécuteur du travail regroupé. Chaque worker a ses pools. Un pool plein entraîne un nouvel envoi après 10 ms en conservant la place d’admission, jamais un parcours synchrone. Sans threads, sous Windows, en mode monoprocessus ou après suppression au reload du pool d’une ancienne tâche, le curseur incrémental est utilisé.

REFRESH utilise ce pool pour un scanner supplémentaire par worker. Les sous-requêtes HTTP natives s’exécutent dans la boucle ; le thread d’E/S n’accède jamais à leur état.

File et index utilisent le mutex slab pour leurs modifications structurelles. Admission et callbacks de fond utilisent trylock et réessaient par temporisateur. Tâches en attente **et actives** restent dans une rbtree de chaînes du noyau par chemin ; la comparaison des clés complètes résout les collisions. L’index coûte `O(log Q)` pour `Q` tâches inachevées ; chercher le chemin est linéaire dans le nombre de chemins enregistrés. Des listes `ngx_queue_t` suivent attente et activité. Le premier doublon actif demande une répétition, les suivants s’y regroupent. Après le passage actuel, elle rejoint la fin de file pour ne pas perdre une purge ultérieure. Des purges continues peuvent provoquer d’autres passages. La place n’est libérée qu’à la fin définitive. E/S et formatage des réponses restent hors verrou.

La rbtree ne déduplique que le nettoyage physique. Les règles de génération occupent des tries d’octets bornés dans une autre zone SHM. Un balayage strict a une identité d’index différente d’une tâche historique ; les décisions REFRESH empruntent le même nettoyage.

File pleine ou allocation impossible laisse la requête attendre avec temporisateur de 10 ms et gestion native de déconnexion. Requêtes et handlers de nettoyage restent dans leur worker ; aucun pointeur de requête en SHM. La lecture globale HTTP/2 et HTTP/3 reste active. La pression ne produit pas de 429/503 ; un petit PURGE déjà lu ne garantit pas TCP Zero Window. Les attentes consomment mémoire et connexions : utilisez les délais et politiques d’admission natifs pour votre budget.

Les tâches admises gardent clés, pools et protections temporaires lors des reloads ordinaires. Un doublon plus récent actualise la protection de l’attente ou de la prochaine répétition. Désactiver l’admission conserve les tâches admises. Un worker en arrêt gracieux termine son passage sans nouvelle tâche. Un arrêt rapide remet la tâche incrémentale inachevée en file sans terminer le parcours dans le hook de sortie. L’arrêt natif du pool gouverne la durée de vie des tâches déjà soumises.

Des époques PID/slot identifient les attributions. Chaque seconde, les workers vérifient au plus 64 entrées actives par passage et réaffectent les tâches du propriétaire terminé. Cela récupère les tâches abandonnées, sans journal transactionnel. Un crash pendant une modification de liste ou slab peut corrompre l’état et imposer un redémarrage ; des suppressions précédant le crash peuvent manquer dans les métriques. Attentes et répétitions nécessitent un worker actif : videz la file avant arrêt si leur achèvement importe. La file n’est pas durable.

Les structures internes ne forment pas une ABI de mise à niveau. Une compilation de structure différente impose un redémarrage complet, après drainage des tâches devant finir. Un reload de la même compilation réutilise la zone.

La suppression est best effort. `202` confirme l’admission, pas la suppression ; un passage terminé n’est pas un instantané atomique. Des requêtes concurrentes peuvent créer des entrées pendant ou après ; les erreurs sont journalisées. Jokers et purge complète suppriment sans actualiser synchroniquement chaque nœud natif. Purges exactes et Vary invalident les nœuds correspondants.

Un passage échoué est réessayé après 100 ms, 1 s et 10 s, au plus trois fois. Les tâches différées gardent leur place d’admission/déduplication tout en laissant avancer les autres. Un nouveau PURGE réinitialise cette fenêtre. Après épuisement, la tâche est retirée avec erreur journalisée ; consultez `cleanup.exhausted` et réessayez après réparation du stockage. L’invalidation reste effective malgré l’échec physique. Les fichiers absents sont des courses attendues ; les fichiers mal formés sont ignorés sans répétition infinie.

---

<a id="performance-tuning"></a>

## Réglage des performances

`queue_size` borne le travail inachevé, `batch_size` groupe les tâches par worker et `throttle_ms` espace les lots. Mesurez latence, contention disque et âge de file avant/après réglage. Une capacité accrue n’augmente pas le débit de drainage.

Ces préréglages sont des points de départ, pas des performances matérielles annoncées. Les colonnes omettent le préfixe `cache_purge_`.

| Charge / budget de stockage | `queue_size` | `batch_size` | `throttle_ms` |
|---|---:|---:|---:|
| Hôte ou stockage limité | 512 | 5 | 25ms |
| Base générale | 2048 | 20 | 10ms |
| Stockage rapide avec marge mesurée | 8192 | 50 | 5ms |
| Courtes pointes, drainage prudent | 8192 | 5 | 50ms |

Par exemple, le préréglage général :

```nginx
cache_purge_background_queue on;
cache_purge_queue_size        2048;
cache_purge_batch_size        20;
cache_purge_throttle_ms       10ms;
```

Une grande file absorbe une pointe, mais finit par saturer si les arrivées dépassent durablement les fins. Estimez la marge supplémentaire par `max(0, arrival_rate - completion_rate) * burst_seconds` tâches, avec des taux après déduplication en tâches/s. Prévoyez durées inégales et arriéré existant. Attendre l’admission ajoute requêtes/connexions ouvertes : budgétez mémoire et descripteurs quand le stockage sature.

<a id="memory-sizing"></a>

### Dimensionnement mémoire

La taille configurée découle des structures natives :

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

| Composant | Mémoire réservée ou utilisée |
|---|---|
| Réserve de tâches | `Q * S`, avec arrondi slab et noms clé/pool intégrés |
| Métriques d’un chemin | `R * NGX_MAX_PROCESSES`, plus alignement ; 192 KiB pour pas de 192 octets et 1024 slots |
| Contrôle de file | `H`, dont propriétaires de processus et tableaux d’époques |
| Réserve des chemins | `M`, dont noms, racines rbtree, alignement et métriques |
| Zone minimale | Huit pages natives |

Avec la structure LP64 mesurée (`I=184`, `H=20576`, chemin 104 octets, statistiques 144), ligne de cache 64 octets, pages 4 KiB et un chemin de 20 octets :

| Capacité | Octets de zone | MiB de zone |
|---:|---:|---:|
| 512 | 1654784 | 1.58 |
| 2048 | 5373952 | 5.12 |
| 8192 | 20242432 | 19.30 |

Une tâche occupe `I + index_bytes + 1 + pool_name_bytes` avant arrondi slab. L’index intégré contient un octet d’action puis la clé historique ou le nom d’espace ; `purge_all` historique garde seulement son marqueur. La capacité borne attentes et actives ensemble. Structures natives, ligne de cache, `NGX_MAX_PROCESSES` et chemins déterminent le résultat ; Windows et 32 bits diffèrent. La protection de reload consomme davantage de SHM. L’allocation peut échouer avant la limite de tâches.

La mémoire de processus est distincte. Chaque parcours actif utilise un tampon fixe de clé, un tampon réutilisable de chemin et une frame fixe par répertoire ouvert. Les frames gardent des longueurs sans recopier les ancêtres : `O(depth + longest_path)`, indépendant du nombre de fichiers. Les flux de répertoire ont leurs tampons de plateforme. Les requêtes attendant gardent un petit contexte temporisateur/nettoyage sans curseur. Piles de threads et pools natifs coûtent séparément.

L’espace de noms a sa propre taille SHM. Un scanner REFRESH réserve un tampon/contexte LP64 fixe de 283864 octets (32 entrées), plus frames et au plus 32 sous-requêtes du pool HTTP. Finir une page libère la requête ; continuer en crée une autre. Les attentes supplémentaires n’allouent pas de tableaux de candidats.

<a id="throughput-and-latency"></a>

### Débit et latence

Pour un worker occupé en continu, approximation utile :

```text
B = tasks completed in a batch
T = throttle delay in milliseconds
D = elapsed batch processing time in milliseconds, including slice waits
rate_per_worker ~= 1000 * B / (D + T) tasks/s
```

Omettre `D` donne le plafond de planification `1000 * B / T`, soit 1000 tâches/s par worker par défaut. Ce n’est ni une mesure ni un débit de fichiers supprimés. Dix tâches totalisant 200 ms avec pause de 10 ms donnent environ 47,6 tâches/s. Le stockage est partagé : multiplier par les workers ne garantit pas le débit. Un worker inactif peut attendre `10 * T` plus le retard de boucle avant de rechercher du travail.

Réduisez le lot ou augmentez la pause pour limiter le travail entre repos. Le pool facultatif réduit la latence du worker pendant l’E/S. Aucun réglage ne borne une syscall ni ne découpe une tâche threadée en lots.

---

<a id="monitoring-and-debugging"></a>

## Supervision et débogage

L’[endpoint de statut](#cache_purge_queue_status) montre attente, activité, admission, doublons, suppressions et HIT/MISS. `202 Accepted` confirme admission/regroupement, pas suppression ni invalidation logique immédiate. Surveillez latence, âge de file et contention de stockage ensemble.

| Niveau | Condition |
|---|---|
| `warn` | Clé trop longue, pool supprimé ou valeur plafonnée |
| `error` | Accès refusé, recherche cache/profil REFRESH échoué, reprises épuisées ; pool natif ou lecture Windows échouée |
| `crit` | Échec ouverture/stat/suppression ou répertoire ; lecture POSIX native échouée |
| `alert` | Échec de fermeture de fichier |
| `emerg` | Configuration invalide, SHM incompatible, redimensionnement ou allocation de métadonnées de reload échouée |
| `debug` | Contexte supplémentaire de requête et parcours |

Les diagnostics utilisent les fonctions natives NGINX et le préfixe `ngx_cache_purge: `. NGINX ajoute niveau, date, IDs processus/thread, texte errno et contexte requête/configuration. Les messages de fichiers nomment l’opération réelle et citent le chemin ; espace de noms et REFRESH indiquent objet et motif du refus. Utilisez un parseur standard du journal NGINX et filtrez le préfixe.

Le débogage utilise `NGX_LOG_DEBUG_HTTP` et exige compilation `--with-debug` et `error_log /var/log/nginx/error.log debug;`.

La pression de file apparaît en métriques d’attente, sans avertissement par reprise. Un fichier disparu est une course attendue, sans diagnostic du module. Les auxiliaires natifs — parseur, allocateur, E/S, pools — gardent leur format ; examinez le journal complet lors d’un échec.

```bash
tail -f /var/log/nginx/error.log | grep 'ngx_cache_purge:'
```

Voir les [contrôles de journalisation](../../t/TESTING.md#logging-contracts) pour les diagnostics testés.

---

<a id="troubleshooting"></a>

## Dépannage

| Symptôme | Vérification ou réglage |
|---|---|
| Latence du worker lors de grands parcours | Activez file et pool facultatif pour E/S. Exact/Vary et file désactivée restent synchrones. |
| Attentes d’admission croissantes | Vérifiez arrivées, âge, pool et disque. Plus de capacité absorbe les pointes ; redimensionner exige redémarrage. |
| iowait élevé | Réduisez `cache_purge_batch_size`, augmentez `cache_purge_throttle_ms`, limitez les purges entrantes. |
| Drainage lent | Mesurez durée et contention avant d’augmenter le lot ou réduire la pause. |
| Le client attend les anciens `412`/`404` | Supprimez `cache_purge_legacy_status` ; traitez `200` avec `Cache-Purge-Code: no_match`. |
| Variantes Vary restantes | Activez `cache_purge_vary_aware` ; vérifiez résultat principal et limite de 511 octets. |
| Travail admis apparemment incomplet | Vérifiez activité, récupération et erreurs. `202` n’est pas un reçu de fin ; une corruption SHM suspectée impose un redémarrage. |

---

<a id="testing"></a>

## Tests

La [carte de couverture](../TEST_COVERAGE.md) (en anglais) distingue les cas testés des lacunes restantes.

Compilez et exécutez la suite complète dans Docker :

```sh
make test-all
NGINX_VERSION=1.31.6 make test-all
```

Depuis `t/`, les services Compose existants restent disponibles :

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

Avec le plugin Compose, utilisez `docker compose` à la place de `docker-compose` ; les Makefiles détectent les deux. `nginx-test` et `make test-all` lancent les sept suites `t/*.t` via le même `prove` qu’en CI. L’image d’intégration inclut contrôles natifs des auxiliaires/structures et exécution en pool. Les compilations strictes sans threads testent l’incrémental. Chaque test échoué fait échouer la commande.

Voir [`t/TESTING.md`](../../t/TESTING.md) pour compilations strictes, sanitizers, Valgrind, plateformes et rapports. Tous les jobs GitHub sont dans `ci.yml`.

---

<a id="migration"></a>

## Migration

<a id="queue-and-status-changes"></a>

### Évolution de la file et du statut

Les changements de structure SHM nécessitent un redémarrage complet. La syntaxe méthode/ACL et les gestionnaires de protocole sont conservés. v4 utilise JSON par défaut, des codes identiques dans l’en-tête et le corps, et `200 no_match` en l’absence d’entrée. `cache_purge_legacy_status` ne produit qu’un avertissement. Adaptez les clients au [contrat HTTP](../HTTP_RESPONSES.md). La capacité inclut les tâches affectées ; la surcharge attend l’admission. Les doublons actifs peuvent programmer un nouveau passage.

Le statut ajoute des champs sans supprimer les anciens noms. `requests.overflow` compte l’attente d’admission. Les métriques sont approximatives, cumulées sur 64 bits. Avec threads, référencer `default` l’enregistre automatiquement ; un pool nommé isole l’E/S. Une configuration de statut seule alloue SHM.

Générations et REFRESH exigent une activation explicite en v4.0. Liez `cache_purge_key` et utilisez `$cache_purge_key` comme totalité de la clé native dans chaque location participant. Les noms physiques changent ; les anciens fichiers deviennent froids et attendent expiration native ou nettoyage historique explicite. Conservez les marqueurs avec le cache. Les anciens locations PURGE ne gagnent pas REFRESH. Les utilisateurs du PR60 expérimental doivent configurer endpoint/profil et paginer ; réponse illimitée, déduction URI/Host et extension implicite des méthodes de PR60 ne sont pas des garanties de compatibilité.

<a id="from-the-original-frickle-module"></a>

### Depuis le module FRiCKLE original

Les formes méthode/liste d’accès et zone/clé restent disponibles. Les entrées absentes renvoient `200 no_match` ; supprimez `cache_purge_legacy_status` et adaptez les clients. Pour activer l’arrière-plan, ajoutez :

```nginx
http {
    cache_purge_background_queue on;
}
```

<a id="from-nginx-selective-cache-purge-module"></a>

### Depuis nginx-selective-cache-purge-module

Remplacez le module : la file est intégrée. Reportez vos paramètres vers `cache_purge_queue_size`, `cache_purge_batch_size` et `cache_purge_throttle_ms`.

---

<a id="temporary-directories"></a>

## Répertoires temporaires

Les parcours ignorent les temporaires configurés sous la racine, en comparant des composants complets. Les noms tels que `proxy_temp` ne sont pas réservés sans configuration temporaire. Seuls les fichiers réguliers de nom exactement 32 caractères hexadécimaux sont candidats. Les temporaires numériques ou suffixés sont ignorés, y compris avec `use_temp_path=off`.

Chaque parcours valide version native d’en-tête, offsets, marqueur `KEY: ` complet, CRC32 et hash clé/variante du nom, avec plafond de clé de 8192 octets. Fichiers mal formés, tronqués, d’une autre version ou incohérents sont ignorés même par purge-all. Les longueurs ETag/Vary égales au maximum natif sont acceptées. Valider le format n’authentifie pas un fichier malveillant.

La racine peut être un lien symbolique ou une jonction Windows ; les liens/reparse points descendants sont ignorés. En POSIX avec détection complète des fonctions relatives à un descripteur, les enfants utilisent `ngx_openat_file`/`ngx_file_at_info` natifs avec no-follow ; suppression relative au parent maintenu ouvert. Cela empêche la redirection par remplacement d’un lien descendant, mais pas le déplacement d’un répertoire déjà ouvert. Windows et anciens Unix utilisent chemins complets et vérifications de liens, avec une course de remplacement restante. Recherche/suppression exacte historique utilise aussi les chemins complets. Gardez les racines privées, sans écrivains non fiables.

Écrivez les chemins canoniques de façon cohérente : les comparaisons ne résolvent pas les alias de liens, montages ou Unicode. Les tâches conservent leur protection ; un doublon récent actualise l’attente ou la prochaine répétition. Le passage actif garde l’ancienne ; ce n’est pas l’union de toutes les générations vivantes de workers. Ne réaffectez pas les chemins tant que des tâches concernées attendent ou s’exécutent.

---

<a id="security"></a>

## Sécurité

- Restreignez les endpoints de purge avec `allow` / `deny` ou authentification en amont. Ne les exposez jamais publiquement.
- Dimensionnez `cache_purge_queue_size` selon le trafic pour borner SHM sous afflux de requêtes.
- Envisagez `limit_req` pour limiter les purges indépendamment du trafic normal.

---

<a id="license"></a>

## Licence

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

## Voir aussi

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [Documentation nginx](https://nginx.org/en/docs/)
