[ngx_cache_purge](README.ja.md)

<a id="directives"></a>

# ディレクティブ

<a id="proxy_cache_purge"></a>

## `proxy_cache_purge`

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

## `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

`proxy_cache_purge` と同等ですが、`fastcgi_cache` / `fastcgi_cache_path` で設定された FastCGI キャッシュゾーンを対象にします。

<a id="scgi_cache_purge"></a>

## `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

`proxy_cache_purge` と同等ですが、`scgi_cache` / `scgi_cache_path` で設定された SCGI キャッシュゾーンを対象にします。

<a id="uwsgi_cache_purge"></a>

## `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

`proxy_cache_purge` と同等ですが、`uwsgi_cache` / `uwsgi_cache_path` で設定された uWSGI キャッシュゾーンを対象にします。

<a id="cache_purge_response_type"></a>

## `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

成功した PURGE 応答の本文形式を指定します。既定は JSON です。`Cache-Purge-Code` と本文の結果コードは一致します。モジュールの応答には `Cache-Control: no-store` が付き、エラーは RFC 9457 の `application/problem+json` を使用します。テキストキーは形式に応じてエスケープし、バイナリキーは符号化方式を明示した Base64 で表現します。

`none` は PURGE 本文を省略します。完了した成功は `204`、受け付けたバックグラウンド処理は `202`、エラーは 4xx/5xx のままです。状態と REFRESH のエンドポイントは構造化 JSON を維持します。コード、フィールド、制約は [HTTP 応答仕様](../HTTP_RESPONSES.md)を参照してください。

<a id="cache_purge_background_queue"></a>

## `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

ワイルドカードと `purge_all` の遅延処理を有効にします。キューへの登録、または同一の待機中／実行中タスクへの統合後に `202 Accepted` を返します。完全一致キーの PURGE には影響しません。

この設定は従来のワイルドカード／全件削除を制御します。`cache_purge_key` を設定した場所では、この設定が off でも物理削除は遅延実行され、論理的な完了として `200` を返します。

キューが満杯、またはタスクのメモリ確保に失敗した場合、受け付けが成功するまでリクエストを保持します。待機中にディレクトリを走査しません。既存の待機中／実行中タス クの重複は満杯でも受け付けられます。小さな待機用コンテキストを確保できない場合は `500` を返します。切断時の cleanup はタイマーを解除し、待機数を減らします。

このフラグと `cache_purge_vary_aware` は最後に指定した値を使います。不正な値は拒否します。`cache_purge_legacy_status` は廃止済みで無視されます。スレッドとライフサイクルは[実行モデル](../ARCHITECTURE.md#execution-model)を参照してください。

<a id="cache_purge_queue_size"></a>

## `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

全キャッシュパスを合わせた未完了タスク数の上限です。待機中と割り当て済みの両方が容量を消費します。重複は同じメモリ領域を使い、再走査もその領域と容量枠を維持しま す。異なるタスクは空き枠を待ちます。

バックグラウンド処理またはステータスエンドポイントを設定すると共有ゾーンを作成します。丸め後のゾーンサイズが同じでも、容量変更には再起動が必要です。reload は受け付け済み作業を保護するためサイズ変更を拒否します。キャッシュルートの追加も、メモリ増加と再起動を必要とする場合があります。完了が必要な待機／実行中作業は先 に処理してください。

名前空間の宣言でもタスクキューが作成されます。

ゾーンサイズにはタスク領域、パスメタデータ、整列されたワーカーメトリクスが含まれます。ビルドとキャッシュパス数に依存します。[メモリ計算](../OPERATIONS.md#memory-sizing)を参照し てください。

<a id="cache_purge_thread_pool"></a>

## `cache_purge_thread_pool`

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

NGINX が名前付きプールを検証し、ネイティブのタスクイベントで完了を通知します。非公開のプール構造や独自の完了キューは複製しません。スレッド対応時は独自名の プールが存在する必要があります。スレッド非対応または Windows では設定を受け付け、作業量を制限したイベントループのスライスを使います。選択プールの満杯は実行を遅延させ、I/O をイベントループに戻しません。専用プールは追加のスレッドスタックとコンテキストスイッチを必要とします。

<a id="cache_purge_batch_size"></a>

## `cache_purge_batch_size`

```
Syntax:  cache_purge_batch_size <number>;
Default: 10
Minimum: 1
Maximum: 64
Context: http
```

ワーカーが休止するまでに処理する最大タスク数です。走査スライスやスレッド完了をまたいで進捗を保持します。空のキューで現在のバッチを終了します。64 を超える値は警告付きで 64 に制限し、0 は拒否します。

制御するのはタスク数であり、ファイル数やコールバック時間ではありません。小さい値はタスク間の休止を増やしますが、実行中のスレッド走査を中断しません。ディスク負荷 の調整には `cache_purge_throttle_ms` と併用します。

<a id="cache_purge_throttle_ms"></a>

## `cache_purge_throttle_ms`

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

## `cache_purge_queue_status`

```
Syntax:  cache_purge_queue_status <cache_path>;
Default: not configured
Context: location
```

GET/HEAD に `Cache-Control: no-store` 付き JSON スナップショットを返します。パスは設定済みのキャッシュルートでなければ なりません。未知のパスは、バックグラウンド処理が無効でも設定エラーになります。エンドポイントへのアクセスを制限してください。

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

`namespaces` は、この location が選択したキャッシュに関係なく、設定済みの `cache_purge_namespace` を返します。同じ名前空間を使うすべてのキャッシュパスは世代状態を共有します。各エントリは、その名前空間のロックを保持してコピーされます。取得はエントリごとに行われるため、リスト全体が単一時点のスナップショットになるわけではありません。

| フィールド | 型／単位 | 意味 |
|---|---|---|
| `name` | 文字列 | キャッシュキーのプレフィックス `p4:<name>:` に含まれる名前空間名 |
| `epoch` | 文字列／10 桁の数字 | 現在の名前空間エポック。完全な再起動で新しい値になる |
| `baseline` | 文字列／16 桁の小文字の 16 進数 | ルールで引き上げられていないキーの世代。名前空間全体の無効化で `serial` に設定される |
| `serial` | 文字列／16 桁の小文字の 16 進数 | 1 から始まり、このエポックで無効化が公開されるたびに増えるシーケンス番号 |
| `rules` | 真偽値 | 完全一致またはプレフィックスのルールが存在すれば `true`。名前空間全体の無効化で消去される。共有メモリ不足による無効化範囲の拡大も含む |

`baseline` と `serial` はキャッシュキーの世代と同じ固定桁数で、文字列として比較できます。応答に収めるためリストを打ち切った場合、`namespaces_complete` は `false` になります。返されたエントリは引き続き利用でき、欠落した名前空間は不明として扱います。[世代モードでのキャッシュファイルの読み取り](OPERATIONS.ja.md#reading-cache-files)を参照してください。

`pending.full` は全件削除操作を数えます。キーが `*` のワイルドカードは別の操作種別です。`active` にはプール待機も含みます。実行中タスクは `queue.size` と `pending.total` からは除外されますが、受け付け容量を消費します。

名前空間走査の集約時、`pending.full` は最初に受け付けたタスクの分類を示します。REFRESH の走査／プローブは PURGE キュー長や削除エラー数に含まれません。遅延再試行でキュー順が変わると、`wait_time` は最長待機時間ではなく取得時の先頭要素の経過時間になる場合があります。

各プロセススロットの書き手は 1 つのイベントループだけで、先頭とストライドをキャッシュラインに整列します。スレッドは完了イベントで結果を返します。ステータスの読み手はキュー mutex を取得せず、加算量 0 を含め atomic fetch-add を使いません。64 ビットビルドでは、カウンターに対して整列された通常の volatile 読み書きを行います。32 ビットビルドでは回数制限付きのシーケンス確認で 64 ビット読み取りを保護し、書き込み中のシャードが 1 回のサンプルから除外される場 合があります。各値は近似であり、ワーカー間のトランザクションではありません。受け付け判断は必ずロック下の制御状態を使い、監視値には依存しません。

累積カウンターは符号なし 64 ビットの最大値で飽和し、通常の reload を越えて維持し、再起動でリセットします。待機数はリクエストの cleanup に追 従します。同期操作とコアのキャッシュマネージャーによる削除は、バックグラウンド削除総数に含みません。バイト数はファイル長の推定であり、ファイルシステムのブロック 数や実際の空き容量の証明ではありません。開いたままの読み手や同時書き込みが結果を変えるためです。途中までの走査結果も集計しますが、クラッシュしたワーカーの結果は 欠ける場合があります。監査ログとしては使用できません。

HIT/MISS はネイティブのキャッシュ判定を持つメインリクエストだけを集計します。upstream 不在、キャッシュ状態 0、BYPASS、EXPIRED、STALE、UPDATING、REVALIDATED は比率から除外します。キャッシュを使わない通常のリクエストを MISS にしません。監視だけでも、バックグラウンド PURGE が無効な状態で SHM ゾーンを作成できます。32 ビットシステムではキューの経過時間は約 24.9 日未満の間隔を対象とし、符号付き差分が負なら 0 として報告します。

<a id="cache_purge_legacy_status"></a>

## `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

設定移行のためだけに受け付ける廃止済みディレクティブです。どちらの値も警告を出し、動作には影響しません。設定から削除してください。不正な値は拒否します。同期処理 で一致がなければ `200 no_match`、物理削除の受け付けは `202 accepted`、世代無効化の公開は `200 invalidated` です。`412` と `404` の切り替えは廃止しました。[HTTP 応答仕様](../HTTP_RESPONSES.md)を参照してください。

<a id="cache_purge_vary_aware"></a>

## `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

`on` の場合、完全一致 PURGE が成功して主ファイルを削除した後、キャッシュディレクトリを走査し、同じ `KEY:` 文字列を含む残りのファイルを削除します。これにより、異なるパスに保存されても同じ論理キーを共有する `Vary` と `gzip_vary` のバリアントを対象にします。キー直後のバイトが `\n` であることを確認し、共通接頭辞だけを持つ別キーへの誤一致を防ぎます。

完全一致の PURGE ごとに全ディレクトリー走査を追加するため、既定では無効です。複数の `Vary`/`gzip_vary` 表現がある場合に有効にしてください。主エントリーが既にない場合も走査します。ワイルドカードと `purge_all` は元から派生ファイルを含みます。完全一致の Vary キーは大文字小文字を区別し、上限は 511 バイトです。超過すると削除前に `400 invalid_selector` を返します。

世代による無効化は、この設定なしでも論理キーの全表現に適用されます。

<a id="cache_purge_namespace"></a>

## `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

location が参照する前に世代の名前空間を宣言します。名前は ASCII 英数字・`_`・`-` の 1～32 文字、SHM は最低 8 ネイティブページです。キャッシュと一時領域の外に専用の永続状態ディレクトリを置いてください。core は排他的な永続マーカーファイルで再起動 epoch を割り当てます。対応するキャッシュが存在し得る限りマーカーを保持してください。キャッシュを残して状態だけを削除すると古い epoch を再利用する可能性があります。電源断や fsync による永続性は保証しません。通常の reload では epoch とルールを維持し、サイズ・パス変更や名前空間の削除には完全再起動が必要です。名前空間とタスクキューの SHM サイズは独立しています。

<a id="cache_purge_key"></a>

## `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

location を既存の名前空間に結び付け、`$cache_purge_key` を提供します。`proxy_cache_key`、`fastcgi_cache_key`、`scgi_cache_key`、`uwsgi_cache_key` の全体をこの変数に設定し、対応する別 PURGE location にも同じ設定を適用します。展開したネイティブキーが生成キーと異なる PURGE は、厳密な無効化を装わず `500` で失敗します。論理式内で `$cache_purge_key` 自身を参照しないでください。論理キーは最大 8096 バイトです。他の location で従来の動作が必要なら参加する location のみに設定してください。継承した設定を解除する `off` はありません。

<a id="proxy_cache_refresh"></a>

## `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

メイン要求専用の明示的な REFRESH エンドポイントを作成し、他のメソッドには `405` を返します。`cache_purge_key` の設定、ネイティブ式としての `$cache_purge_key`、refresh プロファイルが必要です。論理キーで完全一致、末尾 `*` のプレフィックス、または `*` でこのキャッシュパスの名前空間全体を選択します。core のアクセス制御／認証で保護してください。REFRESH は proxy キャッシュのみ、PURGE は引き続き全 4 プロトコルに対応します。

<a id="cache_purge_refresh_profile"></a>

## `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

既知の一つの表現を、固定の内部 proxy location に対応付けます。論理キーから大文字小文字を区別した完全一致のプレフィックスを除くと、`/` で始まる origin URI になる必要があります。query とエスケープは保持します。ホストを推測したり、キー内の `://` を探したりはしません。転送先には `internal`、cache off、要求ヘッダー／本文の転送 off、HEAD、空でない固定 Host、固定の表現ヘッダーが必要です。条件ヘッダーには `$cache_purge_refresh_etag` と `$cache_purge_refresh_last_modified` をそのまま使います。`$cache_purge_refresh_uri` は対応する URI を `proxy_pass` に渡します。この 3 変数は副作用のない取得専用で、プローブ内のみ有効です。内部リダイレクトも preaccess で検証します。管理者がキャッシュキーと同じ origin 表現になることを確認してください。Vary は元の要求ヘッダーがネイティブメタデータに残らないためスキップします。

プローブ location では `proxy_intercept_errors off` を設定してください。origin エラーが成功に書き換えられて失効を起こさないよう、継承した interception は拒否します。

<a id="cache_purge_refresh_timeout"></a>

## `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

worker のスキャナー待機も含め、REFRESH ページの新規走査／プローブ開始を制限します。期限後は新しい処理を始めず、不完全な応答とします。実行中の HEAD は core の `proxy_connect_timeout`、`proxy_send_timeout`、`proxy_read_timeout` に従って終了するため、明示的に設定してください。厳密な応答期限ではなく、ファイルシステムのシステムコールを中断できません。切断と graceful shutdown では新規開始を止め、実行中スレッドの領域は core の完了通知まで保持します。
