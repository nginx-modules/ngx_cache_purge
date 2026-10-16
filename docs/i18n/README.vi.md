<a id="ngx_cache_purge"></a>

# ngx_cache_purge

Mô-đun NGINX dùng để xóa mục trong bộ nhớ đệm `FastCGI`, `proxy`, `SCGI` và `uWSGI`. Trong v4, thế hệ khóa ngăn yêu cầu mới dùng mục cũ ngay cả khi tệp chưa bị xóa. REFRESH hỏi máy chủ gốc xem nội dung trong bộ nhớ đệm proxy đã thay đổi chưa. Cả hai tính năng đều cần cấu hình riêng. Cú pháp PURGE được giữ lại, nhưng phản hồi HTTP thay đổi; xem [chuyển đổi](README.vi.md#migration).

[English](../../README.md) · [简体中文](README.zh-CN.md) · [日本語](README.ja.md) · [한국어](README.ko.md) · [Português (Brasil)](README.pt-BR.md) · [Deutsch](README.de.md) · [Español](README.es.md) · [Français](README.fr.md) · [Türkçe](README.tr.md) · [Polski](README.pl.md) · [Bahasa Indonesia](README.id.md) · [Tiếng Việt](README.vi.md) · [Українська](README.uk.md) · [فارسی](README.fa.md) · [Русский](README.ru.md)

<a id="choosing-a-mode"></a>

## Chọn cách xử lý phù hợp

| Mục tiêu | Cấu hình | Ý nghĩa phản hồi |
|---|---|---|
| Xóa mục đã lỗi thời và chờ thao tác xóa | PURGE khi tắt hàng đợi nền | `200 purged` hoặc `200 no_match`; lỗi trả về 4xx/5xx |
| Lên lịch dọn theo tiền tố hoặc toàn bộ bộ nhớ đệm | PURGE với `cache_purge_background_queue on` | `202 accepted`; mục cũ vẫn có thể được phục vụ trong lúc chờ |
| Không cho yêu cầu mới dùng nội dung cũ, kể cả tệp được ghi muộn | `cache_purge_namespace` + `cache_purge_key` + PURGE | `200 invalidated`; dùng thế hệ khóa mới, xóa tệp sau |
| Kiểm tra thay đổi tại máy chủ gốc | Thế hệ khóa + `proxy_cache_refresh` + hồ sơ nội bộ | `200` kèm kết quả từng mục và thông tin tiếp tục; không nạp sẵn nội dung |

Có thể kết hợp các cơ chế: đổi thế hệ cũng lên lịch dọn tệp. Chỉ bật hàng đợi không ngăn được tác động của việc ghi muộn. Xem [ví dụ v4](README.vi.md#generation-and-refresh-example), [ví dụ PURGE](EXAMPLES.vi.md#sample-configurations) và [phản hồi HTTP](../HTTP_RESPONSES.md) (tiếng Anh).

<a id="compatibility"></a>

## Tương thích

Trong môi trường sản xuất, hãy dùng bản NGINX còn được bảo trì. POSIX/UNIX là nền tảng chính; hỗ trợ Windows còn thử nghiệm. Luồng là tùy chọn. [NGINX / CI](../../t/TESTING.md#compatibility-matrix).

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
./configure --with-compat --add-dynamic-module=/path/to/ngx_cache_purge
make modules
```

Dùng mã nguồn đúng phiên bản NGINX đang cài trên máy chủ. `--with-compat` không bảo đảm khả năng tương thích giữa mọi phiên bản NGINX hoặc thư viện C. Xem [mô-đun động và gói phát hành](../BINARY_MODULES.md) (tiếng Anh).

<a id="generation-and-refresh-example"></a>

## Ví dụ: danh mục sản phẩm

Ví dụ đầy đủ này lắng nghe cổng cục bộ 8080 và chuyển yêu cầu tới ứng dụng ở cổng 8081. Ứng dụng phải phục vụ `/products/42` với Host `shop.example.com`. Thay địa chỉ, tên miền và các thư mục bộ nhớ đệm/trạng thái có quyền ghi.

`shop.example.com` là tên miền trong tiêu đề HTTP Host, không phải thư mục. `/products/42` là URI sản phẩm. `catalog:` là tiền tố khóa cố định, không phải tên máy chủ. Tiền tố cộng URI đầy đủ, gồm chuỗi truy vấn, tạo khóa logic như `catalog:/products/42`. Hồ sơ REFRESH bỏ `catalog:` rồi kiểm tra `/products/42` tại cùng ứng dụng.

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

GET một sản phẩm tồn tại và có thể lưu vào bộ nhớ đệm cho đến `X-Cache-Status: HIT`, sau đó gửi REFRESH:

```sh
curl -i 'http://127.0.0.1:8080/products/42'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/42' \
  'http://127.0.0.1:8080/admin/refresh'
```

Nếu sản phẩm đã thay đổi, gửi PURGE. Ở đây `200 invalidated` chặn yêu cầu mới dùng thế hệ cũ; GET tiếp theo truy cập ứng dụng.

```sh
curl -i -X PURGE 'http://127.0.0.1:8080/products/42'
```

Để kiểm tra mọi sản phẩm, dùng `catalog:/products/*`. Khi `complete: false`, giữ nguyên bộ chọn và thay `<next_cursor>` bằng giá trị trả về. Xử lý lỗi HTTP trước khi tiếp tục.

```sh
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  'http://127.0.0.1:8080/admin/refresh'
curl -i -X REFRESH -H 'X-Cache-Key: catalog:/products/*' \
  -H 'X-Cache-Purge-Cursor: <next_cursor>' \
  'http://127.0.0.1:8080/admin/refresh'
```

<a id="partial-key-purge"></a>

## Xóa theo một phần khóa

Thêm `*` vào **khóa cache đã được tính giá trị** để khớp tiền tố không phân biệt hoa/thường. Khóa chính xác và Vary vẫn phân biệt hoa/thường. Tiền tố dọn dẹp vật lý tối đa 511 byte, không tính `*` cuối. Tiền tố dài hơn được ghi log một lần và trả `400 invalid_selector` mà không duyệt tệp.

Với `cache_purge_key`, so biểu thức logic, tối đa 8096 byte tính cả `*` cuối.

Với biểu thức kết thúc bằng `$uri`, yêu cầu này chọn tiền tố URI:

```http
PURGE /images/header*
```

Dấu sao phải là byte cuối khóa đã tính, không chỉ cuối đường dẫn URI. Xét cả query string hoặc hậu tố biến trong `proxy_cache_key`. Khóa chỉ có `*` duyệt mọi mục hợp lệ.

<a id="generation-invalidation"></a>

## Vô hiệu hóa theo thế hệ

Thế hệ khóa ngăn yêu cầu mới dùng nội dung cũ sau PURGE, kể cả phản hồi cũ được ghi muộn. Tệp được xóa riêng. Không gian tên dùng chung quy tắc vô hiệu hóa; giữ thư mục trạng thái qua các lần khởi động lại.

<a id="conditional-refresh"></a>

## Kiểm tra bộ nhớ đệm với máy chủ gốc (REFRESH)

REFRESH gửi HEAD cho cùng dạng biểu diễn tại nguồn, dùng ETag và Last-Modified đã lưu. Nếu biết trang sản phẩm đã cũ, hãy dùng PURGE. REFRESH không tải nội dung mới, không điền bộ nhớ đệm trống và không kéo dài TTL. [HTTP / REFRESH](../REFRESH.md).

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

<a id="migration"></a>

## Chuyển đổi

Thay đổi cấu trúc SHM cần khởi động lại hoàn toàn. Cú pháp phương thức/ACL và bộ xử lý giao thức được giữ lại. v4 mặc định dùng JSON, mã kết quả thống nhất ở header và nội dung, cùng `200 no_match` khi không có mục. `cache_purge_legacy_status` chỉ phát cảnh báo. Cập nhật ứng dụng khách theo [quy ước HTTP](../HTTP_RESPONSES.md). Sức chứa bao gồm tác vụ đã giao; yêu cầu quá tải chờ được nhận. Tác vụ trùng đang chạy có thể lên lịch duyệt lại.

<a id="security"></a>

## Bảo mật

- Giới hạn endpoint xóa bằng `allow` / `deny` hoặc lớp xác thực phía trước; không công khai.
- Chọn `cache_purge_queue_size` theo lưu lượng để giới hạn SHM khi ngập yêu cầu.
- Cân nhắc `limit_req` để giới hạn PURGE độc lập lưu lượng thường.

<a id="documentation"></a>

## Tài liệu

- [Tham chiếu chỉ thị](DIRECTIVES.vi.md)
- [Ví dụ cấu hình khác](EXAMPLES.vi.md)
- [Vận hành và xử lý sự cố (tiếng Anh)](../OPERATIONS.md)
- [HTTP và giới hạn REFRESH (tiếng Anh)](../REFRESH.md)
- [Chuyển sang v4 (tiếng Anh)](../MIGRATION.md)
- [Kiến trúc triển khai (tiếng Anh)](../ARCHITECTURE.md)
- [Phát triển và kiểm thử (tiếng Anh)](../../t/TESTING.md)

<a id="see-also"></a>

## Xem thêm

- [ngx_slowfs_cache](https://github.com/FRiCKLE/ngx_slowfs_cache)
- [nginx-selective-cache-purge-module](https://github.com/wandenberg/nginx-selective-cache-purge-module)
- [Tài liệu nginx](https://nginx.org/en/docs/)

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
