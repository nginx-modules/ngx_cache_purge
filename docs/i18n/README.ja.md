<a id="ngx_cache_purge"></a>

# ngx_cache_purge

`FastCGI`、`proxy`、`SCGI`、`uWSGI` のキャッシュエントリを削除する NGINX モジュールです。v4 のキー世代管理では、ファイルの削除前に、新しいリクエストから古いエントリを利用できなくします。REFRESH は、プロキシキャッシュの内容が変わったかをオリジンサーバーに確認します。いずれも明示的な設定が必要です。PURGE の構文は引き続き使えますが、HTTP レスポンスは変更されています。[移行](README.ja.md#migration)を参照してください。

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

<a id="choosing-a-mode"></a>

## 目的に応じた動作の選び方

| 目的 | 設定 | レスポンスの意味 |
|---|---|---|
| 古いと分かっているエントリを削除し、削除処理を待つ | バックグラウンドキューを無効にして PURGE | `200 purged` または `200 no_match`。エラーは 4xx/5xx |
| プレフィックス単位または全体の削除を予約する | PURGE と `cache_purge_background_queue on` | `202 accepted`。待機中は古いエントリが返る可能性がある |
| 遅れて書き込まれたものも含め、古い内容を新しいリクエストで使わせない | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`。新しいキー世代を使用し、ファイルは後で削除 |
| オリジンの変更を確認する | キー世代 + `proxy_cache_refresh` + 内部プロファイル | エントリごとの結果と継続情報を含む `200`。事前読み込みは行わない |

これらは組み合わせて使えます。世代の切り替えでも削除処理が予約されます。キューだけでは遅延書き込みによる古い内容の再利用を防げません。 [v4 の例](README.ja.md#generation-and-refresh-example)、[PURGE の例](EXAMPLES.ja.md#sample-configurations)、[HTTP レスポンス](../HTTP_RESPONSES.md)（英語）を参照してください。

<a id="compatibility"></a>

## 互換性

本番環境には保守中の NGINX リリースを使用してください。主な対象は POSIX/UNIX で、Windows 対応は実験的です。スレッドは任意です。 [NGINX / CI](../../t/TESTING.md#compatibility-matrix).

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
./configure --with-compat --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

サーバーにインストールされている NGINX と同じバージョンのソースを使用してください。`--with-compat` は、任意の NGINX バージョンや C ライブラリ間の互換性を保証するものではありません。詳しくは[動的モジュールとリリースアーカイブ](../BINARY_MODULES.md)（英語）を参照してください。

<a id="generation-and-refresh-example"></a>

## 例：商品カタログ

この完全な設定例はローカルの 8080 ポートで受け付け、8081 ポートのアプリケーションへ転送します。アプリケーションは Host `shop.example.com` で `/products/42` を提供する必要があります。アプリケーションのアドレス、ドメイン、書き込み可能なキャッシュと状態のディレクトリを変更してください。

`shop.example.com` は HTTP Host のドメインで、ディレクトリではありません。`/products/42` は商品の URI です。`catalog:` は固定のキャッシュキープレフィックスで、ホスト名ではありません。クエリ文字列を含む完全な URI と結合し、`catalog:/products/42` のような論理キーにします。REFRESH プロファイルは `catalog:` を除き、同じアプリケーションの `/products/42` を確認します。

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

存在するキャッシュ可能な商品を GET し、`X-Cache-Status: HIT` になるまで繰り返してから REFRESH を送ります：

```sh
curl -i 'http://127.0.0.1:8080/products/42'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/42' \
  'http://127.0.0.1:8080/admin/refresh'
```

商品が変更済みなら PURGE を送ります。この設定では `200 invalidated` により新しいリクエストが古い世代を利用できなくなり、次の GET はアプリケーションへ進みます。

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/products/42'
```

全商品の確認には `catalog:/products/*` を使います。`complete: false` の場合は選択条件を維持し、返された値で `<next_cursor>` を置き換えて続行してください。HTTP エラーは先に解決してください。

```sh
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

<a id="partial-key-purge"></a>

## キーの部分一致による PURGE

**評価後のキャッシュキー**に `*` を付けると、大文字小文字を区別せず前方一致します。完全一致キーと Vary は引き続き大文字小文字を区別します。物理削除のプレフィックスは末尾の `*` を除き 511 バイトまでです。超過すると一度ログを記録し、ファイルを走査せず `400 invalid_selector` を返します。

`cache_purge_key` では論理式を照合し、末尾の `*` を含めて最大 8096 バイトを受け付けます。

キー式が `$uri` で終わる場合、次のリクエストで URI の接頭辞を選択できます。

```http
PURGE /images/header*
```

アスタリスクは URI パスだけでなく、評価済みキー全体の最後のバイトである必要があります。`proxy_cache_key` のクエリ文字列や変数の接尾辞に注意してください。キーが `*` のみなら、削除対象になり得る全エントリを走査します。

<a id="generation-invalidation"></a>

## 世代による無効化

キー世代を使うと、PURGE 完了後の新しいリクエストが古い内容に HIT しなくなります。遅れて書き込まれた古い応答も対象です。ファイルは別途削除されます。名 前空間は無効化ルールを共有するための単位です。再起動時も状態ディレクトリを保持してください。

<a id="conditional-refresh"></a>

## オリジンにキャッシュ内容を確認する（REFRESH）

REFRESH は保存済みの ETag と Last-Modified を使い、同じオリジンの表現に HEAD を送ります。商品ページが古いと分かっている場合は PURGE を使ってください。REFRESH は本文の再取得、空のキャッシュの事前生成、TTL の延長を行いません。 [HTTP / REFRESH](../REFRESH.md).

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

<a id="migration"></a>

## 移行

SHM レイアウト変更には完全再起動が必要です。メソッド／ACL 構文とプロトコルハンドラーは維持します。v4 は既定で JSON を使用し、ヘッダーと本文に一致する結果コードを返します。未登録のキーは `200 no_match` となり、`cache_purge_legacy_status` は警告のみ出します。[HTTP 応答仕様](../HTTP_RESPONSES.md)に合わせてクライアントを更新してくださ い。容量は割り当て済みタスクも含み、過負荷時は受け付けを待ちます。実行中の重複は再走査を予約できます。

<a id="security"></a>

## セキュリティ

- `allow` / `deny` または上流の認証で PURGE エンドポイントを制限し、公開しないでください。
- リクエスト集中時の SHM 消費を抑えるため、負荷に合った `cache_purge_queue_size` を選んでください。
- 通常トラフィックと独立して制限するため、PURGE location に `limit_req` を併用することも検討してください。

<a id="documentation"></a>

## ドキュメント

- [ディレクティブ一覧](DIRECTIVES.ja.md)
- [その他の設定例](EXAMPLES.ja.md)
- [運用とトラブル対応（英語）](../OPERATIONS.md)
- [HTTP と REFRESH の制約（英語）](../REFRESH.md)
- [v4 への移行（英語）](../MIGRATION.md)
- [実装アーキテクチャ（英語）](../ARCHITECTURE.md)
- [開発とテスト（英語）](../../t/TESTING.md)

<a id="see-also"></a>

## 関連情報

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [nginx documentation](https://nginx.org/en/docs/)

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
