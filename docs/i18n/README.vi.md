<a id="ngx_cache_purge"></a>

# ngx_cache_purge

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

Mô-đun NGINX dùng để xóa mục trong bộ nhớ đệm `FastCGI`, `proxy`, `SCGI` và `uWSGI`. Trong v4, thế hệ khóa ngăn yêu cầu mới dùng mục cũ ngay cả khi tệp chưa bị xóa. REFRESH hỏi máy chủ gốc xem nội dung trong bộ nhớ đệm proxy đã thay đổi chưa. Cả hai tính năng đều cần cấu hình riêng. Cú pháp PURGE được giữ lại, nhưng phản hồi HTTP thay đổi; xem [chuyển đổi](#migration).

---

<a id="contents"></a>

## Mục lục

- [Tính năng](#features)
- [Chọn cách xử lý phù hợp](#choosing-a-mode)
- [Tương thích](#compatibility)
- [Cài đặt](#installation)
- [Xóa theo một phần khóa](#partial-key-purge)
- [Vô hiệu hóa theo thế hệ](#generation-invalidation)
- [Kiểm tra bộ nhớ đệm với máy chủ gốc (REFRESH)](#conditional-refresh)
- [Cấu hình mẫu](#sample-configurations)
- [Chỉ thị](#directives)
- [Mô hình thực thi](#execution-model)
- [Điều chỉnh hiệu năng](#performance-tuning)
- [Giám sát và gỡ lỗi](#monitoring-and-debugging)
- [Khắc phục sự cố](#troubleshooting)
- [Kiểm thử](#testing)
- [Chuyển đổi](#migration)
- [Thư mục tạm](#temporary-directories)
- [Bảo mật](#security)
- [Giấy phép](#license)

---

<a id="features"></a>

## Tính năng

- **Xóa tại location phục vụ** — phương thức HTTP riêng (`PURGE`) với kiểm soát IP
- **Location xóa riêng** — cú pháp vùng/khóa `proxy_cache_purge zone key`, dùng khóa bắt bởi biểu thức chính quy mà không cần `proxy_pass`
- **Ký tự đại diện / xóa một phần** — `*` cuối khóa duyệt thư mục và xóa mục khớp
- **`purge_all`** — xóa toàn bộ vùng trong một yêu cầu
- **Hàng đợi nền** — duyệt trì hoãn theo lát vòng lặp sự kiện giới hạn hoặc pool luồng NGINX tùy chọn; cấu hình được lô và khoảng nghỉ
- **Nhận biết Vary** — sau xóa khóa chính xác, xóa các biến thể tệp gzip/Vary cùng khóa
- **Định dạng phản hồi** — `json` (mặc định), `html`, `xml`, `text`, `none`
- **Pool có tên** — lựa chọn được kế thừa, chuyển sang xử lý từng bước khi không có luồng hoặc trên Windows
- **Khử trùng lặp tác vụ đang chạy** — một thao tác có chỉ mục và tối đa một lượt chạy lại chờ cho yêu cầu đến trong lúc duyệt
- **Trạng thái** — số liệu hàng đợi, xóa và HIT/MISS theo cache, đọc không khóa mutex dùng chung
- **Khóa thế hệ** — vô hiệu hóa chính xác, theo tiền tố hoặc không gian tên mà không thực hiện I/O đĩa trong đường xử lý yêu cầu; bản ghi cũ hoàn tất muộn vẫn không truy cập được
- [Kiểm tra bộ nhớ đệm với máy chủ gốc (REFRESH)](#conditional-refresh)

---

<a id="choosing-a-mode"></a>

## Chọn cách xử lý phù hợp

| Mục tiêu | Cấu hình | Ý nghĩa phản hồi |
|---|---|---|
| Xóa mục đã lỗi thời và chờ thao tác xóa | PURGE khi tắt hàng đợi nền | `200 purged` hoặc `200 no_match`; lỗi trả về 4xx/5xx |
| Lên lịch dọn theo tiền tố hoặc toàn bộ bộ nhớ đệm | PURGE với `cache_purge_background_queue on` | `202 accepted`; mục cũ vẫn có thể được phục vụ trong lúc chờ |
| Không cho yêu cầu mới dùng nội dung cũ, kể cả tệp được ghi muộn | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`; dùng thế hệ khóa mới, xóa tệp sau |
| Kiểm tra thay đổi tại máy chủ gốc | Thế hệ khóa + `proxy_cache_refresh` + hồ sơ nội bộ | `200` kèm kết quả từng mục và thông tin tiếp tục; không nạp sẵn nội dung |

Có thể kết hợp các cơ chế: đổi thế hệ cũng lên lịch dọn tệp. Chỉ bật hàng đợi không ngăn được tác động của việc ghi muộn. Xem [ví dụ v4](#generation-and-refresh-example), [ví dụ PURGE](#sample-configurations) và [phản hồi HTTP](../HTTP_RESPONSES.md) (tiếng Anh).

---

<a id="compatibility"></a>

## Tương thích

Ma trận liệt kê đích đã cấu hình trong CI. Một job thành công xác nhận cấu hình mã nguồn/build đó, không phải mọi bản phát hành cùng dòng.

| NGINX | CI: push / PR | CI: + full |
|---|---|---|
| 1.30.5, 1.31.6 | `test-matrix` | — |
| 1.31.6 | `build-compatibility` (GCC/Clang), `sanitizer`, `native-macos`, `native-windows` (x64/x86) | `memory-leak-test` (Valgrind) |
| 1.7.9 | `build-compatibility` (GCC) | — |
| 1.7.11, 1.7.12 | `build-compatibility` (GCC) | — |
| 1.20.2, 1.26.3, 1.28.3 | — | `test-matrix` |
| 1.11.4, 1.11.5 | — | `build-compatibility` (GCC) |

Job Linux dùng cùng image Docker và điểm chạy kiểm thử như chạy cục bộ. Job bản địa macOS/Windows kiểm tra API tệp và tiến trình tương ứng; Windows build bằng MSVC, kiểm tra HTTP, đường dẫn và reload của `nginx.exe`. Kịch bản tiến trình/tín hiệu POSIX chạy trên Linux/macOS. Windows không chạy chúng hoặc nhánh pool luồng NGINX tùy chọn.

Không có `--with-threads` thì duyệt từng bước. Bản dựng epoll trước NGINX 1.7.12 cũng dùng chế độ này để tránh lỗi tranh chấp khi thông báo trong lõi. BSD chưa có CI bản địa. Lần chạy thường kiểm tra stable/mainline hiện tại và ranh giới API; lần chạy mở rộng thêm phiên bản cũ và Valgrind. Thay đổi chỉ về tài liệu bỏ qua biên dịch. Lịch chạy và lệnh: [kiểm thử](../../t/TESTING.md).

Mã được tách theo cấu hình, xử lý yêu cầu, hàng đợi, duyệt, vô hiệu hóa, REFRESH và phản hồi. Hàm đường dẫn chung nằm trong `ngx_cache_purge.h`, thao tác nền tảng trong header POSIX/Win32. `generation.*` đổi tên thành `invalidation.*`; thế hệ vẫn biểu thị phiên bản khóa. Xem [kiến trúc](../ARCHITECTURE.md).

Mô-đun theo phong cách NGINX tương thích C89 và API sự kiện, slab, rbtree, nguyên tử của lõi. Không yêu cầu atomics C11 hoặc API hệ thống tệp mới. Build GNU89 nghiêm ngặt kiểm tra thứ tự khai báo và cảnh báo; compiler/nền tảng thực tế được ghi cùng kết quả.

---

<a id="installation"></a>

## Cài đặt

```bash
cd /path/to/nginx-source
./configure --add-module=/path/to/ngx_cache_purge
make
make install
```

Mô-đun động:

```bash
./configure --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

---

<a id="partial-key-purge"></a>

## Xóa theo một phần khóa

Thêm `*` vào **khóa cache đã được tính giá trị** để khớp tiền tố không phân biệt hoa/thường. Khóa chính xác và Vary vẫn phân biệt hoa/thường. Tiền tố dọn dẹp vật lý tối đa 511 byte, không tính `*` cuối. Tiền tố dài hơn được ghi log một lần và trả `400 invalid_selector` mà không duyệt tệp.

Với `cache_purge_key`, so biểu thức logic, tối đa 8096 byte tính cả `*` cuối.

Với biểu thức kết thúc bằng `$uri`, yêu cầu này chọn tiền tố URI:

```http
PURGE /images/header*
```

Dấu sao phải là byte cuối khóa đã tính, không chỉ cuối đường dẫn URI. Xét cả query string hoặc hậu tố biến trong `proxy_cache_key`. Khóa chỉ có `*` duyệt mọi mục hợp lệ.

---

<a id="generation-invalidation"></a>

## Vô hiệu hóa theo thế hệ

Khóa logic xác định một đối tượng. Không gian tên nhóm các khóa dùng chung quy tắc vô hiệu hóa. Thế hệ là số phiên bản được thêm vào khóa bộ nhớ đệm NGINX. Sau PURGE, yêu cầu mới dùng phiên bản mới. GET bắt đầu trước đó vẫn có thể hoàn tất, nhưng tệp ghi muộn của nó không tạo HIT cho yêu cầu mới.

Trong không gian tên đã liên kết, PURGE công bố thế hệ mới và lên lịch quét thế hệ cũ đã khử trùng lặp trước khi trả `200` với `invalidated`. Đường tiếp nhận không mở/xóa tệp cache. Yêu cầu mới lần đầu tính khóa sau công bố không thể HIT thế hệ trước. Mỗi yêu cầu ghim thế hệ theo không gian tên/khóa logic, cả chuyển hướng nội bộ; phản hồi đang chạy có thể xong nhưng bản ghi muộn không thành HIT cho yêu cầu mới. Khớp chính xác phân biệt hoa thường; tiền tố bỏ phân biệt ASCII. `purge_all` đổi toàn không gian tên, kể cả đường khác cố ý chia sẻ. Dọn vật lý chỉ xếp cho đường đích; đường khác dùng hết hạn lõi hoặc purge sau.

Vô hiệu hóa được nhận không hứa thu hồi đĩa ngay. Bản ghi cũ muộn sau quét vẫn không truy cập được, sẽ bị xóa lần dọn sau hoặc hết hạn inactive lõi. Khi SHM thiếu, quy tắc gộp về nền mới của không gian tên: thêm khóa trở thành lạnh, không làm khóa vô hiệu hóa sống lại. Mỗi vô hiệu hóa sau thu hồi tối đa 64 nút trie đã tách. PURGE liên tiếp khóa phổ biến không làm cây tăng vô hạn. Chi phí theo độ dài khóa giới hạn và chỉ mục, không theo số tệp; không hứa O(1) thời gian thực hay tránh starvation khi quá tải. Tiếp nhận có thể chờ. Dùng `proxy_cache_lock` và tương đương để quản lý đợt GET sau vô hiệu hóa.

<a id="conditional-refresh"></a>

## Kiểm tra bộ nhớ đệm với máy chủ gốc (REFRESH)

Dùng REFRESH khi đối tượng đã có trong bộ nhớ đệm nhưng chưa rõ nguồn có thay đổi hay không, chẳng hạn sau khi nhập danh mục. Nếu biết nội dung đã lỗi thời, dùng PURGE. REFRESH gửi HEAD, không tải phần thân, và dùng ETag hoặc thời điểm sửa đổi đã lưu làm điều kiện HTTP. Hồ sơ là một location proxy nội bộ xác định máy chủ gốc, Host, các header và cách ánh xạ URI.

REFRESH kiểm tra đối tượng còn cache trước khi hết TTL; không làm nóng trước hoặc tăng TTL với `304`. Bổ sung `proxy_cache_revalidate`, `proxy_cache_background_update` vốn xử lý truy cập bình thường tới đối tượng hết hạn. Nguồn bỏ qua validator và luôn trả `200` biến REFRESH thành vô hiệu hóa cộng HEAD. Chỉ mục hiện tại, hợp lệ, trên đường hash chuẩn là ứng viên. Thay tệp trong cùng thế hệ có thể bị vô hiệu hóa thận trọng; so thế hệ ngăn HEAD cũ vô hiệu hóa thế hệ PURGE mới hơn.

| Kết quả nguồn nguyên gốc | Hành động / kết quả |
|---|---|
| `304` | Giữ; `kept` |
| `200`, `404`, `410` | Vô hiệu hóa thế hệ có điều kiện, dọn trì hoãn; `invalidated` |
| Trạng thái khác | Giữ; `upstream_status` |
| Không có trạng thái / lỗi truyền tải | Giữ; `transport_error` |
| Siêu dữ liệu Vary | Không HEAD; `skipped_vary` |
| Ánh xạ/hồ sơ sai | Giữ; `skipped_profile` |
| Thế hệ đã đổi trong lúc đó | Không vô hiệu hóa mới; `raced_replaced` |
| Lỗi nội bộ vô hiệu hóa/cấp phát | Giữ; `invalidation_error` |
| Chưa gửi/xong khi trang dừng | `undispatched`; con trỏ không vượt qua |

Mỗi worker có tối đa một trang và một probe hoạt động; hạn mức chung 16 probe giữa worker. Trang khác chờ không cấp mảng ứng viên. Mỗi trang duyệt bằng con trỏ chung, giữ tối đa 32 ứng viên rồi gửi HEAD lõi tuần tự, giới hạn cả bộ nhớ lẫn số yêu cầu con chia sẻ pool chính. REFRESH giống nhau không khử trùng lặp, chỉ chia sẻ hạn mức song song. Quyết định vô hiệu hóa dùng hàng đợi PURGE chung đã khử trùng lặp.

Bắt đầu không con trỏ, rồi gửi `X-Cache-Purge-Cursor: <next_cursor>` tới cùng endpoint với cùng bộ chọn/hồ sơ tới khi `complete` đúng. Con trỏ là tên tệp hex 32 ký tự cuối đã xử lý xong; header sai/trùng trả `400`. `scan_complete` nghĩa là liệt kê thư mục thành công; `complete` còn đòi hết trang khớp tiếp theo và hoàn tất mọi quyết định. `timed_out` báo giới hạn thời gian; lỗi quét trả về `500 scan_failed`. `undispatched` đếm ứng viên chưa xong; scan không đủ chỉ là cận dưới, không phải tổng cache. Nếu scan chưa xong, không chạy probe, kết quả rỗng và con trỏ đầu vào giữ nguyên. Chỉ công khai hash tệp và trạng thái upstream nguyên gốc, không khóa/thông tin xác thực.

Mỗi trang quét lại thư mục để chọn hash tiếp: đổi I/O lặp lấy bộ nhớ giới hạn, tổng `O(files × pages)`. Phân trang nhất quán yếu khi thêm/xóa đồng thời; bắt đầu lại nếu cần ảnh đầy đủ ở thời điểm sau. Chọn bộ lọc/timeout phù hợp. Không thích hợp quét cache hoạt động rất lớn với tần suất cao.

<a id="generation-and-refresh-example"></a>

### Ví dụ thế hệ và REFRESH

Đây là đoạn cấu hình cho main/http; giữ lại khối `events` hiện có. Sửa nguồn và đường dẫn cho phù hợp. Nếu không có `--with-threads`, bỏ `thread_pool` và `cache_purge_thread_pool` để dùng quét từng phần.

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

Trước hết hãy GET một đối tượng tồn tại và có thể lưu vào bộ nhớ đệm. Lặp lại đến khi có `X-Cache-Status: HIT`, rồi kiểm tra cùng đối tượng bằng REFRESH. Sau `kept`, GET tiếp theo vẫn HIT; sau `invalidated`, GET truy cập nguồn lại.

```sh
curl -i 'http://127.0.0.1:8080/objects/item'
curl -i -X REFRESH -H 'X-Cache-Key: origin.test/objects/item' \
  'http://127.0.0.1:8080/admin/refresh'
```

Với tiền tố, bắt đầu không có con trỏ. Nếu `complete: false`, lặp lại với giá trị trả về trong `X-Cache-Purge-Cursor`, thay cho `<next_cursor>`. Giữ nguyên bộ chọn. Xử lý lỗi HTTP trước khi tiếp tục.

```sh
curl -i -X REFRESH -H 'X-Cache-Key: origin.test/objects/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: origin.test/objects/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

Nếu biết đối tượng đã lỗi thời, dùng PURGE trực tiếp. Với thế hệ khóa trong ví dụ, `200 invalidated` ngăn yêu cầu mới dùng thế hệ cũ dù tệp vẫn còn.

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/objects/item'
```

Xem [HTTP và trường hợp biên](../REFRESH.md) (tiếng Anh). Lỗi quét trả về `500 scan_failed`, không phải trang tiếp tục có `scan_error: true`.

<a id="sample-configurations"></a>

## Cấu hình mẫu

<a id="basic--inline-purge-method"></a>

### Cơ bản — phương thức xóa trong location phục vụ

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

### Cơ bản — location xóa riêng

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

### Bật hàng đợi nền

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

### Xóa mọi mục

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

### Phản hồi JSON với ghi đè theo location

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

### Xóa nhận biết Vary

```nginx
http {
    cache_purge_vary_aware on;
    gzip_vary              on;
}
```

Sau xóa khóa, mọi biến thể gzip/Vary ở đường dẫn khác cũng tự động bị xóa.

---

<a id="directives"></a>

## Chỉ thị

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

Với cờ này và `cache_purge_vary_aware`, giá trị khai báo cuối cùng có hiệu lực. Giá trị không hợp lệ bị từ chối. `cache_purge_legacy_status` đã lỗi thời và bị bỏ qua. Luồng và vòng đời: [mô hình thực thi](#execution-model).

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

Kích thước vùng gồm tác vụ, siêu dữ liệu đường dẫn và chỉ số worker căn chỉnh; tùy build/số đường dẫn. Xem [tính bộ nhớ](#memory-sizing).

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

---

<a id="execution-model"></a>

## Mô hình thực thi

**Tác vụ** là một thao tác ký tự đại diện hoặc `purge_all`. **Lô** là số tác vụ worker xử lý trước khoảng nghỉ. **Lát** là bước duyệt tăng dần có giới hạn; một tác vụ có thể cần nhiều lát.

Với thế hệ, một lượt quét vật lý trên mỗi đường dẫn/không gian tên hợp nhất các vô hiệu hóa đã nhận. Trang REFRESH là thao tác HTTP riêng, không phải tác vụ hàng đợi xóa.

| Yêu cầu và cấu hình | Duyệt tệp | Hoàn tất HTTP |
|---|---|---|
| Khóa chính xác kiểu cũ, có/không Vary | Worker yêu cầu; Vary thêm duyệt thư mục | Sau thao tác |
| Ký tự đại diện hoặc `purge_all`, tắt hàng đợi | Worker yêu cầu, duyệt đồng bộ | Sau lượt duyệt |
| Tác vụ nền, pool đã chọn | Mỗi worker một tác vụ pool tại một thời điểm | `202` sau tiếp nhận |
| Tác vụ nền không có pool dùng được | Vòng lặp worker, lát tăng dần | `202` sau vào hàng đợi |
| Hết dung lượng/cấp phát | Yêu cầu chờ tiếp nhận, không tự duyệt | `202` khi được nhận |
| PURGE thế hệ, mọi dạng khóa | Quét không gian tên trì hoãn; không mở/xóa cache trong yêu cầu | `200 invalidated` sau công bố và tiếp nhận |
| Endpoint REFRESH | Quét giới hạn và tối đa 32 HEAD tuần tự | JSON `200` với trường hoàn tất/tiếp nối |

Các hàng không có thế hệ mô tả hành vi cũ. Yêu cầu thế hệ chờ dung lượng trả `200`, không phải `202`, sau tiếp nhận.

Mỗi lát dừng sau tối đa 64 lần lặp thư mục hoặc khi quan sát thấy 5 ms đã trôi qua, tính cả mục bỏ qua và cuối thư mục. Không giới hạn thời gian một syscall riêng lẻ.

Để chuyển I/O nền sang luồng, build với `--with-threads` và khai báo pool lõi trong ngữ cảnh **main**, ngoài `http`:

```nginx
thread_pool default threads=2;
```

`cache_purge_thread_pool <name>` chọn pool tồn tại trong `http`, `server` hoặc `location`. Mặc định `default` được NGINX tạo với tham số chuẩn nếu chỉ tham chiếu mà chưa khai báo. Tên tùy chỉnh phải dùng `thread_pool`. Lựa chọn được kế thừa; yêu cầu được nhận đầu tiên chọn bên thực thi công việc hợp nhất. Mỗi worker có pool riêng. Pool đầy thì gửi lại sau 10 ms, giữ suất tiếp nhận và không chuyển sang duyệt đồng bộ. Không có luồng, Windows, chế độ một tiến trình hoặc pool của tác vụ cũ bị xóa khi reload sẽ dùng con trỏ duyệt từng bước.

REFRESH dùng pool đã chọn cho một bộ quét bổ sung mỗi worker. Yêu cầu con HTTP bản địa chạy trong vòng lặp sự kiện; luồng I/O không truy cập trạng thái của chúng.

Mutex slab bảo vệ thay đổi cấu trúc hàng đợi/chỉ mục. Tiếp nhận và callback nền dùng trylock, thử lại bằng timer. Tác vụ chờ **và đang chạy** ở lại rbtree chuỗi của lõi theo đường dẫn; so khóa đầy đủ giải quyết va chạm hash. Chỉ mục có chi phí `O(log Q)` cho `Q` tác vụ chưa xong; tìm đường dẫn tuyến tính theo số đường dẫn đăng ký. Danh sách `ngx_queue_t` theo dõi chờ/hoạt động. Bản trùng đang chạy đầu tiên yêu cầu lặp lại, các bản sau hợp nhất vào đó. Lượt lặp vào cuối hàng đợi sau lượt hiện tại để không làm mất PURGE đến sau. PURGE liên tục có thể gây nhiều lượt nữa. Chỉ trả suất khi kết thúc cuối cùng. I/O và định dạng phản hồi ngoài khóa.

Rbtree chỉ khử trùng lặp dọn dẹp vật lý. Quy tắc thế hệ nằm trong trie byte giới hạn ở vùng SHM riêng. Lượt quét không gian tên nghiêm ngặt có danh tính chỉ mục khác tác vụ khóa cũ; quyết định REFRESH đi cùng đường dọn dẹp.

Hàng đợi đầy hoặc cấp phát thất bại giữ HTTP chờ với timer 10 ms và xử lý ngắt kết nối của lõi. Yêu cầu/handler dọn dẹp ở worker sở hữu; SHM không có con trỏ yêu cầu. Không tắt đọc toàn kết nối HTTP/2 hay HTTP/3. Áp lực hàng đợi không sinh 429/503; PURGE ngắn đã đọc không bảo đảm TCP Zero Window. Chờ vẫn tốn bộ nhớ/kết nối: dùng timeout và chính sách tiếp nhận lõi theo ngân sách kết nối.

Tác vụ đã nhận giữ khóa, pool và bảo vệ đường dẫn tạm qua reload thường. Bản trùng mới cập nhật bảo vệ tác vụ chờ hoặc lượt sau. Tắt nhận mới vẫn giữ công việc đã nhận. Worker dừng nhẹ nhàng hoàn tất lượt hiện tại, không nhận tác vụ khác. Thoát nhanh trả tác vụ tăng dần chưa xong về hàng đợi, không cố hoàn thành duyệt trong hook thoát. Việc đóng pool lõi quản lý vòng đời tác vụ luồng đã gửi.

Epoch PID/slot nhận diện việc giao tác vụ. Mỗi giây, worker kiểm tra tối đa 64 bản ghi hoạt động mỗi lượt phục hồi, đưa lại công việc có chủ đã thoát vào hàng đợi. Đây là phục hồi phân công bỏ dở, không phải nhật ký giao dịch. Crash khi sửa danh sách/slab có thể hỏng trạng thái và cần restart; xóa ngay trước crash có thể thiếu trong chỉ số. Tác vụ chờ/lặp cần worker sống: rút hết hàng đợi trước dừng nếu cần hoàn tất. Hàng đợi không bền vững qua khởi động lại.

Cấu trúc nội bộ không phải ABI nâng cấp. Chuyển build có cấu trúc khác cần restart đầy đủ, sau khi hoàn tất công việc cần giữ. Reload cùng build tái sử dụng vùng.

Xóa theo best effort. `202` xác nhận nhận việc, không phải đã xóa; lượt duyệt xong không phải snapshot nguyên tử. Yêu cầu đồng thời có thể tạo mục trong/sau lượt duyệt; lỗi được ghi log. Duyệt ký tự đại diện/toàn cache xóa tệp mà không cập nhật đồng bộ mọi nút lõi. Xóa chính xác/Vary vô hiệu hóa nút khớp.

Lượt nền lỗi được thử lại sau 100 ms, 1 s và 10 s, tối đa ba lần. Tác vụ trì hoãn giữ suất tiếp nhận/khử trùng lặp nhưng cho tác vụ khác chạy. PURGE mới đặt lại cửa sổ. Hết thử thì bỏ tác vụ và ghi lỗi; xem `cleanup.exhausted`, sửa lưu trữ rồi thử lại. Vô hiệu hóa thế hệ vẫn có hiệu lực khi dọn vật lý lỗi. Tệp mất là race dự kiến; tệp sai định dạng bị bỏ qua, không thử mãi.

---

<a id="performance-tuning"></a>

## Điều chỉnh hiệu năng

`queue_size` giới hạn việc chưa xong, `batch_size` nhóm tác vụ mỗi worker, `throttle_ms` giãn các nhóm. Đo độ trễ, tranh chấp đĩa và tuổi hàng đợi trước/sau chỉnh. Dung lượng lớn không tăng tốc rút hàng đợi.

Đây là cấu hình khởi đầu, không phải tuyên bố hiệu năng phần cứng. Tên cột bỏ tiền tố `cache_purge_`.

| Tải / ngân sách lưu trữ | `queue_size` | `batch_size` | `throttle_ms` |
|---|---:|---:|---:|
| Máy hoặc lưu trữ hạn chế | 512 | 5 | 25ms |
| Điểm xuất phát chung | 2048 | 20 | 10ms |
| Lưu trữ nhanh có năng lực dư đã đo | 8192 | 50 | 5ms |
| Đợt ngắn, xử lý nền thận trọng | 8192 | 5 | 50ms |

Ví dụ cấu hình chung:

```nginx
cache_purge_background_queue on;
cache_purge_queue_size        2048;
cache_purge_batch_size        20;
cache_purge_throttle_ms       10ms;
```

Hàng đợi lớn hấp thụ đợt tải, nhưng tốc độ đến lâu dài cao hơn tốc độ xong vẫn lấp đầy nó. Ước lượng chỗ thêm bằng `max(0, arrival_rate - completion_rate) * burst_seconds` tác vụ, dùng tác vụ/s sau hợp nhất trùng lặp. Chừa dư cho thời gian không đều và việc tồn. Chờ thêm yêu cầu/kết nối mở; tính bộ nhớ và FD khi lưu trữ bão hòa.

<a id="memory-sizing"></a>

### Tính bộ nhớ

Kích thước vùng được tính từ cấu trúc bản địa:

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

| Thành phần | Bộ nhớ dành riêng hoặc sử dụng |
|---|---|
| Dự trữ tác vụ | `Q * S`, gồm làm tròn slab và tên khóa/pool nhúng |
| Số liệu một đường dẫn | `R * NGX_MAX_PROCESSES` cộng căn chỉnh; 192 KiB với bước 192 byte và 1024 slot |
| Điều khiển hàng đợi | `H`, gồm chủ tiến trình và mảng epoch |
| Dự trữ đường dẫn | `M`, gồm tên, gốc rbtree, căn chỉnh, mảng số liệu |
| Vùng tối thiểu | Tám trang bản địa |

Với bố trí LP64 đo được (`I=184`, `H=20576`, bản ghi đường 104 byte, thống kê 144), cache line 64 byte, trang 4 KiB, một đường dài 20 byte:

| Sức chứa | Byte vùng | MiB vùng |
|---:|---:|---:|
| 512 | 1654784 | 1.58 |
| 2048 | 5373952 | 5.12 |
| 8192 | 20242432 | 19.30 |

Bộ nhớ tác vụ thật là `I + index_bytes + 1 + pool_name_bytes` trước làm tròn slab. Chỉ mục nhúng chứa thẻ hành động một byte và khóa cũ hoặc tên không gian thế hệ. `purge_all` cũ chỉ lưu thẻ. Sức chứa hạn chế tổng cấp phát chờ/hoạt động. Kích thước bản địa, cache line, `NGX_MAX_PROCESSES`, đường dẫn quyết định kết quả; Windows/32-bit khác nhau. Bản ghi bảo vệ reload tốn thêm SHM; cấp phát có thể lỗi trước giới hạn tác vụ.

Bộ nhớ tiến trình riêng. Mỗi lượt hoạt động có buffer khóa cố định, buffer đường tái dùng, frame cố định cho mỗi thư mục mở. Frame giữ độ dài tiền tố, không sao đường tổ tiên: `O(depth + longest_path)` độc lập số tệp. Luồng đọc thư mục có buffer nền tảng. Yêu cầu chờ chỉ giữ ngữ cảnh timer/dọn nhỏ, không con trỏ duyệt. Stack luồng và pool lõi có chi phí riêng.

SHM không gian tên có kích thước riêng. Một scanner REFRESH dùng buffer/ngữ cảnh LP64 cố định 283864 byte (32 mục), frame duyệt, tối đa 32 yêu cầu con trong pool HTTP. Xong trang giải phóng yêu cầu; tiếp nối mở yêu cầu mới. Yêu cầu chờ thêm không cấp mảng ứng viên.

<a id="throughput-and-latency"></a>

### Thông lượng và độ trễ

Với worker bận liên tục, xấp xỉ hữu ích:

```text
B = tasks completed in a batch
T = throttle delay in milliseconds
D = elapsed batch processing time in milliseconds, including slice waits
rate_per_worker ~= 1000 * B / (D + T) tasks/s
```

Bỏ `D` cho trần lập lịch `1000 * B / T`: mặc định 1000 tác vụ/s mỗi worker. Không phải tốc độ xóa tệp hay kết quả đo. Mười tác vụ tổng 200 ms với nghỉ 10 ms cho khoảng 47,6 tác vụ/s. Worker dùng chung lưu trữ nên nhân số worker không bảo đảm thông lượng. Worker rảnh có thể mất `10 * T` cộng trễ vòng lặp trước kiểm tra việc mới.

Giảm lô hoặc tăng nghỉ để giảm việc giữa các lần nghỉ. Dùng pool tùy chọn để giảm trễ worker khi I/O. Không thiết lập nào chặn thời lượng syscall hay chia tác vụ luồng thành lô.

---

<a id="monitoring-and-debugging"></a>

## Giám sát và gỡ lỗi

[Endpoint trạng thái](#cache_purge_queue_status) cho thấy chờ, hoạt động, tiếp nhận, trùng lặp, xóa, HIT/MISS. `202 Accepted` xác nhận nhận/hợp nhất, không phải xóa hoặc vô hiệu hóa logic tức thì. Theo dõi trễ yêu cầu cùng tuổi hàng đợi và tranh chấp lưu trữ.

| Mức | Điều kiện |
|---|---|
| `warn` | Khóa quá dài, pool bị bỏ, giá trị bị giới hạn |
| `error` | Từ chối truy cập, lỗi tìm cache/hồ sơ REFRESH, hết thử lại; lỗi pool lõi hoặc đọc Windows |
| `crit` | Lỗi mở/stat/xóa hoặc thao tác thư mục; đọc POSIX lõi |
| `alert` | Lỗi đóng tệp |
| `emerg` | Cấu hình sai, SHM không tương thích, đổi kích thước hoặc cấp phát siêu dữ liệu reload lỗi |
| `debug` | Ngữ cảnh yêu cầu/duyệt bổ sung |

Chẩn đoán dùng hàm log NGINX bản địa và bắt đầu `ngx_cache_purge: `. NGINX thêm mức, thời gian, ID tiến trình/luồng, errno và ngữ cảnh yêu cầu/cấu hình. Lỗi tệp nêu đúng thao tác lỗi và đường trong ngoặc kép; không gian tên/REFRESH nêu đối tượng và lý do từ chối. Dùng parser error log NGINX chuẩn, lọc tiền tố.

Debug dùng `NGX_LOG_DEBUG_HTTP`, cần cả build `--with-debug` lẫn `error_log /var/log/nginx/error.log debug;`.

Áp lực hàng đợi hiện qua số liệu chờ, không cảnh báo mỗi lần thử. Tệp đã biến mất là race dự kiến, không sinh chẩn đoán mô-đun. Thông báo helper lõi — parser, bộ cấp phát, I/O, pool — giữ định dạng lõi; xem toàn bộ log khi tìm lỗi.

```bash
tail -f /var/log/nginx/error.log | grep 'ngx_cache_purge:'
```

Xem [kiểm tra log](../../t/TESTING.md#logging-contracts) để biết chẩn đoán được thử.

---

<a id="troubleshooting"></a>

## Khắc phục sự cố

| Triệu chứng | Kiểm tra/điều chỉnh |
|---|---|
| Trễ worker khi duyệt lớn | Bật hàng đợi và pool I/O tùy chọn. Chính xác/Vary và tắt nền vẫn đồng bộ. |
| Tăng số chờ tiếp nhận | Kiểm tra tốc độ đến, tuổi hàng, pool, đĩa. Sức chứa thêm hấp thụ đợt; đổi kích thước cần restart. |
| iowait cao | Giảm `cache_purge_batch_size`, tăng `cache_purge_throttle_ms`, hạn chế PURGE vào. |
| Hàng đợi rút chậm | Đo thời gian tác vụ/tranh chấp trước tăng lô hay giảm nghỉ. |
| Ứng dụng khách chờ mã cũ `412`/`404` | Xóa `cache_purge_legacy_status`; xử lý `200` với `Cache-Purge-Code: no_match`. |
| Còn biến thể Vary | Bật `cache_purge_vary_aware`; xem kết quả chính và giới hạn 511 byte. |
| Việc đã nhận có vẻ chưa xong | Xem tác vụ hoạt động, phục hồi, lỗi tệp. `202` không xác nhận hoàn tất; nghi hỏng SHM cần restart. |

---

<a id="testing"></a>

## Kiểm thử

[Bản đồ phạm vi kiểm thử](../TEST_COVERAGE.md) (tiếng Anh) nêu các trường hợp đã kiểm tra và phần còn thiếu.

Build và chạy toàn bộ trong Docker:

```sh
make test-all
NGINX_VERSION=1.31.6 make test-all
```

Từ `t/`, các dịch vụ Compose sẵn có vẫn dùng được:

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

Với plugin Compose dùng `docker compose` thay `docker-compose`; Makefile nhận cả hai. `nginx-test`, `make test-all` chạy bảy bộ `t/*.t` qua cùng `prove` như CI. Image tích hợp gồm helper/bố trí bản địa và kiểm tra runtime pool. Build nghiêm ngặt không luồng thử runtime từng bước. Mọi test lỗi đều làm lệnh thất bại.

Xem [`t/TESTING.md`](../../t/TESTING.md) về build nghiêm ngặt, sanitizer, Valgrind, nền tảng và báo cáo. Mọi job GitHub ở `ci.yml`.

---

<a id="migration"></a>

## Chuyển đổi

<a id="queue-and-status-changes"></a>

### Thay đổi hàng đợi và trạng thái

Thay đổi cấu trúc SHM cần khởi động lại hoàn toàn. Cú pháp phương thức/ACL và bộ xử lý giao thức được giữ lại. v4 mặc định dùng JSON, mã kết quả thống nhất ở header và nội dung, cùng `200 no_match` khi không có mục. `cache_purge_legacy_status` chỉ phát cảnh báo. Cập nhật ứng dụng khách theo [quy ước HTTP](../HTTP_RESPONSES.md). Sức chứa bao gồm tác vụ đã giao; yêu cầu quá tải chờ được nhận. Tác vụ trùng đang chạy có thể lên lịch duyệt lại.

Trạng thái thêm trường, giữ tên cũ. `requests.overflow` nay đếm chờ tiếp nhận. Số liệu gần đúng dùng bộ đếm tích lũy 64-bit. Build có luồng tự đăng ký `default` khi tham chiếu; pool có tên tách I/O. Chỉ cấu hình status cũng cấp SHM.

Thế hệ/REFRESH bật rõ ở v4.0. Liên kết `cache_purge_key`, dùng `$cache_purge_key` làm toàn bộ khóa bản địa ở mọi location tham gia. Tên vật lý đổi; tệp cũ thành lạnh, đợi hết hạn lõi hoặc dọn kiểu cũ tường minh. Giữ marker trạng thái cùng cache. Location PURGE cũ không tự có REFRESH. Người dùng PR60 thử nghiệm phải cấu hình endpoint/hồ sơ mới và phân trang; phản hồi vô hạn, đoán URI/Host, mở rộng phương thức ngầm của PR60 không phải cam kết tương thích.

<a id="from-the-original-frickle-module"></a>

### Từ mô-đun FRiCKLE gốc

Các dạng phương thức/danh sách truy cập và vùng/khóa vẫn dùng được. Mục không tồn tại trả `200 no_match`; xóa `cache_purge_legacy_status` và cập nhật ứng dụng khách. Để bật xử lý nền, thêm:

```nginx
http {
    cache_purge_background_queue on;
}
```

<a id="from-nginx-selective-cache-purge-module"></a>

### Từ nginx-selective-cache-purge-module

Thay mô-đun; hàng đợi đã tích hợp. Chuyển tham số cũ sang `cache_purge_queue_size`, `cache_purge_batch_size`, `cache_purge_throttle_ms`.

---

<a id="temporary-directories"></a>

## Thư mục tạm

Bỏ thư mục tạm cấu hình dưới gốc bằng so toàn thành phần đường. Tên như `proxy_temp` không được dành riêng nếu không là đường tạm đã cấu hình. Chỉ tệp thường có tên đúng 32 ký tự hex là ứng viên xóa. Tệp ghi tạm dạng số/có hậu tố bị bỏ, kể cả `use_temp_path=off`.

Mỗi lượt kiểm phiên bản header bản địa, offset, đầy đủ marker `KEY: `, CRC32, hash tên khóa/biến thể với giới hạn khóa 8192 byte. Tệp sai, cắt cụt, phiên bản khác hay không khớp bị bỏ ngay cả purge-all. Chấp nhận độ dài ETag/Vary đúng tối đa lõi. Xác minh định dạng không xác thực tệp độc hại.

Gốc có thể là symlink hoặc junction Windows; symlink/reparse point con bị bỏ. POSIX vượt kiểm tra đầy đủ tính năng tương đối-descriptor dùng `ngx_openat_file`/`ngx_file_at_info` lõi với no-follow cho con, xóa tương đối thư mục cha đang mở. Ngăn chuyển hướng qua symlink con bị thay, nhưng không là sandbox chống di chuyển thư mục đã mở. Windows/Unix cũ dùng đường đầy đủ và kiểm link, vẫn còn race thay đường. Tìm/xóa khóa chính xác kiểu cũ cũng dùng đường đầy đủ. Giữ gốc riêng tư, không cho bên ghi không đáng tin.

Viết đường cấu hình theo dạng chuẩn nhất quán. So sánh không giải alias symlink, mount, Unicode. Tác vụ giữ bộ bảo vệ; bản trùng mới cập nhật việc chờ/lượt sau. Lượt đang chạy giữ bộ cũ, không phải hợp mọi thế hệ worker sống. Đừng đổi mục đích đường khi tác vụ liên quan đang chờ/chạy.

---

<a id="security"></a>

## Bảo mật

- Giới hạn endpoint xóa bằng `allow` / `deny` hoặc lớp xác thực phía trước; không công khai.
- Chọn `cache_purge_queue_size` theo lưu lượng để giới hạn SHM khi ngập yêu cầu.
- Cân nhắc `limit_req` để giới hạn PURGE độc lập lưu lượng thường.

---

<a id="license"></a>

## Giấy phép

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

## Xem thêm

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [Tài liệu nginx](https://nginx.org/en/docs/)
