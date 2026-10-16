# ngx_cache_purge

[README](README.fr.md) · [Directives](DIRECTIVES.fr.md)

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

L’[endpoint de statut](DIRECTIVES.fr.md#cache_purge_queue_status) montre attente, activité, admission, doublons, suppressions et HIT/MISS. `202 Accepted` confirme admission/regroupement, pas suppression ni invalidation logique immédiate. Surveillez latence, âge de file et contention de stockage ensemble.

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

<a id="temporary-directories"></a>

## Répertoires temporaires

Les parcours ignorent les temporaires configurés sous la racine, en comparant des composants complets. Les noms tels que `proxy_temp` ne sont pas réservés sans configuration temporaire. Seuls les fichiers réguliers de nom exactement 32 caractères hexadécimaux sont candidats. Les temporaires numériques ou suffixés sont ignorés, y compris avec `use_temp_path=off`.

Chaque parcours valide version native d’en-tête, offsets, marqueur `KEY: ` complet, CRC32 et hash clé/variante du nom, avec plafond de clé de 8192 octets. Fichiers mal formés, tronqués, d’une autre version ou incohérents sont ignorés même par purge-all. Les longueurs ETag/Vary égales au maximum natif sont acceptées. Valider le format n’authentifie pas un fichier malveillant.

La racine peut être un lien symbolique ou une jonction Windows ; les liens/reparse points descendants sont ignorés. En POSIX avec détection complète des fonctions relatives à un descripteur, les enfants utilisent `ngx_openat_file`/`ngx_file_at_info` natifs avec no-follow ; suppression relative au parent maintenu ouvert. Cela empêche la redirection par remplacement d’un lien descendant, mais pas le déplacement d’un répertoire déjà ouvert. Windows et anciens Unix utilisent chemins complets et vérifications de liens, avec une course de remplacement restante. Recherche/suppression exacte historique utilise aussi les chemins complets. Gardez les racines privées, sans écrivains non fiables.

Écrivez les chemins canoniques de façon cohérente : les comparaisons ne résolvent pas les alias de liens, montages ou Unicode. Les tâches conservent leur protection ; un doublon récent actualise l’attente ou la prochaine répétition. Le passage actif garde l’ancienne ; ce n’est pas l’union de toutes les générations vivantes de workers. Ne réaffectez pas les chemins tant que des tâches concernées attendent ou s’exécutent.

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

Voir [`t/TESTING.md`](../../t/TESTING.md) pour compilations strictes, sanitizers, Valgrind, plateformes et rapports. Tous les jobs GitHub sont dans `build-test-release.yml`.
