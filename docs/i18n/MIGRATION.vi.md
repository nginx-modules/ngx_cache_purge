# ngx_cache_purge

[README](README.vi.md) · [Chỉ thị](DIRECTIVES.vi.md)

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
