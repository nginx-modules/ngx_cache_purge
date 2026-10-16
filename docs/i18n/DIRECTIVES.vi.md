[ngx_cache_purge](README.vi.md)

<a id="directives"></a>

# Chỉ thị

<a id="proxy_cache_purge"></a>

### `proxy_cache_purge`

```
Syntax:  proxy_cache_purge on | off;
         proxy_cache_purge <method> [purge_all] from all | <cidr> ...;
         proxy_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

**Dạng tại location phục vụ** (`from …`) chặn phương thức HTTP đã chọn và xóa mục tương ứng. `on` viết tắt cho `PURGE`, `off` tắt. Giới hạn bằng CIDR hoặc dùng `from all`. `purge_all` trước `from` xóa toàn vùng bất kể URI.

**Location riêng** (vùng/khóa) chỉ được dùng trong location xóa, thường có regex bắt khóa. Tìm vùng theo tên và xóa theo biểu thức đã biên dịch. Không dùng chung với `proxy_cache`, `proxy_pass` trong cùng location.

<a id="fastcgi_cache_purge"></a>

### `fastcgi_cache_purge`

```
Syntax:  fastcgi_cache_purge on | off;
         fastcgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         fastcgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Tương đương `proxy_cache_purge` cho vùng `fastcgi_cache` / `fastcgi_cache_path`.

<a id="scgi_cache_purge"></a>

### `scgi_cache_purge`

```
Syntax:  scgi_cache_purge on | off;
         scgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         scgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Tương đương `proxy_cache_purge` cho vùng `scgi_cache` / `scgi_cache_path`.

<a id="uwsgi_cache_purge"></a>

### `uwsgi_cache_purge`

```
Syntax:  uwsgi_cache_purge on | off;
         uwsgi_cache_purge <method> [purge_all] from all | <cidr> ...;
         uwsgi_cache_purge <zone> "<key_expression>";
Default: off
Context: http, server, location
```

Tương đương `proxy_cache_purge` cho vùng `uwsgi_cache` / `uwsgi_cache_path`.

<a id="cache_purge_response_type"></a>

### `cache_purge_response_type`

```
Syntax:  cache_purge_response_type json | html | xml | text | none;
Default: json
Context: http, server, location
```

Chọn định dạng nội dung phản hồi PURGE thành công; mặc định là JSON. `Cache-Purge-Code` trùng với mã kết quả trong nội dung. Phản hồi của mô-đun có `Cache-Control: no-store`; lỗi dùng `application/problem+json` theo RFC 9457. Khóa văn bản được escape theo định dạng; khóa nhị phân dùng Base64 kèm thông tin mã hóa.

`none` bỏ nội dung PURGE: thao tác thành công đã hoàn tất trả `204`, công việc nền đã được chấp nhận vẫn trả `202`, lỗi giữ mã 4xx/5xx. Điểm cuối trạng thái và REFRESH vẫn dùng JSON có cấu trúc. Xem mã, trường dữ liệu và giới hạn trong [quy ước HTTP](../HTTP_RESPONSES.md).

<a id="cache_purge_background_queue"></a>

### `cache_purge_background_queue`

```
Syntax:  cache_purge_background_queue on | off;
Default: off
Context: http
```

Bật xử lý trì hoãn ký tự đại diện và `purge_all`. Trả `202 Accepted` sau vào hàng đợi hoặc hợp nhất với tác vụ giống nhau đang chờ/chạy. Không đổi xóa khóa chính xác.

Điều khiển hành vi cũ của wildcard/toàn cache. Liên kết `cache_purge_key` luôn dùng dọn vật lý trì hoãn và `200` logic, kể cả khi tắt tùy chọn này.

Hết chỗ hoặc lỗi cấp phát giữ yêu cầu mở tới khi được nhận, không duyệt khi chờ. Bản trùng của tác vụ chờ/chạy có thể nhận khi đầy. Không cấp được trạng thái chờ nhỏ thì trả `500`. Dọn khi ngắt kết nối hủy timer và giảm số đang chờ.

Với cờ này và `cache_purge_vary_aware`, giá trị khai báo cuối cùng có hiệu lực. Giá trị không hợp lệ bị từ chối. `cache_purge_legacy_status` đã lỗi thời và bị bỏ qua. Luồng và vòng đời: [mô hình thực thi](../ARCHITECTURE.md#execution-model).

<a id="cache_purge_queue_size"></a>

### `cache_purge_queue_size`

```
Syntax:  cache_purge_queue_size <number>;
Default: 1024
Minimum: 1
Context: http
```

Số tác vụ chưa xong tối đa trên mọi đường dẫn, cả chờ và đã giao. Bản trùng dùng chung một cấp phát; lượt lặp giữ bộ nhớ và suất. Tác vụ khác chờ chỗ trống.

Vùng chung được tạo khi cấu hình nền hoặc endpoint trạng thái. Đổi dung lượng cần restart ngay cả khi kích thước làm tròn bằng nhau. Reload từ chối đổi kích thước để giữ việc đã nhận. Thêm gốc cache cũng có thể cần bộ nhớ/restart; hoàn tất trước công việc chờ/hoạt động cần giữ.

Khai báo không gian tên cũng tạo hàng đợi.

Kích thước vùng gồm tác vụ, siêu dữ liệu đường dẫn và chỉ số worker căn chỉnh; tùy build/số đường dẫn. Xem [tính bộ nhớ](../OPERATIONS.md#memory-sizing).

<a id="cache_purge_thread_pool"></a>

### `cache_purge_thread_pool`

```
Syntax:  cache_purge_thread_pool <name>;
Default: default
Context: http, server, location
```

Chọn bên thực thi tác vụ nền đã nhận theo kế thừa thông thường. Ví dụ:

```nginx
thread_pool purge_io threads=2 max_queue=128;
http {
    cache_purge_background_queue on;
    cache_purge_thread_pool purge_io;
}
```

NGINX xác minh pool, chuyển hoàn tất qua sự kiện tác vụ bản địa; không sao chép cấu trúc riêng hay hàng đợi hoàn tất. Có luồng thì pool tùy chỉnh phải tồn tại. Không luồng/Windows vẫn nhận chỉ thị và dùng lát event loop giới hạn. Pool đầy trì hoãn thực thi, không đưa I/O về vòng lặp. Pool riêng thêm stack luồng và chuyển ngữ cảnh.

<a id="cache_purge_batch_size"></a>

### `cache_purge_batch_size`

```
Syntax:  cache_purge_batch_size <number>;
Default: 10
Minimum: 1
Maximum: 64
Context: http
```

Tối đa tác vụ mỗi worker giữa các khoảng nghỉ. Tiến độ giữ qua lát duyệt và hoàn tất luồng. Hàng đợi trống kết thúc lô. Trên 64 bị chặn kèm cảnh báo; 0 bị từ chối.

Điều khiển tác vụ, không phải tệp hay thời gian callback. Giá trị nhỏ thêm nghỉ giữa tác vụ, không ngắt lượt luồng đang chạy. Dùng với `cache_purge_throttle_ms` để chỉnh tải đĩa.

<a id="cache_purge_throttle_ms"></a>

### `cache_purge_throttle_ms`

```
Syntax:  cache_purge_throttle_ms <time>;
Default: 10ms
Minimum: 1ms
Context: http
```

Khoảng nghỉ giữa lô. Chấp nhận `10ms`, `500ms`, `1s` và dạng ghép trong ngoặc kép như `"2s 500ms"`. Số nguyên không đơn vị là mili giây: `10` bằng `10ms`. Thiếu đơn vị sinh cảnh báo; ghi rõ để tránh.

Worker rảnh kiểm tra lại sau mười lần khoảng này. Lát chưa xong và mutex bận dùng timer 1 ms. Đây là khoảng lập lịch tối thiểu, không phải hạn chót. Từ chối 0 và giá trị ngoài phạm vi an toàn của nền tảng.

Tiếp nhận, áp lực pool và thử dọn trì hoãn dùng polling 10 ms.

<a id="cache_purge_queue_status"></a>

### `cache_purge_queue_status`

```
Syntax:  cache_purge_queue_status <cache_path>;
Default: not configured
Context: location
```

Cung cấp snapshot JSON GET/HEAD với `Cache-Control: no-store`. Đường dẫn phải là gốc cache cấu hình; đường dẫn lạ khiến nạp cấu hình thất bại dù nền tắt. Hạn chế truy cập:

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

`queue` mô tả hàng đợi chung của mọi đường dẫn:

| Trường | Kiểu / đơn vị | Ý nghĩa |
|---|---|---|
| `size` | Số nguyên / tác vụ | Đang chờ worker |
| `running` | Số nguyên / tác vụ | Đã giao, gồm chờ pool |
| `max_size` | Số nguyên / tác vụ | Tổng sức chứa chờ và đã giao |
| `wait_time` | Số nguyên / mili giây | Tuổi chờ lâu nhất; 0 khi trống |
| `full` | Boolean | Mẫu tổng chờ/đã giao đạt sức chứa |

`cache` mô tả gốc được location chọn:

| Trường | Kiểu / đơn vị | Ý nghĩa |
|---|---|---|
| `pending.total` | Số nguyên / tác vụ | Tác vụ chờ của cache này |
| `pending.full` | Số nguyên / tác vụ | `purge_all` chờ, nằm trong `pending.total` |
| `active` | Số nguyên / tác vụ | Đã giao tới khi ghi nhận hoàn tất xong |
| `requests.overflow` | Số nguyên / yêu cầu | Đã vào chờ tiếp nhận, gồm áp lực khóa/cấp phát |
| `requests.deduplicated` | Số nguyên / yêu cầu | Hợp nhất với công việc chờ/đang chạy |
| `requests.waiting` | Số nguyên / yêu cầu | Hiện đang chờ tiếp nhận cho cache này |
| `files.deleted` | Số nguyên / tệp | Xóa nền thành công báo cáo cuối lượt |
| `files.errors` | Số nguyên / lỗi | Xóa thất bại, trừ tệp đã mất |
| `files.bytes` | Số nguyên / byte | Kích thước logic ước tính của tệp đã xóa |
| `responses.hit`, `responses.miss` | Số nguyên / yêu cầu chính | Quyết định HIT/MISS lõi quan sát ở pha log |
| `responses.hit_ratio` | Số / 0..1 | HIT / (HIT + MISS), 0 nếu không có quan sát |
| `logical.invalidations` | Số nguyên / quyết định | Vô hiệu hóa đã công bố, gồm trùng lặp và REFRESH |
| `cleanup.scan_errors` | Số nguyên / lỗi | Lỗi siêu dữ liệu/mở/đọc/đóng/cấp phát nền |
| `cleanup.retries` | Số nguyên / lượt | Lượt thêm do dọn dẹp lỗi |
| `cleanup.exhausted` | Số nguyên / tác vụ | Tác vụ bỏ sau ba lần thử lại thất bại |

`pending.full` đếm xóa toàn cache; wildcard khóa `*` là loại khác. `active` gồm chờ pool, không thuộc `queue.size`/`pending.total` nhưng vẫn chiếm sức chứa.

Lượt quét không gian tên hợp nhất giữ phân loại của tác vụ đầu tiên trong `pending.full`. Scan/probe REFRESH không tính vào độ sâu PURGE hoặc lỗi xóa. Đổi vị trí thử lại trì hoãn có thể làm `wait_time` thành tuổi đầu hàng đợi được lấy mẫu thay vì tuổi chờ tối đa.

Mỗi slot tiến trình có một bên ghi từ event loop; điểm đầu và khoảng cách căn cache line. Luồng trả kết quả qua sự kiện hoàn tất. Bộ đọc trạng thái không dùng mutex hoặc atomic fetch-add, kể cả tăng 0. 64-bit dùng tải/lưu volatile căn chỉnh thường; 32-bit kiểm tra chuỗi có giới hạn bảo vệ đọc 64-bit, có thể bỏ một shard bận khỏi mẫu. Trường là xấp xỉ, không phải giao dịch giữa worker. Tiếp nhận dùng trạng thái điều khiển có khóa, không dùng số liệu giám sát.

Bộ đếm tích lũy bão hòa tại tối đa unsigned 64-bit, giữ qua reload, đặt lại khi restart. Số đang chờ theo vòng đời dọn yêu cầu. Thao tác đồng bộ và xóa bởi cache manager lõi không tính vào tổng nền. Byte ước tính độ dài, không phải block hay bằng chứng lấy lại dung lượng; độc giả đang mở và ghi đồng thời làm thay đổi kết quả. Tính cả kết quả từng phần; worker crash có thể mất kết quả. Không phải nhật ký kiểm toán.

HIT/MISS chỉ đếm yêu cầu chính có quyết định cache bản địa. Thiếu upstream, trạng thái 0, BYPASS, EXPIRED, STALE, UPDATING, REVALIDATED bị loại khỏi tỷ lệ. Yêu cầu vốn không cache không thành MISS. Chỉ giám sát cũng có thể tạo SHM khi nền tắt. Trên 32-bit, tuổi hàng đợi dưới khoảng 24,9 ngày; chênh lệch đồng hồ có dấu âm báo 0.

<a id="cache_purge_legacy_status"></a>

### `cache_purge_legacy_status`

```
Syntax:  cache_purge_legacy_status on | off;
Default: — (obsolete; ignored)
Context: http
```

Chỉ thị đã lỗi thời, chỉ được chấp nhận để chuyển đổi cấu hình. Cả hai giá trị đều phát cảnh báo và bị bỏ qua; hãy xóa chỉ thị. Giá trị không hợp lệ vẫn bị từ chối. Thao tác đồng bộ không có kết quả khớp trả `200 no_match`, dọn dẹp vật lý được chấp nhận trả `202 accepted`, vô hiệu hóa thế hệ đã công bố trả `200 invalidated`. Không còn lựa chọn giữa `412` và `404`. Xem [quy ước HTTP](../HTTP_RESPONSES.md).

<a id="cache_purge_vary_aware"></a>

### `cache_purge_vary_aware`

```
Syntax:  cache_purge_vary_aware on | off;
Default: off
Context: http
```

Với `on`, sau xóa chính xác thành công, xóa tệp chính rồi duyệt xóa tệp còn lại có chuỗi `KEY:` giống nhau. Gồm mọi `Vary`, `gzip_vary` trên đường dẫn khác nhưng chung khóa logic. Byte ngay sau khóa phải là `\n`, tránh khớp sai chỉ do cùng tiền tố.

Mặc định tắt vì mỗi lần PURGE khóa chính xác thêm một lượt duyệt toàn bộ thư mục. Bật khi có nhiều biểu diễn `Vary`/`gzip_vary`. Vẫn duyệt khi mục chính đã không còn. Ký tự đại diện và `purge_all` vốn đã bao gồm các biến thể. Khóa Vary chính xác phân biệt hoa/thường và giới hạn 511 byte; khóa dài hơn trả `400 invalid_selector` trước khi xóa bất kỳ mục nào.

Vô hiệu hóa thế hệ bao quát mọi biểu diễn của khóa mà không cần tùy chọn này.

<a id="cache_purge_namespace"></a>

### `cache_purge_namespace`

```text
Syntax:  cache_purge_namespace <name> <size> <state-directory>;
Default: none
Context: http
```

Khai báo không gian tên trước khi location liên kết. Tên có 1–32 chữ ASCII, số, `_` hoặc `-`; SHM tối thiểu tám trang bản địa. Dùng thư mục trạng thái bền vững riêng ngoài gốc cache/tạm. Lõi tạo marker bền vững độc quyền để cấp epoch restart. Giữ các tệp nhỏ này chừng nào cache tương ứng có thể tồn tại; xóa trạng thái mà giữ cache có thể tái dùng epoch cũ. Không bảo đảm độ bền mất điện/fsync. Reload thường giữ epoch/quy tắc; đổi kích thước/đường dẫn hoặc bỏ không gian tên cần restart đầy đủ. SHM không gian tên và hàng đợi có kích thước riêng.

<a id="cache_purge_key"></a>

### `cache_purge_key`

```text
Syntax:  cache_purge_key <namespace> "<logical-key-expression>";
Default: none; inherited when configured
Context: http, server, location
```

Liên kết location với không gian tên có sẵn, cung cấp `$cache_purge_key`. Dùng biến này làm toàn bộ `proxy_cache_key`, `fastcgi_cache_key`, `scgi_cache_key` hoặc `uwsgi_cache_key`, kể cả location PURGE riêng tương ứng. Nếu khóa bản địa mở rộng khác khóa sinh ra, PURGE trả `500` thay vì xác nhận vô hiệu hóa nghiêm ngặt. Không tham chiếu `$cache_purge_key` trong chính biểu thức logic của nó. Khóa logic tối đa 8096 byte. Chỉ liên kết location tham gia khi nơi khác cần hành vi cũ; không có `off` để bỏ liên kết kế thừa.

<a id="proxy_cache_refresh"></a>

### `proxy_cache_refresh`

```text
Syntax:  proxy_cache_refresh <zone> "<native-key-expression>";
Default: off
Context: location
```

Tạo endpoint REFRESH tường minh chỉ cho yêu cầu chính; phương thức khác nhận `405`. Cần liên kết `cache_purge_key`, biểu thức bản địa `$cache_purge_key` và hồ sơ refresh. Khóa logic chọn khóa chính xác, tiền tố cuối `*` hoặc `*` cho không gian tên tại đường dẫn này. Bảo vệ bằng truy cập/xác thực lõi. REFRESH chỉ hỗ trợ proxy; PURGE hỗ trợ cả bốn giao thức.

<a id="cache_purge_refresh_profile"></a>

### `cache_purge_refresh_profile`

```text
Syntax:  cache_purge_refresh_profile <internal-uri> <logical-key-prefix>;
Default: none
Context: location
```

Ánh xạ một biểu diễn đã biết vào location proxy nội bộ dạng literal. Bỏ tiền tố chính xác có phân biệt hoa thường phải để lại URI nguồn bắt đầu `/`; giữ query string và escape. Mô-đun không đoán host hay tìm `://` trong khóa. Đích phải có `internal`, tắt cache, tắt chuyển tiếp header/body, HEAD, Host literal không rỗng và header biểu diễn literal. Hai header điều kiện dùng đúng `$cache_purge_refresh_etag`, `$cache_purge_refresh_last_modified`; `$cache_purge_refresh_uri` cung cấp URI cho `proxy_pass`. Ba biến thuần này chỉ có nghĩa trong probe. Kiểm tra đích ở preaccess, kể cả chuyển hướng nội bộ. Quản trị viên bảo đảm cùng biểu diễn nguồn với khóa; Vary bị bỏ vì siêu dữ liệu không giữ header yêu cầu gốc.

Đặt `proxy_intercept_errors off` ở location probe. Từ chối chặn lỗi kế thừa để lỗi nguồn không bị viết lại thành thành công gây vô hiệu hóa.

<a id="cache_purge_refresh_timeout"></a>

### `cache_purge_refresh_timeout`

```text
Syntax:  cache_purge_refresh_timeout <time>;
Default: 10s
Context: http, server, location
Range:   1ms .. 1h
```

Giới hạn việc gửi scan/probe mới của trang REFRESH, cả chờ bộ quét worker. Hết thời gian thì dừng việc mới và đánh dấu phản hồi chưa đủ. HEAD đang chạy hoàn tất theo `proxy_connect_timeout`, `proxy_send_timeout`, `proxy_read_timeout`; hãy đặt rõ. Không phải hạn chót cứng cho phản hồi và không ngắt syscall. Ngắt kết nối/dừng nhẹ nhàng dừng gửi mới, giữ bộ nhớ luồng hoạt động tới hoàn tất bản địa.
