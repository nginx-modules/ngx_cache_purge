# ngx_cache_purge

[README](README.fr.md) · [Directives](DIRECTIVES.fr.md)

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
