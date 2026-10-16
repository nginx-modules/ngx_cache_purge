# ngx_cache_purge

[README](README.vi.md) · [Chỉ thị](DIRECTIVES.vi.md)

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

[Endpoint trạng thái](DIRECTIVES.vi.md#cache_purge_queue_status) cho thấy chờ, hoạt động, tiếp nhận, trùng lặp, xóa, HIT/MISS. `202 Accepted` xác nhận nhận/hợp nhất, không phải xóa hoặc vô hiệu hóa logic tức thì. Theo dõi trễ yêu cầu cùng tuổi hàng đợi và tranh chấp lưu trữ.

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

<a id="temporary-directories"></a>

## Thư mục tạm

Bỏ thư mục tạm cấu hình dưới gốc bằng so toàn thành phần đường. Tên như `proxy_temp` không được dành riêng nếu không là đường tạm đã cấu hình. Chỉ tệp thường có tên đúng 32 ký tự hex là ứng viên xóa. Tệp ghi tạm dạng số/có hậu tố bị bỏ, kể cả `use_temp_path=off`.

Mỗi lượt kiểm phiên bản header bản địa, offset, đầy đủ marker `KEY: `, CRC32, hash tên khóa/biến thể với giới hạn khóa 8192 byte. Tệp sai, cắt cụt, phiên bản khác hay không khớp bị bỏ ngay cả purge-all. Chấp nhận độ dài ETag/Vary đúng tối đa lõi. Xác minh định dạng không xác thực tệp độc hại.

Gốc có thể là symlink hoặc junction Windows; symlink/reparse point con bị bỏ. POSIX vượt kiểm tra đầy đủ tính năng tương đối-descriptor dùng `ngx_openat_file`/`ngx_file_at_info` lõi với no-follow cho con, xóa tương đối thư mục cha đang mở. Ngăn chuyển hướng qua symlink con bị thay, nhưng không là sandbox chống di chuyển thư mục đã mở. Windows/Unix cũ dùng đường đầy đủ và kiểm link, vẫn còn race thay đường. Tìm/xóa khóa chính xác kiểu cũ cũng dùng đường đầy đủ. Giữ gốc riêng tư, không cho bên ghi không đáng tin.

Viết đường cấu hình theo dạng chuẩn nhất quán. So sánh không giải alias symlink, mount, Unicode. Tác vụ giữ bộ bảo vệ; bản trùng mới cập nhật việc chờ/lượt sau. Lượt đang chạy giữ bộ cũ, không phải hợp mọi thế hệ worker sống. Đừng đổi mục đích đường khi tác vụ liên quan đang chờ/chạy.

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

Xem [`t/TESTING.md`](../../t/TESTING.md) về build nghiêm ngặt, sanitizer, Valgrind, nền tảng và báo cáo. Mọi job GitHub ở `build-test-release.yml`.
