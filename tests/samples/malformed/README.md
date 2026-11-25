# Danh Mục Mẫu File .torrent Dị Dạng (Malformed Samples)

Thư mục này chứa các file `.torrent` dị dạng được cố tình tạo ra nhằm mục đích kiểm thử độ bền (robustness), xử lý ngoại lệ an toàn và chống tấn công leo thang thư mục (Path Traversal / Directory Escape) của Bencode Parser và Torrent Metadata Parser (`TorrentFile::parse` / `TorrentFile::parseFile`).

Tất cả các file trong thư mục này **BẮT BUỘC** phải bị Parser từ chối (reject) và ném ra ngoại lệ (`std::invalid_argument` hoặc `std::runtime_error`), tuyệt đối không được làm crash ứng dụng và không được tạo file/thư mục tùy tiện ra ngoài thư mục download.

---

## Bảng Phân Loại & Mục Đích Kiểm Thử

### 1. Nhóm Lỗi Cú Pháp Bencode (Syntax & Token Errors)

| Tên File | Dữ Liệu Raw Bencode | Lý Do Dị Dạng | Lỗi Kỳ Vọng |
|---|---|---|---|
| `empty_file.torrent` | *(0 bytes)* | File rỗng | `Bencode: empty input` |
| `unterminated_dict.torrent` | `d4:name4:test` | Bắt đầu dictionary `d` nhưng thiếu ký tự `e` đóng | `Bencode: unterminated dictionary` |
| `unterminated_int.torrent` | `d4:name3:fooi42` | Bắt đầu số nguyên `i42` nhưng thiếu ký tự `e` đóng | `Bencode: unterminated integer` |
| `truncated_string.torrent` | `d4:name100:shorte` | Chuỗi khai báo dài 100 byte nhưng thực tế chỉ có 5 byte | `Bencode: truncated string value` |
| `invalid_int_leading_zeros.torrent` | `d4:namei042ee` | Số nguyên có chữ số 0 vô nghĩa ở đầu (`i042e`) | `Bencode: invalid integer format` |
| `dict_non_string_key.torrent` | `di1ei2ee` | Key của từ điển là số nguyên thay vì chuỗi byte | `Bencode: dictionary keys must be strings` |
| `unsupported_token.torrent` | `d4:name?e` | Chứa ký tự token không hợp lệ trong bencode | `Bencode: unsupported token` |

---

### 2. Nhóm Lỗi Cấu Trúc Metadata BitTorrent (Schema & Type Errors)

| Tên File | Dữ Liệu Raw Bencode | Lý Do Dị Dạng | Lỗi Kỳ Vọng |
|---|---|---|---|
| `not_a_dict.torrent` | `i42e` | Cấp cao nhất (root) không phải dictionary mà là integer | `top-level value must be a dictionary` |
| `missing_info.torrent` | `d8:announce36:http://tracker.example:6969/announcee` | Dictionary gốc không chứa trường `info` | `missing info dictionary` |
| `info_not_dict.torrent` | `d4:info10:not-a-dicte` | Trường `info` là string thay vì dictionary | `info must be a dictionary` |
| `missing_name.torrent` | `d4:infod12:piece length...` | Dictionary `info` thiếu trường bắt buộc `name` | `missing info.name` |
| `name_not_string.torrent` | `d4:infod4:namei123e...` | Trường `name` là integer thay vì string | `info.name must be a string` |
| `missing_piece_length.torrent` | `d4:infod4:name4:test...` | Dictionary `info` thiếu trường `piece length` | `missing info.piece length` |
| `piece_length_not_int.torrent` | `d4:infod...12:piece length4:256k...` | `piece length` là string thay vì integer | `info.piece length must be an integer` |
| `zero_piece_length.torrent` | `d4:infod...12:piece lengthi0e...` | `piece length` bằng 0 (không hợp lệ) | `info.piece length must be positive` |
| `negative_piece_length.torrent` | `d4:infod...12:piece lengthi-262144e...` | `piece length` âm | `info.piece length must be positive` |
| `missing_pieces.torrent` | `d4:infod...12:piece lengthi262144e...` | Dictionary `info` thiếu trường băm `pieces` | `missing info.pieces` |
| `pieces_not_string.torrent` | `d4:infod...6:piecesi123e...` | Trường `pieces` là integer thay vì chuỗi nhị phân | `info.pieces must be a string` |
| `pieces_bad_length.torrent` | `d4:infod...6:pieces25:1234567890123456789012345...` | Độ dài chuỗi `pieces` (25 bytes) không chia hết cho 20 | `pieces length must be divisible by 20` |
| `negative_length.torrent` | `d4:infod...6:lengthi-1eee` | Kích thước file `length` là số âm | `info.length must not be negative` |

---

### 3. Nhóm Lỗi An Toàn & Chống Xuyên Thủng Thư Mục (Security & Path Traversal)

Các file này mô phỏng các đòn tấn công độc hại từ tracker hoặc peer giả mạo gửi torrent nhằm ghi đè các file hệ thống nhạy cảm hoặc thoát ra khỏi thư mục tải về:

| Tên File | Giá Trị Đường Dẫn Độc Hại | Kỹ Thuật Tấn Công | Lỗi Kỳ Vọng |
|---|---|---|---|
| `path_traversal_name.torrent` | `name: "../../evil.txt"` | Directory Traversal dạng relative thoát lên 2 cấp cha | `info.name is not a safe relative path` |
| `path_traversal_absolute.torrent` | `name: "/etc/passwd"` | Đường dẫn tuyệt đối chuẩn POSIX/Linux | `info.name is not a safe relative path` |
| `path_traversal_windows_drive.torrent` | `name: "C:/Windows/cmd"` | Đường dẫn tuyệt đối Windows có ký tự ổ đĩa (`C:`) | `info.name is not a safe relative path` |
| `path_traversal_backslash.torrent` | `name: "evil\\cmd.exe"` | Sử dụng ký tự phân cách ngược (`\`) để né filter | `info.name is not a safe relative path` |
| `path_traversal_ads.torrent` | `name: "file.txt:stream"` | Tấn công NTFS Alternate Data Stream (chứa dấu `:`) | `info.name is not a safe relative path` |
| `path_traversal_multi_file.torrent` | `files: path = ["..", "evil"]` | Directory Traversal trong danh sách file của multi-file torrent | `unsafe file path` |

---

## Cách Chạy Kiểm Thử

Toàn bộ các mẫu file trên được kiểm tra tự động qua test suite `test_malformed_torrent`:

```bash
# Build test
cmake --build build --target test_malformed_torrent

# Chạy test
ctest -R test_malformed_torrent --output-on-failure
```
