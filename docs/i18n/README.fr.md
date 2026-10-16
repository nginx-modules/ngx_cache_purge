

<a id="contents"></a>

## Sommaire

- [Fonctionnalités](#features)
- [Compatibilité](#compatibility)
- [Paquets des distributions](#distribution-packages)
- [Installation](#installation)
- [Choisir le comportement adapté](#choosing-a-mode)
- [Utilisation](#usage)
- [Purge par clé partielle](#partial-key-purge)
- [Invalidation par générations](#generation-invalidation)
- **REFRESH** — [Vérifier le cache auprès de l’origine (REFRESH)](#conditional-refresh)
- [Sécurité](#security)
- [Migration](#migration)
- [Exemples de configuration traduits](#localized-examples)
- [Documentation](#documentation)
- [Voir aussi](#see-also)
- [Licence](#license)

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

<a id="compatibility"></a>

## Compatibilité

En production, utilisez une version de NGINX encore maintenue. POSIX/UNIX est la plateforme principale ; Windows reste expérimental. Les threads sont facultatifs. [NGINX / CI](../../t/TESTING.md#compatibility-matrix).

<a id="distribution-packages"></a>

## Paquets des distributions

Plusieurs distributions et projets de ports proposent ce fork, mais pas nécessairement en v4. Avant l’installation, vérifiez la version du paquet et l’ABI de NGINX. Consultez les [paquets et leurs sources](../DISTRIBUTIONS.md).

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
./configure --with-compat --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

Utilisez les sources de la version exacte de NGINX installée sur le serveur. `--with-compat` ne garantit pas la compatibilité entre toutes les versions de NGINX ou bibliothèques C. Voir [les modules dynamiques et les archives de publication](../BINARY_MODULES.md) (en anglais).

<a id="choosing-a-mode"></a>

## Choisir le comportement adapté

| Objectif | Configuration | Sens de la réponse |
|---|---|---|
| Supprimer des entrées périmées et attendre la tentative de suppression | PURGE, file d’attente désactivée | `200 purged` ou `200 no_match` ; 4xx/5xx en cas d’erreur |
| Planifier une purge par préfixe ou de tout le cache | PURGE avec `cache_purge_background_queue on` | `202 accepted` ; les anciennes entrées peuvent encore être servies en attendant |
| Empêcher toute nouvelle requête d’utiliser une ancienne entrée, même écrite tardivement | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated` ; nouvelle génération de clé, puis suppression des fichiers |
| Vérifier si le contenu a changé à l’origine | Générations + `proxy_cache_refresh` + profil interne | `200` avec résultats par entrée et continuation ; aucun préchauffage |

Ces mécanismes se combinent : une invalidation par génération programme aussi le nettoyage. La file seule ne protège pas des écritures tardives. Voir l’[exemple v4](#localized-examples), les [exemples PURGE](#localized-examples) et les [réponses HTTP](../HTTP_RESPONSES.md) (en anglais).

<a id="usage"></a>

## Utilisation

[→ Exemples de configuration traduits](#localized-examples)


<a id="partial-key-purge"></a>

## Purge par clé partielle

Ajoutez `*` à la **clé de cache évaluée** pour rechercher un préfixe sans tenir compte de la casse. Les clés exactes et Vary restent sensibles à la casse. Un préfixe de nettoyage physique accepte 511 octets au maximum, hors `*` final. Au-delà, un message est journalisé et `400 invalid_selector` est renvoyé sans parcours des fichiers.

Avec `cache_purge_key`, la comparaison porte sur l’expression logique, jusqu’à 8096 octets, `*` final compris.

Pour une expression finissant par `$uri`, cette requête choisit un préfixe URI :

```http
PURGE /images/header*
```

L’astérisque doit terminer la clé évaluée, pas seulement le chemin URI. Tenez compte des paramètres et suffixes variables de `proxy_cache_key`. Une clé limitée à `*` parcourt toutes les entrées admissibles.

<a id="generation-invalidation"></a>

## Invalidation par générations

Les générations de clés empêchent les nouvelles requêtes de retrouver un ancien contenu après PURGE, même si une réponse est écrite plus tard. Les fichiers sont supprimés séparément. Un espace de noms partage les règles d’invalidation ; conservez son répertoire d’état entre les redémarrages.

<a id="conditional-refresh"></a>

## Vérifier le cache auprès de l’origine (REFRESH)

REFRESH envoie HEAD pour la même représentation à l’origine, avec les valeurs ETag et Last-Modified conservées. Si la fiche produit est déjà obsolète, utilisez PURGE. REFRESH ne télécharge pas de nouveau corps, ne remplit pas un cache vide et ne prolonge pas le TTL. [HTTP / REFRESH](../REFRESH.md).

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

<a id="security"></a>

## Sécurité

- Restreignez les endpoints de purge avec `allow` / `deny` ou authentification en amont. Ne les exposez jamais publiquement.
- Dimensionnez `cache_purge_queue_size` selon le trafic pour borner SHM sous afflux de requêtes.
- Envisagez `limit_req` pour limiter les purges indépendamment du trafic normal.

<a id="migration"></a>

## Migration

Les changements de structure SHM nécessitent un redémarrage complet. La syntaxe méthode/ACL et les gestionnaires de protocole sont conservés. v4 utilise JSON par défaut, des codes identiques dans l’en-tête et le corps, et `200 no_match` en l’absence d’entrée. `cache_purge_legacy_status` ne produit qu’un avertissement. Adaptez les clients au [contrat HTTP](../HTTP_RESPONSES.md). La capacité inclut les tâches affectées ; la surcharge attend l’admission. Les doublons actifs peuvent programmer un nouveau passage.

<a id="localized-examples"></a>

## Exemples de configuration traduits

Les exemples complets couvrent le catalogue avec REFRESH et les configurations PURGE classiques. [Exemples de configuration en anglais](../EXAMPLES.md).

<details>
<summary>Exemples de configuration traduits — 7 exemples</summary>

<a id="generation-and-refresh-example"></a>

### Exemple : catalogue de produits

Cet exemple complet écoute localement sur le port 8080 et transmet les requêtes à l’application du port 8081. Celle-ci doit servir `/products/42` avec Host `shop.example.com`. Adaptez l’adresse, le domaine et les répertoires de cache et d’état accessibles en écriture.

`shop.example.com` est le domaine de l’en-tête HTTP Host, pas un répertoire. `/products/42` est l’URI du produit. `catalog:` est un préfixe fixe de clé, pas un nom d’hôte. Le préfixe et l’URI complète, paramètres compris, forment la clé logique, par exemple `catalog:/products/42`. Le profil REFRESH retire `catalog:` et vérifie `/products/42` dans la même application.

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

Demandez par GET un produit existant et pouvant être mis en cache jusqu’à obtenir `X-Cache-Status: HIT`, puis envoyez REFRESH :

```sh
curl -i 'http://127.0.0.1:8080/products/42'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/42' \
  'http://127.0.0.1:8080/admin/refresh'
```

Si le produit a changé, envoyez PURGE. Ici, `200 invalidated` empêche les nouvelles requêtes d’utiliser l’ancienne génération ; le GET suivant interroge l’application.

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/products/42'
```

Pour vérifier tous les produits, utilisez `catalog:/products/*`. Si `complete: false`, gardez le sélecteur et remplacez `<next_cursor>` par la valeur reçue. Corrigez les erreurs HTTP avant de continuer.

```sh
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

<a id="sample-configurations"></a>

### Exemples de configuration

Les cinq premiers blocs sont des exemples complets et indépendants de `nginx.conf`. Le dernier ajoute la gestion de Vary à une section `http` existante. NGINX reçoit les connexions locales sur le port 8080 ; `catalog_backend` désigne le groupe de serveurs de l’application à l’adresse `127.0.0.1:8081`. `shop.example.com` est le HTTP Host envoyé à l’application. Adaptez les adresses et utilisez un répertoire de cache accessible en écriture par NGINX.

<a id="basic--inline-purge-method"></a>

#### Base — méthode de purge intégrée

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

#### Base — location de purge distinct

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

#### File en arrière-plan activée

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

#### Purger toutes les entrées

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

#### Réponses JSON avec réglage par location

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

#### Purge tenant compte de Vary

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

Après purge d’une clé, ses variantes gzip et Vary sur d’autres chemins sont également supprimées automatiquement.

</details>

<a id="documentation"></a>

## Documentation

- [Référence des directives](DIRECTIVES.fr.md)
- [Autres exemples de configuration](#localized-examples)
- [Exploitation et dépannage](OPERATIONS.fr.md)
- [HTTP et limites de REFRESH (anglais)](../REFRESH.md)
- [Migration vers v4](MIGRATION.fr.md)
- [Architecture de l’implémentation (anglais)](../ARCHITECTURE.md)
- [Tests](OPERATIONS.fr.md#testing)
- [Développement et tests (anglais)](../../t/TESTING.md)

<a id="see-also"></a>

## Voir aussi

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [Documentation nginx](https://nginx.org/en/docs/)

<a id="license"></a>

## Licence

```
Copyright (c) 2009-2014, FRiCKLE <info@frickle.com>
Copyright (c) 2009-2014, Piotr Sikora <piotr.sikora@frickle.com>
Copyright (C) 2016-2026, Denys Denysov

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
