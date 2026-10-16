# ngx_cache_purge

[README](README.ja.md) · [ディレクティブ](DIRECTIVES.ja.md)

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
