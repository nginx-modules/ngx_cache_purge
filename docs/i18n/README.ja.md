<a id="ngx_cache_purge"></a>

# ngx_cache_purge

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

`FastCGI`、`proxy`、`SCGI`、`uWSGI` のキャッシュエントリを削除する NGINX モジュールです。v4 のキー世代管理では、ファイルの削除前に、新しいリクエストから古いエントリを利用できなくします。REFRESH は、プロキシキャッシュの内容が変わったかをオリジンサーバーに確認します。いずれも明示的な設定が必要です。PURGE の構文は引き続き使えますが、HTTP レスポンスは変更されています。[移行](#migration)を参照してください。

---

<a id="contents"></a>

## 目次

- [機能](#features)
- [目的に応じた動作の選び方](#choosing-a-mode)
- [互換性](#compatibility)
- [インストール](#installation)
- [キーの部分一致による PURGE](#partial-key-purge)
- [世代による無効化](#generation-invalidation)
- [オリジンにキャッシュ内容を確認する（REFRESH）](#conditional-refresh)
- [設定例](#sample-configurations)
- [ディレクティブ](#directives)
- [実行モデル](#execution-model)
- [性能調整](#performance-tuning)
- [監視とデバッグ](#monitoring-and-debugging)
- [トラブルシューティング](#troubleshooting)
- [テスト](#testing)
- [移行](#migration)
- [一時ディレクトリ](#temporary-directories)
- [セキュリティ](#security)
- [ライセンス](#license)

---

<a id="features"></a>

## 機能

- **同一 location での PURGE** — 専用 HTTP メソッド（`PURGE`）と IP アクセス制御
- **専用の PURGE location** — `proxy_cache_purge zone key` 構文で正規表現から取得したキーを指定し、`proxy_pass` なしで削除
- **ワイルドカード／部分一致** — 末尾の `*` によりキャッシュディレクトリを走査し、一致するエントリを削除
- **`purge_all`** — 1 回のリクエストでキャッシュゾーン内の全エントリを削除
- **バックグラウンドキュー** — 作業量を制限したイベントループの走査、または NGINX スレッドプールによる遅延実行。バッチ数と待機時間を設定可能
- **Vary 対応** — 完全一致 PURGE の後、同じキーを持つ gzip、Vary などのファイルバリアントも削除
- **応答形式** — `json`（既定）、`html`、`xml`、`text`、`none`
- **名前付きプール** — 継承可能なプール選択。スレッド非対応ビルドと Windows では段階的な走査に切り替え
- **実行中タスクの重複排除** — 走査中に同じ PURGE を受けても、インデックス上の操作は 1 件で、次回の再走査は最大 1 件だけ予約
- **ステータス** — キャッシュごとのキュー、削除、HIT/MISS メトリクスを、共有キューの mutex を取得せずに参照
- **世代付きキー** — PURGE リクエスト内でディスク I/O を行わず、完全一致・プレフィックス・名前空間を無効化。古い書き込みが遅れて完了しても新規要求からは参照されません
- [オリジンにキャッシュ内容を確認する（REFRESH）](#conditional-refresh)

---

<a id="choosing-a-mode"></a>

## 目的に応じた動作の選び方

| 目的 | 設定 | レスポンスの意味 |
|---|---|---|
| 古いと分かっているエントリを削除し、削除処理を待つ | バックグラウンドキューを無効にして PURGE | `200 purged` または `200 no_match`。エラーは 4xx/5xx |
| プレフィックス単位または全体の削除を予約する | PURGE と `cache_purge_background_queue on` | `202 accepted`。待機中は古いエントリが返る可能性がある |
| 遅れて書き込まれたものも含め、古い内容を新しいリクエストで使わせない | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`。新しいキー世代を使用し、ファイルは後で削除 |
| オリジンの変更を確認する | キー世代 + `proxy_cache_refresh` + 内部プロファイル | エントリごとの結果と継続情報を含む `200`。事前読み込みは行わない |

これらは組み合わせて使えます。世代の切り替えでも削除処理が予約されます。キューだけでは遅延書き込みによる古い内容の再利用を防げません。[v4 の例](#generation-and-refresh-example)、[PURGE の例](#sample-configurations)、[HTTP レスポンス](../HTTP_RESPONSES.md)（英語）を参照してください。

---

<a id="compatibility"></a>

## 互換性

この表は CI に設定された対象を示します。成功したジョブが検証するのは、そのソースとビルド構成であり、同一系列のすべてのリリースではありません。

| NGINX | CI: push / PR | CI: + full |
|---|---|---|
| 1.30.5, 1.31.6 | `test-matrix` | — |
| 1.31.6 | `build-compatibility` (GCC/Clang), `sanitizer`, `native-macos`, `native-windows` (x64/x86) | `memory-leak-test` (Valgrind) |
| 1.7.9 | `build-compatibility` (GCC) | — |
| 1.7.11, 1.7.12 | `build-compatibility` (GCC) | — |
| 1.20.2, 1.26.3, 1.28.3 | — | `test-matrix` |
| 1.11.4, 1.11.5 | — | `build-compatibility` (GCC) |

Linux ジョブはローカル実行と同じ Docker イメージとテスト起動方法を使います。ネイティブ macOS と Windows のジョブは、それぞれのファイルシステム API とプロセス API を検証します。Windows では MSVC でビルドし、`nginx.exe` の HTTP、パス、reload を検証します。POSIX のプロセス／シグナル用フィクスチャは Linux と macOS で実行します。Windows ではこれらのフィクスチャと NGINX スレッドプールの実行経路を使用しません。

`--with-threads` なしでは増分走査を使います。NGINX 1.7.12 より前の epoll 構成でも、コアの通知競合を避けるためこの方式に切り替えます。BSD のネイティブ CI はありません。通常の実行は現在の stable/mainline と API 境界を確認し、拡張実行では過去のバージョンと Valgrind を追加します。文書だけの変更ではビルドを省略します。実行条件とコマンドは[テスト説明](../../t/TESTING.md)を参照してください。

コードは設定、ハンドラー、キュー、走査、無効化、REFRESH、応答に分割されています。共通のパス関数は `ngx_cache_purge.h`、プラットフォーム操作は POSIX/Win32 ヘッダーにあります。`generation.*` は `invalidation.*` に改名しました。内部の generation は引き続きキーの世代を表します。[構成](../ARCHITECTURE.md)を参照してください。

NGINX の C89 互換スタイルに従い、コアの event、slab、rbtree、atomic API を使用します。C11 atomics や新しいファイルシステム API は必須ではありません。厳格な GNU89 ビルドで宣言順と警告を確認し、実際に検証したコンパイラとプラットフォームをテスト結果に記録します。

---

<a id="installation"></a>

## インストール

```bash
cd /path/to/nginx-source
./configure --add-module=/path/to/ngx_cache_purge
make
make install
```

動的モジュールの場合：

```bash
./configure --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

---

<a id="partial-key-purge"></a>

## キーの部分一致による PURGE

**評価後のキャッシュキー**に `*` を付けると、大文字小文字を区別せず前方一致します。完全一致キーと Vary は引き続き大文字小文字を区別します。物理削除のプレフィックスは末尾の `*` を除き 511 バイトまでです。超過すると一度ログを記録し、ファイルを走査せず `400 invalid_selector` を返します。

`cache_purge_key` では論理式を照合し、末尾の `*` を含めて最大 8096 バイトを受け付けます。

キー式が `$uri` で終わる場合、次のリクエストで URI の接頭辞を選択できます。

```http
PURGE /images/header*
```

アスタリスクは URI パスだけでなく、評価済みキー全体の最後のバイトである必要があります。`proxy_cache_key` のクエリ文字列や変数の接尾辞に注意してください。キーが `*` のみなら、削除対象になり得る全エントリを走査します。

---

<a id="generation-invalidation"></a>

## 世代による無効化

論理キーはオブジェクトを識別し、名前空間は同じ無効化ルールを使うキーをまとめます。世代とは NGINX のキャッシュキーに付加するバージョンです。PURGE 後の新しいリクエストは新しいバージョンを使います。先に始まった GET は完了できますが、その遅い書き込みは新しいリクエストの HIT にはなりません。

設定済みの名前空間では、PURGE は新世代を公開し、古い世代を削除する重複排除済みの走査を予約してから `200` と `invalidated` を返します。受付経路でキャッシュファイルの open/unlink は行いません。公開後に初めてキーを評価する新規要求は旧世代を HIT できません。要求ごとに名前空間／論理キーの世代を固定し、内部リダイレクトをまたいで保持します。開始済みの応答は完了できますが、その遅れた書き込みを後続の新規要求が HIT することはありません。完全一致は大小文字を区別し、プレフィックスは ASCII の大小文字を無視します。`purge_all` は名前空間全体を変更し、意図的に共有している他のキャッシュパスにも影響します。物理削除は指定パスに予約し、他のパスは core の失効処理または後続の PURGE に任せます。

失効を受け付けても、ディスク容量が即時に解放されるとは限りません。走査後に旧世代の書き込みが完了しても参照不能であり、後続の削除や core の inactive 期限で回収されます。名前空間 SHM が不足するとルールを新しい名前空間ベースラインに集約し、追加のキーを cold にします。失効済みキーを復活させることはありません。後続の無効化ごとに、切り離した trie ノードを最大 64 個回収します。同一ホットキーの反復 PURGE で木が無制限に成長することはありません。計算量はキャッシュファイル数ではなく、上限のあるキー長とタスク索引サイズに依存します。過負荷時の実時間 O(1) や starvation 回避を保証せず、受付は待機し得ます。無効化後の GET 集中には core の `proxy_cache_lock` などを利用してください。

<a id="conditional-refresh"></a>

## オリジンにキャッシュ内容を確認する（REFRESH）

キャッシュ済みの内容がオリジンで変わったか分からない場合に REFRESH を使います。商品カタログの取り込み後などが該当します。古いことが分かっていれば PURGE を使います。REFRESH は本文を取得せず HEAD を送り、保存済みの ETag や更新日時を HTTP の検証条件に使います。プロファイルは、オリジン、Host、ヘッダー、URI の対応関係を定める内部プロキシ location です。

REFRESH は TTL 前のキャッシュを検証します。事前読み込みは行わず、`304` でも TTL を延長しません。通常の期限切れ処理を行う core の `proxy_cache_revalidate` と `proxy_cache_background_update` を補完します。origin が validator を無視して常に `200` を返す場合、無効化に HEAD の負荷を追加する動作になります。候補は正規ハッシュパスにある、現世代の有効なファイルだけです。同一世代での物理的な置換は保守的に無効化される場合がありますが、世代比較により古い HEAD が後続 PURGE の新世代を無効化することは防ぎます。

| origin の実際の結果 | 動作／結果 |
|---|---|
| `304` | 保持。`kept` |
| `200`、`404`、`410` | 世代を条件付きで無効化し遅延削除。`invalidated` |
| その他のステータス | 保持。`upstream_status` |
| origin ステータスなし／転送エラー | 保持。`transport_error` |
| Vary メタデータ | HEAD なし。`skipped_vary` |
| 無効なマッピング／プロファイル | 保持。`skipped_profile` |
| 世代が既に置換済み | 新規無効化なし。`raced_replaced` |
| 内部の無効化／割り当て失敗 | 保持。`invalidation_error` |
| ページ停止までに未開始／未完了 | `undispatched`。カーソルはその項目を越えない |

worker ごとに実行中の REFRESH ページは一つ、同時プローブは一つで、全 worker 合計の共有上限は 16 プローブです。他の要求は候補配列を確保せず待機します。各ページは共通カーソルでディレクトリを走査し、最大 32 候補を保持してから core の HEAD サブリクエストを直列実行します。これにより候補メモリとメイン要求プールを共有するサブリクエスト数を制限します。同一の REFRESH HTTP 要求自体は重複排除せず、同時実行上限を共有します。無効化は共通の重複排除済み PURGE キューへ送ります。

カーソルなしで開始し、`complete` が true になるまで同じエンドポイント・論理セレクター・プロファイルに `X-Cache-Purge-Cursor: <next_cursor>` を送ります。カーソルは処理済みの最後の 32 桁 hex ファイル名です。不正または重複したカーソルヘッダーには `400` を返します。`scan_complete` は列挙の正常終了、`complete` はさらに次の該当ページがなく全選択項目の判断が完了したことを示します。`timed_out` は時間制限を示し、走査失敗は `500 scan_failed` を返します。`undispatched` は選択済みで未完了の件数で、走査が不完全なら下限にすぎずキャッシュ総数ではありません。走査未完了時はプローブを行わず、結果は空で入力カーソルを返します。結果にはハッシュと実際の upstream ステータスのみを含み、キーや認証情報は返しません。

各ページは次のハッシュを選ぶために再走査します。メモリを制限する代わりに I/O を繰り返し、全体では `O(ファイル数 × ページ数)` です。同時追加・削除がある場合のページ継続は弱い整合性のみを持ち、後時点の全体確認が必要なら最初からやり直してください。セレクターと期限を適切に設定してください。非常に大きな稼働中キャッシュを高頻度に全走査する用途には向きません。

<a id="generation-and-refresh-example"></a>

### 世代と REFRESH の設定例

main/http に組み込む設定断片です。既存の `events` ブロックを残し、オリジンとパスを変更してください。`--with-threads` なしのビルドでは `thread_pool` と `cache_purge_thread_pool` を削除し、段階的な走査を使います。

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

まず存在するキャッシュ可能なオブジェクトを GET し、`X-Cache-Status: HIT` が出るまで繰り返します。その後、同じオブジェクトを REFRESH で確認します。`kept` なら次の GET も HIT、`invalidated` なら再びオリジンへ問い合わせます。

```sh
curl -i 'http://127.0.0.1:8080/objects/item'
curl -i -X REFRESH -H 'X-Cache-Key: origin.test/objects/item' \
  'http://127.0.0.1:8080/admin/refresh'
```

プレフィックスではカーソルなしで開始します。`complete: false` なら、返された値を `X-Cache-Purge-Cursor` に設定し、下の `<next_cursor>` を置き換えて繰り返します。選択条件は変えず、HTTP エラーの原因を解決してから続行してください。

```sh
curl -i -X REFRESH -H 'X-Cache-Key: origin.test/objects/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: origin.test/objects/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

古いことが分かっている場合は PURGE を使います。この例のキー世代管理では、`200 invalidated` 後に新しいリクエストが古い世代を HIT することはありません。古いファイルは残っている場合があります。

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/objects/item'
```

[HTTP と境界ケース](../REFRESH.md)（英語）を参照してください。走査失敗は `500 scan_failed` であり、`scan_error: true` の継続ページではありません。

<a id="sample-configurations"></a>

## 設定例

<a id="basic--inline-purge-method"></a>

### 基本 — 同一 location で PURGE

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

### 基本 — 専用 PURGE location

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

### バックグラウンドキューの有効化

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

### 全エントリの削除

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

### location ごとに上書きする JSON 応答

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

### Vary 対応の PURGE

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

キーを削除すると、別のファイルパスにある gzip と Vary のバリアントも自動で削除します。

---

<a id="directives"></a>

## ディレクティブ

<a id="proxy_cache_purge"></a>

### `proxy_cache_purge`

```
Syntax:  proxy_cache_purge on | off;
         proxy_cache_purge <method> [purge_all] from all | <cidr> ...;
         proxy_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

**同一 location 形式**（`from …`）は、proxy location 内で指定 HTTP メソッドを受け取り、一致したキャッシュエントリを削除します。`on` は `PURGE` の省略形で、`off` は削除機能を無効にします。CIDR リストでアドレスを制限でき、`from all` は任意のアドレスを許可します。`from` の前に `purge_all` を置くと、リクエスト URI にかかわらずゾーン全体を削除します。

**専用 location 形式**（ゾーンとキーの引数）は PURGE 用 location 内だけで使用できます。通常は正規表現のキャプチャでキーを取得します。ゾーン名を検索し、コンパイルされたキー式で削除します。同じ location の `proxy_cache` または `proxy_pass` とは併用できません。

<a id="fastcgi_cache_purge"></a>

### `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

`proxy_cache_purge` と同等ですが、`fastcgi_cache` / `fastcgi_cache_path` で設定された FastCGI キャッシュゾーンを対象にします。

<a id="scgi_cache_purge"></a>

### `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

`proxy_cache_purge` と同等ですが、`scgi_cache` / `scgi_cache_path` で設定された SCGI キャッシュゾーンを対象にします。

<a id="uwsgi_cache_purge"></a>

### `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

`proxy_cache_purge` と同等ですが、`uwsgi_cache` / `uwsgi_cache_path` で設定された uWSGI キャッシュゾーンを対象にします。

<a id="cache_purge_response_type"></a>

### `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

成功した PURGE 応答の本文形式を指定します。既定は JSON です。`Cache-Purge-Code` と本文の結果コードは一致します。モジュールの応答には `Cache-Control: no-store` が付き、エラーは RFC 9457 の `application/problem+json` を使用します。テキストキーは形式に応じてエスケープし、バイナリキーは符号化方式を明示した Base64 で表現します。

`none` は PURGE 本文を省略します。完了した成功は `204`、受け付けたバックグラウンド処理は `202`、エラーは 4xx/5xx のままです。状態と REFRESH のエンドポイントは構造化 JSON を維持します。コード、フィールド、制約は [HTTP 応答仕様](../HTTP_RESPONSES.md)を参照してください。

<a id="cache_purge_background_queue"></a>

### `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

ワイルドカードと `purge_all` の遅延処理を有効にします。キューへの登録、または同一の待機中／実行中タスクへの統合後に `202 Accepted` を返します。完全一致キーの PURGE には影響しません。

この設定は従来のワイルドカード／全件削除を制御します。`cache_purge_key` を設定した場所では、この設定が off でも物理削除は遅延実行され、論理的な完了として `200` を返します。

キューが満杯、またはタスクのメモリ確保に失敗した場合、受け付けが成功するまでリクエストを保持します。待機中にディレクトリを走査しません。既存の待機中／実行中タスクの重複は満杯でも受け付けられます。小さな待機用コンテキストを確保できない場合は `500` を返します。切断時の cleanup はタイマーを解除し、待機数を減らします。

このフラグと `cache_purge_vary_aware` は最後に指定した値を使います。不正な値は拒否します。`cache_purge_legacy_status` は廃止済みで無視されます。スレッドとライフサイクルは[実行モデル](#execution-model)を参照してください。

<a id="cache_purge_queue_size"></a>

### `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

全キャッシュパスを合わせた未完了タスク数の上限です。待機中と割り当て済みの両方が容量を消費します。重複は同じメモリ領域を使い、再走査もその領域と容量枠を維持します。異なるタスクは空き枠を待ちます。

バックグラウンド処理またはステータスエンドポイントを設定すると共有ゾーンを作成します。丸め後のゾーンサイズが同じでも、容量変更には再起動が必要です。reload は受け付け済み作業を保護するためサイズ変更を拒否します。キャッシュルートの追加も、メモリ増加と再起動を必要とする場合があります。完了が必要な待機／実行中作業は先に処理してください。

名前空間の宣言でもタスクキューが作成されます。

ゾーンサイズにはタスク領域、パスメタデータ、整列されたワーカーメトリクスが含まれます。ビルドとキャッシュパス数に依存します。[メモリ計算](#memory-sizing)を参照してください。

<a id="cache_purge_thread_pool"></a>

### `cache_purge_thread_pool`

```
Syntax:  cache_purge_thread_pool <name>;
Default: default
Context: http, server, location
```

通常の継承規則に従って、受け付けたバックグラウンドタスクの実行先を選びます。例：

```nginx
thread_pool purge_io threads=2 max_queue=128;
http {
    cache_purge_background_queue on;
    cache_purge_thread_pool purge_io;
}
```

NGINX が名前付きプールを検証し、ネイティブのタスクイベントで完了を通知します。非公開のプール構造や独自の完了キューは複製しません。スレッド対応時は独自名のプールが存在する必要があります。スレッド非対応または Windows では設定を受け付け、作業量を制限したイベントループのスライスを使います。選択プールの満杯は実行を遅延させ、I/O をイベントループに戻しません。専用プールは追加のスレッドスタックとコンテキストスイッチを必要とします。

<a id="cache_purge_batch_size"></a>

### `cache_purge_batch_size`

```
Syntax:  cache_purge_batch_size <number>;
Default: 10
Minimum: 1
Maximum: 64
Context: http
```

ワーカーが休止するまでに処理する最大タスク数です。走査スライスやスレッド完了をまたいで進捗を保持します。空のキューで現在のバッチを終了します。64 を超える値は警告付きで 64 に制限し、0 は拒否します。

制御するのはタスク数であり、ファイル数やコールバック時間ではありません。小さい値はタスク間の休止を増やしますが、実行中のスレッド走査を中断しません。ディスク負荷の調整には `cache_purge_throttle_ms` と併用します。

<a id="cache_purge_throttle_ms"></a>

### `cache_purge_throttle_ms`

```
Syntax:  cache_purge_throttle_ms <time>;
Default: 10ms
Minimum: 1ms
Context: http
```

バックグラウンドバッチ間の遅延です。`10ms`、`500ms`、`1s`、引用符付きの複合値 `"2s 500ms"` などを受け付けます。単位なし整数はミリ秒を意味し、`10` と `10ms` は同値です。単位なしでは設定警告を出し、明示すると警告を回避できます。

アイドル状態のワーカーはこの値の 10 倍後に再確認します。未完了スライスと mutex 再試行は 1 ms タイマーを使用します。タイマー値は最小のスケジュール間隔であり、期限ではありません。0 とプラットフォームの安全な範囲を超える値は拒否します。

受付、プール待ち、遅延削除の再試行は 10 ms 間隔で確認します。

<a id="cache_purge_queue_status"></a>

### `cache_purge_queue_status`

```
Syntax:  cache_purge_queue_status <cache_path>;
Default: not configured
Context: location
```

GET/HEAD に `Cache-Control: no-store` 付き JSON スナップショットを返します。パスは設定済みのキャッシュルートでなければなりません。未知のパスは、バックグラウンド処理が無効でも設定エラーになります。エンドポイントへのアクセスを制限してください。

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

`queue` は全キャッシュパス共通のキューを表します。

| フィールド | 型／単位 | 意味 |
|---|---|---|
| `size` | 整数／タスク | ワーカーへの割り当てを待つタスク |
| `running` | 整数／タスク | プール待機を含む割り当て済みタスク |
| `max_size` | 整数／タスク | 待機中と割り当て済みを合わせた容量 |
| `wait_time` | 整数／ミリ秒 | 最古の待機タスクの経過時間。空なら 0 |
| `full` | 真偽値 | 取得した待機中と割り当て済みの合計が容量に達したか |

`cache` はこの location が選択したキャッシュルートを表します。

| フィールド | 型／単位 | 意味 |
|---|---|---|
| `pending.total` | 整数／タスク | このキャッシュの待機タスク |
| `pending.full` | 整数／タスク | `pending.total` に含まれる待機中の `purge_all` |
| `active` | 整数／タスク | 完了集計が終わるまでの割り当て済みタスク |
| `requests.overflow` | 整数／リクエスト | ロック競合やメモリ不足を含め、受け付け待機に入ったリクエスト |
| `requests.deduplicated` | 整数／リクエスト | 待機中または実行中の作業へ統合したリクエスト |
| `requests.waiting` | 整数／リクエスト | このキャッシュで現在受け付けを待つリクエスト |
| `files.deleted` | 整数／ファイル | 走査完了時に報告された、成功したバックグラウンド unlink |
| `files.errors` | 整数／エラー | 既に消えたファイルを除くバックグラウンド unlink 失敗 |
| `files.bytes` | 整数／バイト | unlink に成功したファイルの推定論理サイズ |
| `responses.hit`, `responses.miss` | 整数／メインリクエスト | log phase で観測したコアの HIT と MISS |
| `responses.hit_ratio` | 数値／0..1 | HIT / (HIT + MISS)。両方とも未観測なら 0 |
| `logical.invalidations` | 整数／判断回数 | 重複要求や REFRESH を含む、公開済みの世代無効化 |
| `cleanup.scan_errors` | 整数／エラー数 | 背景処理のメタデータ・open・read・close・割り当てエラー |
| `cleanup.retries` | 整数／走査回数 | 失敗により追加された再走査 |
| `cleanup.exhausted` | 整数／タスク数 | 3 回の再試行失敗後に終了したタスク |

`pending.full` は全件削除操作を数えます。キーが `*` のワイルドカードは別の操作種別です。`active` にはプール待機も含みます。実行中タスクは `queue.size` と `pending.total` からは除外されますが、受け付け容量を消費します。

名前空間走査の集約時、`pending.full` は最初に受け付けたタスクの分類を示します。REFRESH の走査／プローブは PURGE キュー長や削除エラー数に含まれません。遅延再試行でキュー順が変わると、`wait_time` は最長待機時間ではなく取得時の先頭要素の経過時間になる場合があります。

各プロセススロットの書き手は 1 つのイベントループだけで、先頭とストライドをキャッシュラインに整列します。スレッドは完了イベントで結果を返します。ステータスの読み手はキュー mutex を取得せず、加算量 0 を含め atomic fetch-add を使いません。64 ビットビルドでは、カウンターに対して整列された通常の volatile 読み書きを行います。32 ビットビルドでは回数制限付きのシーケンス確認で 64 ビット読み取りを保護し、書き込み中のシャードが 1 回のサンプルから除外される場合があります。各値は近似であり、ワーカー間のトランザクションではありません。受け付け判断は必ずロック下の制御状態を使い、監視値には依存しません。

累積カウンターは符号なし 64 ビットの最大値で飽和し、通常の reload を越えて維持し、再起動でリセットします。待機数はリクエストの cleanup に追従します。同期操作とコアのキャッシュマネージャーによる削除は、バックグラウンド削除総数に含みません。バイト数はファイル長の推定であり、ファイルシステムのブロック数や実際の空き容量の証明ではありません。開いたままの読み手や同時書き込みが結果を変えるためです。途中までの走査結果も集計しますが、クラッシュしたワーカーの結果は欠ける場合があります。監査ログとしては使用できません。

HIT/MISS はネイティブのキャッシュ判定を持つメインリクエストだけを集計します。upstream 不在、キャッシュ状態 0、BYPASS、EXPIRED、STALE、UPDATING、REVALIDATED は比率から除外します。キャッシュを使わない通常のリクエストを MISS にしません。監視だけでも、バックグラウンド PURGE が無効な状態で SHM ゾーンを作成できます。32 ビットシステムではキューの経過時間は約 24.9 日未満の間隔を対象とし、符号付き差分が負なら 0 として報告します。

<a id="cache_purge_legacy_status"></a>

### `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

設定移行のためだけに受け付ける廃止済みディレクティブです。どちらの値も警告を出し、動作には影響しません。設定から削除してください。不正な値は拒否します。同期処理で一致がなければ `200 no_match`、物理削除の受け付けは `202 accepted`、世代無効化の公開は `200 invalidated` です。`412` と `404` の切り替えは廃止しました。[HTTP 応答仕様](../HTTP_RESPONSES.md)を参照してください。

<a id="cache_purge_vary_aware"></a>

### `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

`on` の場合、完全一致 PURGE が成功して主ファイルを削除した後、キャッシュディレクトリを走査し、同じ `KEY:` 文字列を含む残りのファイルを削除します。これにより、異なるパスに保存されても同じ論理キーを共有する `Vary` と `gzip_vary` のバリアントを対象にします。キー直後のバイトが `\n` であることを確認し、共通接頭辞だけを持つ別キーへの誤一致を防ぎます。

完全一致の PURGE ごとに全ディレクトリー走査を追加するため、既定では無効です。複数の `Vary`/`gzip_vary` 表現がある場合に有効にしてください。主エントリーが既にない場合も走査します。ワイルドカードと `purge_all` は元から派生ファイルを含みます。完全一致の Vary キーは大文字小文字を区別し、上限は 511 バイトです。超過すると削除前に `400 invalid_selector` を返します。

世代による無効化は、この設定なしでも論理キーの全表現に適用されます。

<a id="cache_purge_namespace"></a>

### `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

location が参照する前に世代の名前空間を宣言します。名前は ASCII 英数字・`_`・`-` の 1～32 文字、SHM は最低 8 ネイティブページです。キャッシュと一時領域の外に専用の永続状態ディレクトリを置いてください。core は排他的な永続マーカーファイルで再起動 epoch を割り当てます。対応するキャッシュが存在し得る限りマーカーを保持してください。キャッシュを残して状態だけを削除すると古い epoch を再利用する可能性があります。電源断や fsync による永続性は保証しません。通常の reload では epoch とルールを維持し、サイズ・パス変更や名前空間の削除には完全再起動が必要です。名前空間とタスクキューの SHM サイズは独立しています。

<a id="cache_purge_key"></a>

### `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

location を既存の名前空間に結び付け、`$cache_purge_key` を提供します。`proxy_cache_key`、`fastcgi_cache_key`、`scgi_cache_key`、`uwsgi_cache_key` の全体をこの変数に設定し、対応する別 PURGE location にも同じ設定を適用します。展開したネイティブキーが生成キーと異なる PURGE は、厳密な無効化を装わず `500` で失敗します。論理式内で `$cache_purge_key` 自身を参照しないでください。論理キーは最大 8096 バイトです。他の location で従来の動作が必要なら参加する location のみに設定してください。継承した設定を解除する `off` はありません。

<a id="proxy_cache_refresh"></a>

### `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

メイン要求専用の明示的な REFRESH エンドポイントを作成し、他のメソッドには `405` を返します。`cache_purge_key` の設定、ネイティブ式としての `$cache_purge_key`、refresh プロファイルが必要です。論理キーで完全一致、末尾 `*` のプレフィックス、または `*` でこのキャッシュパスの名前空間全体を選択します。core のアクセス制御／認証で保護してください。REFRESH は proxy キャッシュのみ、PURGE は引き続き全 4 プロトコルに対応します。

<a id="cache_purge_refresh_profile"></a>

### `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

既知の一つの表現を、固定の内部 proxy location に対応付けます。論理キーから大文字小文字を区別した完全一致のプレフィックスを除くと、`/` で始まる origin URI になる必要があります。query とエスケープは保持します。ホストを推測したり、キー内の `://` を探したりはしません。転送先には `internal`、cache off、要求ヘッダー／本文の転送 off、HEAD、空でない固定 Host、固定の表現ヘッダーが必要です。条件ヘッダーには `$cache_purge_refresh_etag` と `$cache_purge_refresh_last_modified` をそのまま使います。`$cache_purge_refresh_uri` は対応する URI を `proxy_pass` に渡します。この 3 変数は副作用のない取得専用で、プローブ内のみ有効です。内部リダイレクトも preaccess で検証します。管理者がキャッシュキーと同じ origin 表現になることを確認してください。Vary は元の要求ヘッダーがネイティブメタデータに残らないためスキップします。

プローブ location では `proxy_intercept_errors off` を設定してください。origin エラーが成功に書き換えられて失効を起こさないよう、継承した interception は拒否します。

<a id="cache_purge_refresh_timeout"></a>

### `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

worker のスキャナー待機も含め、REFRESH ページの新規走査／プローブ開始を制限します。期限後は新しい処理を始めず、不完全な応答とします。実行中の HEAD は core の `proxy_connect_timeout`、`proxy_send_timeout`、`proxy_read_timeout` に従って終了するため、明示的に設定してください。厳密な応答期限ではなく、ファイルシステムのシステムコールを中断できません。切断と graceful shutdown では新規開始を止め、実行中スレッドの領域は core の完了通知まで保持します。

---

<a id="execution-model"></a>

## 実行モデル

**タスク**は 1 件のワイルドカード操作または `purge_all` 操作です。**バッチ**は、ワーカーが休止するまでに処理するタスク数です。**スライス**は、段階的なディレクトリ走査の作業量を制限した 1 ステップです。1 タスクに多数のスライスが必要な場合があります。

世代付きキーでは、キャッシュパスと名前空間ごとの一つの物理走査に無効化要求を集約します。REFRESH ページは独立した HTTP 操作で、PURGE キューのタスクではありません。

| リクエストと設定 | ファイル走査 | HTTP 応答の完了 |
|---|---|---|
| 従来の完全一致キー（Vary 照合の有無を問わない） | リクエストワーカー。Vary は追加のディレクトリ走査を実施 | 操作終了後 |
| ワイルドカードまたは `purge_all`、バックグラウンドキュー無効 | リクエストワーカーで同期走査 | 走査終了後 |
| 選択したプールによるバックグラウンドタスク | 各ワーカーにつき同時に最大 1 プールタスク | 受け付け後に `202` |
| 利用可能なスレッドプールがないバックグラウンドタスク | ワーカーのイベントループで段階的に走査 | キュー登録後に `202` |
| キュー容量またはメモリ割り当ての不足 | 受け付けを待機。リクエスト専用の走査は実行しない | 受け付け後に `202` |
| 世代による PURGE、すべてのキー形式 | 名前空間の遅延走査。要求内でキャッシュを open/unlink しない | 公開と受付後に `200 invalidated` |
| REFRESH エンドポイント | 有界走査と最大 32 回の直列 HEAD | 完了・継続情報を含む `200` JSON |

世代付きキー以外の行は従来の動作です。容量待機中の世代要求は、受付後に `202` ではなく `200` を返します。

1 スライスは、ディレクトリの反復が最大 64 回に達するか、経過時間 5 ms を確認した時点で終了します。スキップしたエントリとディレクトリ終端も予算に含みます。ただし、単一のファイルシステムシステムコールの所要時間は制限できません。

バックグラウンドのファイル操作をスレッドで実行するには、`--with-threads` でビルドし、`http` の外側の **main** コンテキストにコアのプールを定義します。

```nginx
thread_pool default threads=2;
```

`cache_purge_thread_pool <name>` は `http`、`server`、`location` で名前付きプールを選択します。既定は `default` です。このプールが参照されても明示的な宣言がなければ、NGINX が標準設定で作成します。独自の名前はコアの `thread_pool` で宣言する必要があります。設定は継承され、重複してまとめられたタスクでは最初に受け付けたリクエストがプールを決めます。各ワーカーは独自のコアプールを持ちます。プールが満杯の場合はタスクの容量枠を保持し、10 ms 後に投入を再試行します。満杯を理由に同期走査を開始しません。スレッド非対応ビルド、Windows、単一プロセスモード、または reload で以前のタスクのプールが削除された場合は、段階実行のカーソルを使います。

REFRESH は選択したプールに worker ごとに最大一つの追加スキャナーを投入します。HTTP サブリクエストはイベントループで実行され、I/O スレッドからその状態にはアクセスしません。

キューとインデックスの構造変更には slab mutex を使います。受け付け処理とバックグラウンドコールバックは trylock を使用し、取得できなければタイマーで再試行します。待機中**および実行中**のタスクをキャッシュパスごとのコア文字列 rbtree に保持し、ハッシュ衝突はキー全体の比較で解決します。対象パスの未完了タスク数を `Q` とすると、インデックス操作は `O(log Q)` です。キャッシュパスの検索は登録パス数に対して線形です。待機中と実行中の管理にはネイティブ `ngx_queue_t` リストを使います。実行中操作に最初の重複が来ると再走査を予約し、その後の重複は同じ予約にまとめます。現在の走査後に再走査をキュー末尾へ入れるため、後続 PURGE を黙って捨てません。新しい PURGE が継続すると、さらに再走査する場合があります。最終完了時にのみ容量枠を解放します。ファイル操作と応答生成はロックの外で行います。

rbtree は物理削除タスクの重複排除専用です。世代ルールは別の SHM 領域の有界なバイトトライに保持します。名前空間走査と従来のキー別タスクは異なる索引識別子を使い、REFRESH の無効化も共通の削除処理へ合流します。

キューが満杯、またはタスクのメモリ確保に失敗した場合、HTTP リクエストは 10 ms の再試行タイマーとコアの切断検出を使って受け付けを待ちます。リクエストと cleanup ハンドラーは所属ワーカーに保持し、SHM にリクエストポインターを保存しません。HTTP/2 と HTTP/3 で接続全体の読み取りを止めません。キュー負荷による 429/503 は返さず、読み取り済みの短い PURGE で TCP Zero Window が発生する保証もありません。待機リクエストは依然としてメモリと接続を消費するため、配備環境の接続予算に合わせてコアのタイムアウトと受け付け方針を設定してください。

受け付けたタスクは、通常の reload 後もキー、プール名、一時パスの保護設定を保持します。新しい重複は、待機中タスクまたは次回の再走査の保護設定を更新します。新規バックグラウンド受け付けを無効にしても、受け付け済みの作業は失われません。graceful 終了中のワーカーは現在の走査を終え、新しいタスクを引き受けません。高速終了時は未完了の段階実行タスクを待機キューに戻し、exit hook で走査を完遂しません。投入済みのスレッドタスクの寿命は、コアのプール終了処理が管理します。

割り当て済みタスクは PID とスロットの世代番号で識別します。ワーカーは 1 秒間隔の回復処理で最大 64 件の実行中レコードを確認し、所有者が終了した作業を再登録します。これは所有者を失った割り当ての回復であり、トランザクションジャーナルではありません。共有リストや slab の変更中にクラッシュすると、状態破損により再起動が必要になる可能性があります。クラッシュ直前の削除はメトリクスに残らない場合があります。待機作業と再走査には実行中ワーカーが必要です。完了が必要なら停止前にキューを空にしてください。キューは永続化されません。

キューの内部レイアウトはアップグレード ABI ではありません。異なるレイアウトのビルドへ交換するときは、必要な作業を先に完了させてから完全に再起動してください。同じビルドの通常 reload はゾーンを再利用します。

削除はベストエフォートです。`202` は受け付けを示し、削除完了を示しません。走査完了もキャッシュのアトミックなスナップショットではありません。同時リクエストは走査中や走査後に新しいエントリを作成でき、ファイルエラーはログに記録されます。ワイルドカードと全件走査は、すべてのネイティブキャッシュノードを同期更新せずに unlink します。完全一致と Vary 操作では一致したノードを無効化します。

バックグラウンド処理の失敗時は 100 ms、1 秒、10 秒後に最大 3 回再試行します。待機中も重複排除と受付枠を保持しますが、他のタスクは進行できます。新たな PURGE は再試行回数をリセットします。上限後はエラーを記録してタスクを終了するため、`cleanup.exhausted` を確認し、ストレージを修復して再度 PURGE してください。物理削除が失敗しても世代の無効化は有効です。消失済みファイルは通常の競合として扱い、不正なファイルは再試行せずスキップします。

---

<a id="performance-tuning"></a>

## 性能調整

3 つの設定は異なる制約を制御します。`queue_size` は未完了作業を制限し、`batch_size` はワーカーごとにタスクをまとめ、`throttle_ms` はその間隔を設定します。変更前後でリクエスト遅延、ストレージ競合、キューの経過時間を測定してください。容量を増やしても処理スループットは増えません。

以下は開始点となる設定であり、ハードウェア性能の保証ではありません。列名から共通の `cache_purge_` 接頭辞を省略しています。

| 負荷／ストレージの条件 | `queue_size` | `batch_size` | `throttle_ms` |
|---|---:|---:|---:|
| 制約のあるホストまたはストレージ | 512 | 5 | 25ms |
| 一般的な開始点 | 2048 | 20 | 10ms |
| 余力を測定済みの高速ストレージ | 8192 | 50 | 5ms |
| 短いバーストと控えめなバックグラウンド処理 | 8192 | 5 | 50ms |

例えば一般的な設定は次のとおりです。

```nginx
cache_purge_background_queue on;
cache_purge_queue_size        2048;
cache_purge_batch_size        20;
cache_purge_throttle_ms       10ms;
```

大きいキューはバーストを吸収できますが、到着率が完了率を継続的に上回ると満杯になります。重複排除後のタスク毎秒を測り、追加の必要枠を `max(0, arrival_rate - completion_rate) * burst_seconds` で概算できます。処理時間のばらつきと既存の滞留分に余裕を持たせてください。受け付け待機は開いたリクエストと接続を増やすため、ストレージ飽和時のメモリと FD の予算にも含めます。

<a id="memory-sizing"></a>

### メモリ計算

設定するゾーンサイズは、ネイティブ構造体のサイズから計算します。

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

| 要素 | 予約または使用するメモリ |
|---|---|
| タスク用の予約 | `Q * S`。slab の丸めとインラインのキー／プール名の余裕を含む |
| 1 パスのメトリクス | `R * NGX_MAX_PROCESSES` と整列領域。ストライド 192 バイト、1024 スロットなら 192 KiB |
| キュー制御 | `H`。プロセス所有者と世代番号の配列を含む |
| パス用の予約 | `M`。名前、rbtree の根、整列領域、メトリクス配列を含む |
| 最小ゾーン | ネイティブページ 8 枚 |

実測した LP64 レイアウト（`I=184`、`H=20576`、パスレコード 104 バイト、統計レコード 144 バイト）で、キャッシュライン 64 バイト、ページ 4 KiB、長さ 20 バイトのキャッシュパスが 1 つの場合、式は次の値になります。

| 容量 | ゾーンのバイト数 | ゾーンの MiB |
|---:|---:|---:|
| 512 | 1654784 | 1.58 |
| 2048 | 5373952 | 5.12 |
| 8192 | 20242432 | 19.30 |

実際のタスク割り当ては slab の丸め前で `I + index_bytes + 1 + pool_name_bytes` です。埋め込み索引は操作タグ 1 バイトと従来のキー、または世代走査用の名前空間名を格納します。従来の `purge_all` はタグのみを格納します。上限は待機中と実行中の割り当ての合計です。構造体、キャッシュライン、`NGX_MAX_PROCESSES`、設定パスで使用量が決まり、Windows・32 ビットでは異なります。reload 時の保護設定も SHM を消費し、件数上限より先に割り当てが失敗する場合があります。

プロセスメモリは別です。実行中の走査は、固定のキーバッファ、再利用するパスバッファ、開いたディレクトリごとの固定長フレームを使います。フレームには親パスを複製せず接頭辞長を保持するため、領域はファイル数と無関係な `O(depth + longest_path)` です。ディレクトリストリームにはプラットフォーム側のバッファがあります。待機リクエストは走査カーソルを持たず、小さなタイマー／cleanup コンテキストだけを保持します。スレッドスタックとコアプールには別途メモリが必要です。

名前空間 SHM のサイズは別途設定します。REFRESH スキャナーの候補領域／コンテキストは測定した LP64 で固定 283864 バイト（32 件）です。このほかに走査フレームと、HTTP 要求プール内に最大 32 個の core サブリクエストがあります。ページ完了で要求を解放し、継続は新たな要求で行います。他の待機要求は候補配列を確保しません。

<a id="throughput-and-latency"></a>

### スループットと遅延

継続して処理するワーカーには、次の概算が使えます。

```text
B = tasks completed in a batch
T = throttle delay in milliseconds
D = elapsed batch processing time in milliseconds, including slice waits
rate_per_worker ~= 1000 * B / (D + T) tasks/s
```

`D` を無視したスケジューリング上限は `1000 * B / T` で、既定ではワーカーごとに 1000 タスク/秒です。これはファイル削除率でも実測値でもありません。10 タスクで 200 ms かかる場合、同じ 10 ms の休止を加えると約 47.6 タスク/秒です。ストレージを共有するため、ワーカー数を掛けてもそのスループットは保証されません。アイドルワーカーの新規作業確認は最大 `10 * T` とイベントループの遅延だけ待つ場合があります。

バッチを小さくするか休止を長くすると、休止間の作業が減ります。ディスク操作中のワーカー遅延を小さくするにはプールを使用します。どちらの設定も単一システムコールを時間制限したり、スレッドのタスクを複数バッチへ分割したりはしません。

---

<a id="monitoring-and-debugging"></a>

## 監視とデバッグ

[ステータスエンドポイント](#cache_purge_queue_status)で、待機／実行中の作業、受け付け待機、重複、unlink 結果、HIT/MISS を確認できます。`202 Accepted` は受け付けまたは統合の確認であり、削除完了や即時の論理無効化の証明ではありません。リクエスト遅延をキューの経過時間とストレージ競合と合わせて監視してください。

| レベル | 条件 |
|---|---|
| `warn` | 過長キー、削除されたタスクのプール、上限に調整された設定値 |
| `error` | アクセス拒否、キャッシュ検索や REFRESH プロファイルの失敗、再試行の上限到達、コアプールや Windows の読み取り失敗 |
| `crit` | ファイルのオープン／メタデータ取得／削除、ディレクトリ操作の失敗、POSIX コアの読み取り失敗 |
| `alert` | ファイルのクローズ失敗 |
| `emerg` | 不正設定、非互換 SHM、サイズ変更、reload メタデータの確保失敗 |
| `debug` | リクエストと走査に関する追加情報 |

モジュールの診断は NGINX 標準のログ関数を使い、`ngx_cache_purge: ` で始まります。重大度、時刻、プロセス／スレッド ID、errno の説明、リクエストや設定のコンテキストは、設定された出力形式に従って NGINX が付加します。ファイルシステムのメッセージには実際に失敗した操作と引用符で囲んだパスを、namespace と REFRESH のエラーには対象と拒否理由を記録します。収集には標準の NGINX エラーログパーサーを使い、メッセージの接頭辞で絞り込んでください。

デバッグメッセージは `NGX_LOG_DEBUG_HTTP` を使い、`--with-debug` ビルドと `error_log /var/log/nginx/error.log debug;` の両方が必要です。

キュー負荷は再試行ごとの警告ではなく、受け付け待機のメトリクスで表します。既に消えたファイルは想定内の競合であり、モジュールの診断ログを出しません。設定パーサー、メモリアロケーター、I/O、スレッドプールなど、NGINX コアの関数が出力するメッセージはコアの書式を維持します。障害調査ではログ全体を確認してください。

```bash
tail -f /var/log/nginx/error.log | grep 'ngx_cache_purge:'
```

検証対象の診断は[ログ検証](../../t/TESTING.md#logging-contracts)を参照してください。

---

<a id="troubleshooting"></a>

## トラブルシューティング

| 症状 | 確認または調整 |
|---|---|
| 大規模走査でワーカー遅延が増える | バックグラウンドキューと I/O 用プールを使用。完全一致／Vary とキュー無効時の操作は同期のままです。 |
| 受け付け待機が増える | 到着率、キュー経過時間、プール容量、ディスク負荷を確認。容量追加はバースト対策であり、変更には再起動が必要です。 |
| iowait が高い | `cache_purge_batch_size` を下げ、`cache_purge_throttle_ms` を上げ、PURGE の流入を制限します。 |
| キューの処理が遅い | バッチ増加や休止短縮の前に、タスク時間とストレージ競合を測定します。 |
| クライアントが従来の `412`/`404` を期待する | `cache_purge_legacy_status` を削除し、`200` と `Cache-Purge-Code: no_match` を処理してください。 |
| Vary バリアントが残る | `cache_purge_vary_aware`、主ファイルの PURGE 結果、511 バイトのキー制限を確認します。 |
| 受け付け済み作業が完了しない | 実行中タスク、回復処理、ファイルエラーを確認。`202` は完了通知ではなく、SHM 破損が疑われる場合は再起動が必要です。 |

---

<a id="testing"></a>

## テスト

[テスト範囲](../TEST_COVERAGE.md)（英語）に、検証済みの動作と残っている不足を記載しています。

Docker 内でビルドし、全スイートを実行します。

```sh
make test-all
NGINX_VERSION=1.31.6 make test-all
```

`t/` から従来の Compose サービスを使用できます。

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

Compose プラグインでは `docker-compose` を `docker compose` に置き換えます。Makefile は両方を検出します。`nginx-test` と `make test-all` は、CI と同じ `prove` の起動方法で全 7 個の `t/*.t` スイートを実行します。統合イメージにはネイティブ補助関数／レイアウトの検証とプール実行のランタイムテストが含まれます。スレッドを無効にした厳格なビルドでは段階実行を検証します。テスト失敗はコマンドの失敗として伝わります。

厳格なビルド、sanitizer、Valgrind、プラットフォームジョブ、レポートは [`t/TESTING.md`](../../t/TESTING.md) を参照してください。GitHub のジョブはすべて `ci.yml` に定義しています。

---

<a id="migration"></a>

## 移行

<a id="queue-and-status-changes"></a>

### キューとステータスの変更

SHM レイアウト変更には完全再起動が必要です。メソッド／ACL 構文とプロトコルハンドラーは維持します。v4 は既定で JSON を使用し、ヘッダーと本文に一致する結果コードを返します。未登録のキーは `200 no_match` となり、`cache_purge_legacy_status` は警告のみ出します。[HTTP 応答仕様](../HTTP_RESPONSES.md)に合わせてクライアントを更新してください。容量は割り当て済みタスクも含み、過負荷時は受け付けを待ちます。実行中の重複は再走査を予約できます。

既存のステータスフィールド名を保ったまま項目を追加します。`requests.overflow` は受け付け待機に入ったリクエストを数えます。メトリクスは近似値で、累積値は 64 ビットです。スレッド対応ビルドは参照時に `default` を自動登録します。PURGE I/O を隔離するには名前付きプールを使用してください。ステータスだけの設定でも SHM を確保します。

v4.0 の世代無効化と REFRESH は明示的な opt-in です。参加する全 location で `cache_purge_key` を設定し、ネイティブのキャッシュキー全体に `$cache_purge_key` を使います。物理ファイル名が変わるため、既存の従来ファイルは HIT せず、core の期限切れ処理または明示的な従来の削除まで残ります。状態マーカーをキャッシュと共に保持してください。既存の PURGE location に REFRESH は追加されません。実験的 PR60 の利用者は新しいエンドポイント・プロファイル・ページ継続へ移行してください。PR60 の無制限応答、URI/Host の推測、暗黙のメソッド拡張は互換性の対象ではありません。

<a id="from-the-original-frickle-module"></a>

### 元の FRiCKLE モジュールから

メソッド／アクセスリスト形式とゾーン／キー形式は引き続き使用できます。存在しないエントリーは `200 no_match` を返します。`cache_purge_legacy_status` を削除し、クライアントを更新してください。バックグラウンド処理を有効にするには、次を追加します。

```nginx
http {
    cache_purge_background_queue on;
}
```

<a id="from-nginx-selective-cache-purge-module"></a>

### nginx-selective-cache-purge-module から

モジュールを置き換えます。バックグラウンドキューは内蔵されています。従来のパラメーターを `cache_purge_queue_size`、`cache_purge_batch_size`、`cache_purge_throttle_ms` に対応付けてください。

---

<a id="temporary-directories"></a>

## 一時ディレクトリ

キャッシュルート配下の設定済み一時ディレクトリは、パス要素全体の比較で走査対象から外します。`proxy_temp` などの名前自体を予約しているわけではありません。削除候補は、ファイル名がちょうど 32 桁の 16 進数である通常ファイルだけです。数値名や接尾辞付きのキャッシュ書き込み一時ファイルは、`use_temp_path=off` の場合もスキップします。

全走査でネイティブヘッダーの版、オフセット、完全な `KEY: ` マーカー、CRC32、キー／variant のファイル名ハッシュを検証します。キー上限は固定 8192 バイトです。不正・途中切れ・異なる版・不一致のファイルは全件 PURGE でもスキップします。ETag/Vary 長が core の最大値と等しい場合も受け付けます。これは形式検証であり、悪意あるファイルの認証ではありません。

設定したキャッシュルート自体は symlink や Windows junction でも構いません。配下の symlink／reparse point はスキップします。必要な descriptor-relative API がすべてある POSIX では、core の `ngx_openat_file`／`ngx_file_at_info` と no-follow フラグで子を開き、保持した親ディレクトリから相対削除します。配下のリンク差し替えによる転送は防ぎますが、既に開いたディレクトリの移動に対する sandbox ではありません。Windows と古い Unix はリンク確認と core の完全パス操作を使うため、パス差し替え競合が残ります。従来の完全一致キーの core 検索・削除も完全パスを使います。ルートへの書き込みは信頼できる管理者に限定してください。

設定パスの表記を一貫させてください。比較ではシンボリックリンク、マウント、Unicode の別名を解決しません。タスクは保護設定を保持し、新しい重複は待機作業または次回の再走査を更新します。実行中の走査は従来の設定を使います。全ワーカー世代の保護設定を合成する方式ではありません。関連タスクが待機／実行中の間は、そのパスを別用途に転用しないでください。

---

<a id="security"></a>

## セキュリティ

- `allow` / `deny` または上流の認証で PURGE エンドポイントを制限し、公開しないでください。
- リクエスト集中時の SHM 消費を抑えるため、負荷に合った `cache_purge_queue_size` を選んでください。
- 通常トラフィックと独立して制限するため、PURGE location に `limit_req` を併用することも検討してください。

---

<a id="license"></a>

## ライセンス

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

## 関連情報

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [nginx documentation](https://nginx.org/en/docs/)
