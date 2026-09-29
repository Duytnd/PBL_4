# Kế hoạch dự án: Client BitTorrent tự xây dựng, tương thích chuẩn, với chiến lược tự thiết kế

## 1. Tổng quan

- Nhóm **tự cài đặt (tự viết code) từ đầu** một client BitTorrent bằng C++, **tuân theo đúng đặc tả BitTorrent (BEP) đã có sẵn**, để trao đổi dữ liệu được với client có sẵn (qBittorrent, Transmission...) theo cả hai chiều: client của nhóm tải từ client thật, và client thật tải từ client của nhóm.
- Nhóm **không thiết kế một giao thức mới**. Định dạng và luật của giao thức (handshake, message, `.torrent`, tracker, DHT) là của BitTorrent; nhóm viết code thực hiện đúng các luật đó, không dùng thư viện BitTorrent có sẵn (libtorrent...).
- Phần nhóm **tự thiết kế** nằm ở những chỗ chuẩn để ngỏ: chiến lược chọn piece, chiến lược chia băng thông, cách quản lý peer, và một số message mở rộng riêng qua extension protocol (BEP 10).
- Tự viết trình tạo file `.torrent` (`torrent_builder`), tracker server và toàn bộ client; có web UI đơn giản để theo dõi và điều khiển.
- Loại dự án: học tập / cá nhân, nhóm 3 người, test trên 3 laptop qua LAN, không có deadline cố định.
- Ngôn ngữ: C++ (khuyến nghị C++17 trở lên).
- Không còn chức năng quét mã độc trong phạm vi dự án này.

## 2. Phạm vi

**Trong phạm vi:**
- Torrent v1 (SHA-1), một file và nhiều file / thư mục; tạo và đọc file `.torrent`.
- Tracker HTTP theo BEP 3 và compact peers (BEP 23): cả tracker server tự viết lẫn tracker client.
- Peer wire protocol đầy đủ theo BEP 3: handshake, choke / unchoke, interested, have, bitfield, request, piece, cancel.
- Extension protocol (BEP 10) và ít nhất một extension riêng của nhóm.
- DHT theo BEP 5; magnet link (BEP 9) và PEX (BEP 11) ở giai đoạn sau.
- Chiến lược chọn piece và chia băng thông có thể thay đổi được, kèm công cụ đo để so sánh.
- Web UI (trình duyệt) ngay từ giai đoạn đầu.

**Ngoài phạm vi:**
- Torrent v2 / hybrid (BEP 52), uTP (BEP 29), UDP tracker (BEP 15), web seed. Client sẽ từ chối torrent chỉ có v2 và báo lỗi rõ ràng.
- Không đặt mục tiêu production-grade (không cần chịu tải hàng nghìn peer, không cần chống DDoS...).

**Phân biệt "cài đặt" và "thiết kế" (để tránh hiểu lầm):**

| | Ai quyết định | Nhóm làm gì |
|---|---|---|
| **Luật của giao thức** (định dạng `.torrent`, cách tính `info_hash`, SHA-1, handshake, ID và định dạng message 0-9, định dạng tracker, KRPC của DHT) | Đặc tả BitTorrent (BEP) | **Cài đặt** đúng từng byte theo đặc tả. Không được đổi, vì đổi bất kỳ chỗ nào là client thật không hiểu |
| **Code thực hiện giao thức** (bencode, tracker, wire protocol, ghép piece, DHT...) | Nhóm | **Tự viết từ đầu**, không dùng thư viện BitTorrent có sẵn |
| **Chính sách mà chuẩn để ngỏ** (chọn piece nào xin, unchoke ai, giới hạn kết nối, ban peer) | Nhóm | **Tự thiết kế** và so sánh nhiều phương án |
| **Message mở rộng riêng** qua BEP 10 | Nhóm | **Tự thiết kế**; chỉ có tác dụng khi cả hai đầu là client của nhóm, client thật sẽ bỏ qua |

Khi viết báo cáo cần nêu đúng: đề tài là **cài đặt một client BitTorrent tương thích chuẩn, kèm nghiên cứu chiến lược chọn piece / chia băng thông và một extension riêng**, không phải thiết kế giao thức BitTorrent mới. Giao thức chạy giữa client của nhóm và qBittorrent chính là giao thức BitTorrent chuẩn.

**Về nội dung test:** chỉ dùng nội dung hợp pháp (file tự tạo, ISO Linux...). Mặc định test trong LAN với tracker riêng. Chỉ vào DHT công khai ở giai đoạn 4 và chỉ với torrent hợp pháp.

## 3. Kiến trúc tổng thể

Hai tầng (tầng nền là luật có sẵn của BitTorrent mà nhóm **cài đặt**; tầng trên là phần nhóm **tự thiết kế**):

```
┌──────────────────────────────────────────────────────────┐
│ TẦNG TRÊN: thiết kế riêng của nhóm                       │
│  • Chiến lược chọn piece (piece_picker)                  │
│  • Chiến lược chia băng thông (choker)                   │
│  • Chính sách kết nối: giới hạn, ban peer sai hash       │
│  • Extension riêng qua BEP 10 (tên có prefix riêng)      │
│  • Công cụ đo và so sánh chiến lược (bench)              │
├──────────────────────────────────────────────────────────┤
│ TẦNG NỀN: đúng chuẩn, không được đổi                     │
│  • .torrent v1 (bencode, SHA-1, info_hash)               │
│  • Tracker HTTP (BEP 3) + compact peers (BEP 23)         │
│  • Handshake 68 byte + message ID 0-9                    │
│  • Block 16 KiB, verify SHA-1 từng piece                 │
│  • Extension protocol (BEP 10), DHT (BEP 5)              │
└──────────────────────────────────────────────────────────┘
```

Luồng xử lý:

```
Người chia sẻ: torrent_builder -> .torrent (kèm địa chỉ tracker)
        |
Người tải: nạp .torrent -> tính info_hash (SHA-1 của info dict)
        |
Tìm peer (Tracker -> DHT -> PEX)
        |
Handshake -> (extended handshake nếu hai bên hỗ trợ)
        |
Trao đổi block 16 KiB (piece_picker chọn xin gì, choker chọn cho ai)
        |
Ghép block thành piece, verify SHA-1, ghi vào file
        |
Hoàn tất -> tiếp tục seed
```

Các thành phần chính:

| Thành phần | Vai trò |
|---|---|
| Bencode, Torrent parser, Torrent builder | Đọc / ghi bencode; đọc, kiểm tra `.torrent`, tính `info_hash`; tạo `.torrent` từ file / thư mục |
| Piece manager | Ánh xạ piece ↔ vị trí trong file, đọc / ghi block, verify SHA-1 |
| Tracker server / client | Đăng ký và trả danh sách peer theo `info_hash` (BEP 3, 23) |
| Peer connection / protocol | Kết nối TCP, handshake, đọc / ghi message, giới hạn phòng thủ |
| Extension | BEP 10 và các extension (riêng của nhóm, sau này BEP 9, 11) |
| Piece picker | Chọn block cần xin từ peer nào; nhiều chiến lược đổi được |
| Choker | Chọn peer nào được unchoke; nhiều chiến lược đổi được |
| DHT node | BEP 5: Kademlia trên UDP |
| Bench | Giới hạn băng thông, ghi số liệu để so sánh chiến lược |
| Web UI | HTTP server cục bộ hiển thị torrent, peer, tiến trình, số liệu đo |

## 4. Tech stack & thư viện

| Nhu cầu | Thư viện đề xuất |
|---|---|
| Networking / async I/O (TCP + UDP) | Asio (standalone hoặc Boost.Asio) |
| SHA-1 (piece, `info_hash`) | OpenSSL (EVP API) |
| Bencode | Tự viết parser + writer riêng |
| JSON cho API web UI | nlohmann/json (chỉ dùng cho `/api/*`, không dùng cho metadata) |
| HTTP client (tracker announce) | libcurl hoặc cpr |
| Web server nhúng | cpp-httplib (single-header) |
| Build system | CMake (đa nền tảng vì 3 laptop có thể khác OS) |
| Thư viện BitTorrent có sẵn (libtorrent...) | **Không dùng** — nhóm tự cài đặt toàn bộ phần giao thức |
| Client đối chứng (không nằm trong code) | qBittorrent hoặc Transmission |
| Công cụ đối chiếu `.torrent` | mktorrent hoặc qBittorrent |
| Gỡ lỗi gói tin | Wireshark (có sẵn bộ giải mã BitTorrent) |

Cài đặt trên mỗi laptop (apt trên Linux, brew trên Mac, vcpkg trên Windows): trình biên dịch C++17 + CMake, OpenSSL (dev), libcurl (dev). `cpp-httplib` và `nlohmann/json` chỉ là file header, bỏ thẳng vào project. Người dùng chỉ cần có sẵn trình duyệt để mở giao diện.

## 5. Thiết kế chi tiết

### 5.1 File `.torrent` và `torrent_builder`

**Bencode:** số nguyên `i42e`, chuỗi byte `4:spam`, danh sách `l...e`, từ điển `d...e`. Khoá của từ điển phải sắp xếp theo thứ tự byte tăng dần khi ghi ra.

**Cấu trúc `.torrent` (v1):**

```
d
  8:announce   <URL tracker của nhóm>
  10:created by / 13:creation date   (tuỳ chọn, nằm NGOÀI info nên không ảnh hưởng info_hash)
  4:info d
      4:name          <tên file (1 file) hoặc tên thư mục gốc (nhiều file)>
      12:piece length <số nguyên, luỹ thừa của 2, vd 262144 = 256 KiB>
      6:pieces        <các SHA-1 20 byte nối liền nhau, mỗi hash ứng với 1 piece>
      6:length        <tổng kích thước>                    (torrent 1 file)
      -- hoặc --
      5:files l [ d 6:length <n> 4:path l <thành phần đường dẫn...> e ... ]   (nhiều file)
  e
e
```

- Các file được nối liên tục thành 1 luồng byte trước khi chia piece. Piece cuối có thể ngắn hơn. Một piece có thể vắt qua ranh giới hai file.
- **`info_hash`** = SHA-1 của **đúng chuỗi byte gốc** của `info` dict. Parser phải giữ vị trí byte của `info` trong dữ liệu gốc để hash, không hash lại bản đã mã hoá lại.
- Chỉ hỗ trợ v1. Nếu `info` chỉ có dấu hiệu v2 (không có `pieces`) thì từ chối.

**`torrent_builder` — quy trình:**
1. **Đầu vào:** đường dẫn file hoặc thư mục, URL tracker, `piece length` (tuỳ chọn), tên torrent (mặc định lấy tên file / thư mục).
2. **Duyệt file:** đệ quy, lấy file thường, bỏ qua thư mục rỗng, symlink và file không đọc được (ghi cảnh báo). **Sắp xếp theo đường dẫn (thứ tự byte)** để cùng một thư mục luôn cho cùng `info_hash`. Tách đường dẫn tương đối thành từng thành phần, dùng UTF-8, không lưu đường dẫn tuyệt đối.
3. **Chia piece và hash theo luồng:** đọc bằng bộ đệm cố định (vd 1 MiB), nối các file thành 1 luồng byte, dùng 1 bộ SHA-1 tăng dần; đủ `piece length` byte thì chốt hash 1 piece; cuối luồng chốt nốt piece cuối. File kích thước 0 vẫn có trong `files` nhưng không đóng góp byte.
4. **Ghi `.torrent`** (khoá đã sắp xếp) và in `info_hash`, số piece, tổng kích thước.

**Chọn `piece length`:** luỹ thừa của 2, mặc định 256 KiB; có thể tự chọn theo kích thước để có vài trăm tới vài nghìn piece, trong khoảng 16 KiB đến 4 MiB.

**Giao diện:** lệnh CLI, vd `p2p create <đường dẫn> --tracker http://<ip>:<port>/announce --piece-length 262144 -o out.torrent`. Nút "Tạo torrent" trên web UI là mở rộng về sau.

**Kiểm chứng:** tạo `.torrent` từ cùng dữ liệu bằng builder của nhóm và bằng mktorrent / qBittorrent với cùng `piece length`, so sánh `info_hash`. Thử 1 file, thư mục lồng nhau, file kích thước không chia hết `piece length`, file rỗng, file lớn hơn RAM.

**Kiểm tra bắt buộc khi đọc `.torrent` từ nguồn ngoài:**
- Giới hạn độ sâu bencode, kích thước file `.torrent`, số file và số piece tối đa.
- `pieces` phải có độ dài chia hết cho 20 và số hash phải khớp tổng kích thước / `piece length`.
- Mỗi thành phần `path` bị từ chối nếu rỗng, là `.` hoặc `..`, chứa `/`, `\`, ký tự NUL, có ký tự ổ đĩa (`C:`), hoặc là tên dành riêng của Windows (`CON`, `NUL`...). Sau khi ghép, kiểm tra đường dẫn chuẩn hoá vẫn nằm trong thư mục tải. Nếu không, `.torrent` độc hại có thể ghi file ra ngoài thư mục tải (path traversal).

### 5.2 Tracker (BEP 3 + BEP 23)

Client gửi HTTP GET tới URL `announce` với: `info_hash` (20 byte, URL-encode từng byte), `peer_id` (20 byte), `port`, `uploaded`, `downloaded`, `left`, `compact=1`, `event` (`started` / `stopped` / `completed`, bỏ trống khi announce định kỳ), `numwant`.

Tracker trả bencode:
- Thành công: `d8:intervali1800e5:peers<chuỗi compact>e`, mỗi peer 6 byte (4 byte IPv4 + 2 byte port, big-endian).
- Lỗi: `d14:failure reason<lý do>e`.

Lưu ý:
- `info_hash` và `peer_id` là **byte nhị phân**, không phải chuỗi UTF-8. Phải URL-encode từng byte khi gửi, và tracker server phải giải mã lại đúng 20 byte. Kiểm tra cách `cpp-httplib` xử lý tham số chứa byte lạ; nếu cần thì tự phân tích query string.
- `peer_id` kiểu Azureus: `-PB0001-` + 12 byte ngẫu nhiên.
- Tracker server lưu peer theo `info_hash`, có thời gian hết hạn (vd 2 × `interval`), giới hạn số peer trả về, và không trả lại chính peer đang hỏi.
- Vì tracker theo chuẩn nên qBittorrent có thể announce tới tracker của nhóm, và client của nhóm cũng announce được tới tracker khác. Đây là một phần test đối chứng.

### 5.3 Peer wire protocol (BEP 3)

**Handshake** (68 byte, gửi ngay khi mở kết nối TCP):

```
1 byte  : 19
19 byte : "BitTorrent protocol"
8 byte  : reserved  (bit 0x10 ở byte chỉ số 5 = hỗ trợ extension BEP 10;
                     bit 0x01 ở byte chỉ số 7 = hỗ trợ DHT BEP 5)
20 byte : info_hash
20 byte : peer_id
```

Nếu `info_hash` của peer đối diện khác torrent đang xử lý thì đóng kết nối.

**Message** (sau handshake): `[4 byte độ dài][1 byte ID][payload]`, độ dài = 0 là keep-alive.

| ID | Message | Payload |
|---|---|---|
| 0 | choke | — |
| 1 | unchoke | — |
| 2 | interested | — |
| 3 | not interested | — |
| 4 | have | index piece (4 byte) |
| 5 | bitfield | bit từng piece, chỉ gửi ngay sau handshake |
| 6 | request | index, begin, length |
| 7 | piece | index, begin, dữ liệu block |
| 8 | cancel | index, begin, length |
| 9 | port | port UDP của DHT (BEP 5) |
| 20 | extended | extension protocol (BEP 10) |

**Trạng thái mỗi kết nối:** `am_choking`, `am_interested`, `peer_choking`, `peer_interested`. Ban đầu cả hai bên đều choking và không interested.

**Block:** piece được chia thành block 16 KiB (block cuối của piece cuối có thể ngắn hơn). Client xin từng block bằng `request`, chỉ verify SHA-1 khi đủ block của piece. Piece sai hash thì bỏ và xin lại từ peer khác; peer gửi sai hash nhiều lần thì ban.

**Giới hạn phòng thủ:** từ chối message có độ dài lớn bất thường (vd > 128 KiB), request có `length` quá lớn hoặc index / begin ngoài phạm vi, block mà mình chưa request, bitfield sai độ dài. Đặt timeout cho handshake và cho từng request chưa được trả lời.

### 5.4 Extension protocol (BEP 10) và extension riêng

1. Khi handshake, bật bit extension trong `reserved`.
2. Sau handshake, mỗi bên gửi message ID 20 với byte tiếp theo là `0` (extended handshake), kèm một dict bencode liệt kê extension mình hiểu, mỗi extension có một số ID do bên gửi tự gán:

```
d 1:m d 11:ut_metadata i2e  8:pb_stats i7e e
  1:v 12:MyClient 0.1
e
```

3. Khi muốn gửi extension X cho peer, dùng đúng số ID mà **peer đó** đã khai báo cho X trong `m` của họ. Nếu peer không khai báo X thì không gửi.
4. Nhận extension lạ thì bỏ qua, không đóng kết nối.

**Extension riêng của nhóm:** đặt tên có prefix riêng (vd `pb_`), không dùng prefix `ut_` hay `lt_` của client khác. Ví dụ để nhóm tham khảo và tự quyết:
- `pb_stats`: peer báo tốc độ và số piece đã có để hai bên chọn nhau tốt hơn.
- `pb_have_range`: gộp nhiều `have` liên tiếp thành một dải, giảm số message.

Với client thật, extension riêng sẽ tự bị bỏ qua (họ không khai báo nó). Vì vậy extension riêng chỉ test được giữa hai client của nhóm; test với client thật chỉ để chắc rằng mọi thứ vẫn chạy bình thường.

### 5.5 Chiến lược chọn piece và chia băng thông

Chuẩn BitTorrent không quy định khi nào gửi `request` hay `unchoke`. Đây là nơi nhóm thiết kế, và cần được đặt sau một giao diện chung để thay đổi được bằng cấu hình:

**Chọn piece (`piece_picker`)** — các chiến lược đề xuất:
- Rarest-first (mặc định của hầu hết client): ưu tiên piece hiếm nhất trong số peer đã kết nối; piece đầu tiên chọn ngẫu nhiên để có dữ liệu chia sẻ nhanh.
- Tuần tự (sequential): tiện cho xem trước, nhưng làm yếu sự đa dạng piece trong mạng.
- Kết hợp (hybrid) hoặc biến thể riêng của nhóm.
- Các tham số: số request chạy song song mỗi peer (pipeline), khi nào vào endgame (xin cùng block từ nhiều peer rồi `cancel` phần thừa).

**Chia băng thông (`choker`)** — các chiến lược đề xuất:
- Tit-for-tat cổ điển: mỗi 10 giây chọn 3 peer đang interested có tốc độ upload cho mình cao nhất, cộng 1 slot optimistic unchoke xoay vòng mỗi 30 giây. Khi đang seed thì chọn theo tốc độ mình upload được.
- Biến thể riêng: chia theo tỷ lệ đóng góp, thay đổi số slot...

**Quản lý peer:** giới hạn số kết nối, ban peer gửi piece sai hash, ưu tiên peer nhanh.

**Đo và so sánh (`bench`):**
- Bộ giới hạn băng thông tích hợp trong client (token bucket cho từng kết nối hoặc toàn cục) để thí nghiệm tái lập được, thay vì phụ thuộc vào tình trạng mạng LAN.
- Số liệu ghi lại: thời gian hoàn thành, tốc độ tải theo thời gian, tỷ lệ upload / download từng peer, số block trùng lặp trong endgame, độ đa dạng piece.
- Thí nghiệm: chạy nhiều peer ảo (nhiều process mỗi laptop), thay đổi chiến lược, so sánh số liệu. Hiển thị kết quả lên web UI.

### 5.6 Tìm peer

- **Giai đoạn 1:** tracker HTTP (mục 5.2), chạy trên 1 trong 3 laptop.
- **Giai đoạn 4:** DHT theo BEP 5.
  - UDP, thông điệp KRPC mã hoá bencode: `ping`, `find_node`, `get_peers`, `announce_peer`.
  - Node ID 160-bit, khoảng cách XOR, k = 8, routing table dạng k-bucket có tách bucket.
  - `get_peers` trả về peer (nếu node biết) hoặc danh sách node gần hơn; `announce_peer` cần `token` lấy từ `get_peers` trước đó.
  - Bootstrap từ một node đã biết (trong LAN: một node do nhóm chạy).
  - Vì chỉ có 3 laptop, mô phỏng thêm peer ảo (mỗi laptop chạy 5-10 process ở port khác nhau) để DHT có đủ node hội tụ.
  - Khi 2 peer bắt tay và bit DHT trong `reserved` được bật thì gửi message `port` (ID 9).
- **Giai đoạn 4 (tuỳ chọn):** PEX (BEP 11) và magnet link qua metadata exchange (BEP 9), dựa trên extension protocol.
- **Giai đoạn 5 (tuỳ chọn):** LSD (BEP 14) để các laptop trong LAN tự tìm thấy nhau không cần tracker.

### 5.7 Web UI

Web UI ở mức đơn giản, mục tiêu là theo dõi và điều khiển:
- Phần lõi C++ chạy nền và khởi động thêm 1 HTTP server cục bộ (vd `127.0.0.1:8080`) dùng `cpp-httplib`; phục vụ file tĩnh trong `web/` và API JSON.
- **Chỉ bind `127.0.0.1`**, kiểm tra header `Host`, dùng token phiên cho các API ghi. Nếu không thì một trang web khác đang mở trong trình duyệt có thể gọi tới `localhost:8080` (CSRF / DNS rebinding) để thêm torrent hoặc đọc thông tin.
- API tối thiểu: `GET /api/status` (torrent, peer, tiến trình, số liệu đo), `POST /api/torrents` (thêm `.torrent`), `POST /api/torrents/{info_hash}/pause|resume|remove`; chọn chiến lược piece picker / choker qua API cấu hình.
- Trang JS gọi API định kỳ (polling ~1 giây), đơn giản hơn WebSocket.
- Tách luồng: 1 thread networking, 1 thread HTTP server, giao tiếp qua vùng trạng thái dùng chung có khoá (mutex) để không block khi mạng chậm và tránh race condition.
- Màn hình tối thiểu: danh sách torrent với % tiến trình, danh sách peer (địa chỉ, tốc độ, trạng thái choke), biểu đồ tốc độ đơn giản từ số liệu `bench`.

## 6. Cấu trúc thư mục project

```
p2p-network/
├── CMakeLists.txt
├── README.md
├── .gitignore
├── src/
│   ├── main.cpp                  (lệnh con: create, download, seed, bench...)
│   ├── core/
│   │   ├── bencode.h / bencode.cpp
│   │   ├── torrent_file.h / torrent_file.cpp
│   │   ├── torrent_builder.h / torrent_builder.cpp
│   │   ├── path_sanitizer.h / path_sanitizer.cpp
│   │   ├── piece_manager.h / piece_manager.cpp
│   │   └── hash_utils.h / hash_utils.cpp
│   ├── network/
│   │   ├── tracker_client.h / tracker_client.cpp
│   │   ├── peer_connection.h / peer_connection.cpp
│   │   ├── peer_protocol.h / peer_protocol.cpp
│   │   ├── extension.h / extension.cpp        (BEP 10 + extension riêng, sau này BEP 9, 11)
│   │   ├── lsd.h / lsd.cpp                    (BEP 14 — giai đoạn 5)
│   │   ├── nat_traversal.h / nat_traversal.cpp (giai đoạn 5)
│   │   ├── encryption.h / encryption.cpp      (MSE — giai đoạn 5)
│   │   └── dht/
│   │       ├── node_id.h / node_id.cpp
│   │       ├── routing_table.h / routing_table.cpp
│   │       ├── kademlia_node.h / kademlia_node.cpp
│   │       └── krpc.h / krpc.cpp
│   ├── strategy/
│   │   ├── piece_picker.h / piece_picker.cpp   (giao diện + các chiến lược chọn piece)
│   │   ├── choker.h / choker.cpp               (giao diện + các chiến lược chia băng thông)
│   │   └── peer_policy.h / peer_policy.cpp     (giới hạn kết nối, ban peer)
│   ├── bench/
│   │   ├── bandwidth_limiter.h / bandwidth_limiter.cpp
│   │   └── metrics.h / metrics.cpp
│   └── webui/
│       ├── http_server.h / http_server.cpp
│       ├── api_routes.h / api_routes.cpp
│       └── state_bridge.h / state_bridge.cpp
├── web/
│   ├── index.html
│   ├── style.css
│   └── app.js
├── tracker/
│   ├── main.cpp
│   ├── tracker_server.h / tracker_server.cpp
│   └── peer_registry.h / peer_registry.cpp
├── tests/
│   ├── test_bencode.cpp
│   ├── test_torrent_builder.cpp
│   ├── test_torrent_file.cpp
│   ├── test_path_sanitizer.cpp
│   ├── test_piece_manager.cpp
│   ├── test_peer_protocol.cpp
│   ├── test_extension.cpp
│   ├── test_piece_picker.cpp
│   ├── test_choker.cpp
│   ├── test_kademlia.cpp
│   └── samples/
│       └── malformed/            (bencode / .torrent dị dạng để test parser)
└── data/
    ├── torrents/
    └── downloads/
```

Ghi chú:
- **Dùng file header (.h):** mỗi module có 1 `.h` chứa khai báo công khai, `.cpp` chứa cài đặt. File cần module khác thì `#include` header, không tự chép khai báo. Mỗi header bắt đầu bằng `#pragma once`; chi tiết nội bộ để trong `.cpp`. `main.cpp` không cần header.
- `piece_manager`, `piece_picker`, `peer_connection` và `choker` gọi chéo nhau, nên trong header ưu tiên forward declare (`class PieceManager;`) khi chỉ cần con trỏ / tham chiếu, và `#include` đầy đủ trong `.cpp` để tránh include vòng.
- `core/bencode` dùng chung cho builder, parser, tracker client và DHT. Khi đọc phải giữ vị trí byte của `info` để tính `info_hash`.
- `core/path_sanitizer`: gom các kiểm tra đường dẫn vào 1 nơi để test riêng, dùng ở `torrent_file` và `piece_manager`.
- `strategy/`: `piece_picker` và `choker` là logic thuần, nhận đầu vào là trạng thái (bitfield của peer, thống kê tốc độ, phần còn thiếu) và trả về quyết định (block nào xin, peer nào unchoke). Nhờ vậy test và mô phỏng được mà không cần mở mạng.
- `bench/`: bộ giới hạn băng thông và ghi số liệu, dùng chung cho thí nghiệm so sánh chiến lược.
- `webui/state_bridge`: vùng trạng thái dùng chung (có khoá) giữa thread networking và thread HTTP server.
- `web/`: file tĩnh HTML/CSS/JS, không phải code C++, không cần build.
- `tests/samples/malformed/`: mẫu bencode / `.torrent` cố tình sai (thiếu `e`, số âm, lồng quá sâu, `path` chứa `..`...) để chắc parser không crash và không ghi file ra ngoài.
- `data/`: thư mục runtime, không commit vào git.

## 7. Lộ trình phát triển

1. **Nền tảng (BEP 3 + BEP 23)** — chia bước nhỏ, mỗi bước kiểm chứng xong mới sang bước sau:
   1. Bencode (đọc / ghi) + test.
   2. `torrent_builder`, kiểm chứng bằng cách so `info_hash` với mktorrent / qBittorrent.
   3. `torrent_file` (đọc, kiểm tra, `path_sanitizer`) và `piece_manager` (ghép / đọc block theo vị trí, verify SHA-1).
   4. Tracker server + tracker client; peer wire protocol cơ bản (tải tuần tự từ 1 peer đã unchoke).
   5. Piece picker và choker phiên bản đơn giản (rarest-first + tit-for-tat).
   - **Cột mốc:** tải thành công 1 torrent 1 file rồi nhiều file **giữa client của nhóm và qBittorrent**, cả 2 chiều; sau đó giữa 3 laptop, kể cả piece được trao đổi chéo giữa các peer chứ không chỉ từ 1 seeder.
2. **Web UI cơ bản** — nhúng HTTP server, hiển thị torrent + peer + tiến trình, thêm / dừng / xoá torrent, nối vào core.
3. **Tầng thiết kế riêng** — thêm các chiến lược chọn piece và chia băng thông (mục 5.5), bộ giới hạn băng thông và ghi số liệu, extension protocol BEP 10 cùng ít nhất một extension riêng, chạy thí nghiệm so sánh.
4. **DHT + mở rộng** — Kademlia theo BEP 5 (test bằng peer ảo, sau đó thử với DHT công khai bằng torrent hợp pháp), PEX (BEP 11), magnet (BEP 9).
5. **Hoàn thiện** — LSD (BEP 14), UPnP / NAT-PMP, mã hoá MSE, dọn UI, kiểm thử tải và dữ liệu dị dạng, tổng hợp báo cáo.

## 8. Kế hoạch test

**Đối chứng với client thật (quan trọng nhất):**
- Cài qBittorrent (hoặc Transmission) trên 1 laptop. Ban đầu tắt DHT / PEX / LSD của nó, chỉ dùng tracker của nhóm để tránh nhiễu.
- Chiều 1: qBittorrent seed, client của nhóm tải, so hash file với bản gốc.
- Chiều 2: client của nhóm seed, qBittorrent tải.
- `.torrent` do `torrent_builder` tạo mở được trong qBittorrent; `.torrent` do qBittorrent tạo mở được trong client của nhóm; `info_hash` trùng khớp.
- qBittorrent announce tới tracker của nhóm và ngược lại.
- Khi có extension riêng: xác nhận client thật không lỗi khi gặp client của nhóm bật extension, và extension chỉ dùng giữa 2 client của nhóm.

**Test 3 laptop:**
- Mỗi laptop chạy 1 client; 1 laptop kiêm tracker.
- A seed torrent nhiều file → B và C cùng tải, kiểm tra piece trao đổi chéo giữa B và C.
- Ngắt kết nối 1 peer giữa chừng, kiểm tra client xin lại từ peer khác.

**Thí nghiệm so sánh chiến lược:**
- Cố định băng thông bằng `bench`, cố định số peer ảo, thay đổi chiến lược piece picker / choker; lặp mỗi cấu hình nhiều lần, so thời gian hoàn thành, độ công bằng và số block trùng lặp.

**Test an toàn:**
- Parser đọc các mẫu trong `tests/samples/malformed/`: không crash, không cấp phát bộ nhớ quá mức.
- `.torrent` có `path` chứa `..`, đường dẫn tuyệt đối hoặc tên dành riêng phải bị từ chối, không có file nào được tạo ngoài thư mục tải.
- Gửi message quá dài, request ngoài phạm vi, bitfield sai độ dài, extension lạ: kết nối bị đóng hoặc bỏ qua đúng cách và client vẫn chạy.

## 9. Phân công chi tiết cho nhóm 3 người

Web UI khá đơn giản, nên khối lượng được chia lại: mỗi người có một mảng lõi độc lập, và Người 3 nhận thêm phần chiến lược và đo lường thay cho việc chỉ làm giao diện. Mỗi người tự lập trình, tự kiểm thử, tự viết báo cáo cho mảng của mình.

| Người | Mảng | Thành phần |
|---|---|---|
| 1 | **Metadata, lưu trữ và tìm peer** | `bencode`, `torrent_file`, `torrent_builder`, `path_sanitizer`, `piece_manager`, `hash_utils`, tracker server + `tracker_client`, DHT (BEP 5), LSD |
| 2 | **Giao thức truyền và mở rộng** | `peer_connection`, `peer_protocol`, `extension` (BEP 10 + extension riêng, PEX, magnet), MSE, NAT |
| 3 | **Chiến lược, đo lường và giao diện** | `piece_picker`, `choker`, `peer_policy`, `bench`, Web UI, bộ kiểm thử đối chứng với client thật |

### Tóm tắt dễ hiểu (cho người mới bắt đầu tìm hiểu)

- **Người 1 – Kho và danh bạ**: tạo "phiếu mô tả" (`.torrent`) cho gói hàng, cắt file thành từng miếng và kiểm tra mã từng miếng, đọc ghi file trên ổ đĩa, và lo phần giúp các máy tìm ra nhau (danh bạ tracker, sau này tự tìm nhau qua DHT).
- **Người 2 – Đưa thư**: nói chuyện với các máy khác đúng "ngôn ngữ" BitTorrent, gửi nhận từng miếng, và thêm các "ngôn ngữ mở rộng" riêng của nhóm.
- **Người 3 – Chiến lược gia và bảng theo dõi**: quyết định xin miếng nào trước, cho ai tải trước, đo xem chiến lược nào tốt hơn, dựng màn hình theo dõi và dựng môi trường thử nghiệm với qBittorrent.

### Người 1 — Metadata, lưu trữ và tìm peer

**Giai đoạn 1**
- Lập trình: `bencode`, `torrent_builder` (lệnh CLI `create`), `torrent_file` + `path_sanitizer`, `piece_manager` (block / piece, SHA-1, ghi file, ánh xạ piece → file), tracker server và tracker client (compact peers) — `src/core/`, `src/network/tracker_client`, `tracker/`.
- Kiểm thử: `info_hash` khớp mktorrent / qBittorrent; builder với 1 file, thư mục lồng nhau, file rỗng, file không chia hết `piece length`, file lớn; parser với mẫu dị dạng; tracker trả đúng peer khi nhiều client announce cùng lúc, và làm việc đúng với qBittorrent.
- Báo cáo: định dạng bencode và `.torrent`, cách tính `info_hash`, thuật toán chia piece và verify hash, luồng tracker.

**Giai đoạn 4 (DHT)**
- Lập trình: DHT theo BEP 5 (`routing_table`, `krpc`, `kademlia_node`).
- Kiểm thử: mô phỏng 15-30 peer ảo (nhiều process mỗi laptop); thử với DHT của qBittorrent.
- Báo cáo: giải thích Kademlia và KRPC, so sánh ưu / nhược với tracker.

**Giai đoạn 5**
- Lập trình: LSD (BEP 14).
- Kiểm thử: 3 laptop trong LAN tự tìm thấy nhau không cần tracker.

### Người 2 — Giao thức truyền và mở rộng

**Giai đoạn 1**
- Lập trình: `peer_connection`, `peer_protocol` (handshake 68 byte, đọc / ghi message, giới hạn phòng thủ) dùng Asio — `src/network/`.
- Kiểm thử: trao đổi piece qua LAN giữa 3 laptop và với qBittorrent; xử lý peer ngắt kết nối giữa chừng; message quá dài hoặc dị dạng.
- Báo cáo: mô tả wire protocol và các loại message.

**Giai đoạn 3**
- Lập trình: `extension` — BEP 10 và ít nhất một extension riêng (mục 5.4).
- Kiểm thử: hai client của nhóm dùng được extension; client thật không lỗi và extension tự bị bỏ qua khi kết nối với client thật; peer không khai báo extension thì không bị gửi.
- Báo cáo: cơ chế extension protocol, thiết kế extension riêng và giới hạn của nó.

**Giai đoạn 4**
- Lập trình: PEX (BEP 11) và magnet link qua metadata exchange (BEP 9).
- Kiểm thử: thêm torrent bằng magnet từ swarm do qBittorrent seed; PEX làm tăng số peer biết nhau.

**Giai đoạn 5**
- Lập trình: NAT traversal cơ bản (UPnP / NAT-PMP), mã hoá kết nối MSE.
- Kiểm thử: kết nối 2 peer qua mạng khác nhau nếu điều kiện cho phép; kết nối mã hoá vẫn trao đổi piece bình thường, kể cả với qBittorrent.
- Báo cáo: phần bảo mật kết nối và NAT.

### Người 3 — Chiến lược, đo lường và giao diện

**Giai đoạn 1 (làm song song, không phụ thuộc mạng)**
- Lập trình: `piece_picker` và `choker` phiên bản đầu (rarest-first, tit-for-tat) dưới dạng logic thuần có giao diện chung, nhận trạng thái vào và trả quyết định; test bằng dữ liệu giả và mô phỏng nhiều peer, không cần mở kết nối.
- Dựng môi trường đối chứng: cài qBittorrent trên 1 laptop, viết hướng dẫn và script kiểm tra hash cho cả nhóm dùng; viết mẫu `.torrent` dị dạng cho `tests/samples/malformed/`.
- Kiểm thử: rarest-first chọn đúng piece hiếm; endgame không xin thừa; choker giữ đúng số slot và xoay optimistic unchoke.
- Báo cáo: thuật toán chọn piece và tit-for-tat.

**Giai đoạn 2 (Web UI cơ bản)**
- Lập trình: nhúng `cpp-httplib`, route file tĩnh + API `/api/status` và thêm / dừng / xoá torrent; bind `127.0.0.1`, kiểm tra `Host`, token phiên; tách thread qua `state_bridge` — `src/webui/`, `web/`.
- Kiểm thử: trang không đứng khi mạng chậm hoặc mất kết nối; dữ liệu API khớp trạng thái thật; thử gọi API từ một trang web khác nguồn và xác nhận bị chặn.
- Báo cáo: kiến trúc HTTP server nhúng, tách thread, vì sao chọn polling.

**Giai đoạn 3 (Tầng thiết kế riêng)**
- Lập trình: thêm các chiến lược chọn piece và chia băng thông (mục 5.5), `peer_policy` (giới hạn kết nối, ban peer sai hash), `bench` (bộ giới hạn băng thông + ghi số liệu), biểu đồ đơn giản và chọn chiến lược trên web UI.
- Kiểm thử: thí nghiệm so sánh chiến lược với số peer ảo và băng thông cố định, lặp nhiều lần để số liệu đáng tin.
- Báo cáo: phân tích kết quả so sánh, chiến lược nào tốt hơn trong điều kiện nào và vì sao.

**Giai đoạn 5**
- Hoàn thiện web UI, tổng hợp báo cáo chung của cả nhóm, chạy toàn bộ kịch bản đối chứng với client thật một lượt cuối.

### Việc chung, không chia độc lập

- **Chốt giao diện giữa các module ngay đầu giai đoạn 1**, viết thành các file `.h` trước khi cài đặt: `PieceManager` (hỏi piece nào đã có, đọc / ghi block), thông tin trạng thái của một peer, giao diện của `piece_picker` và `choker`. Người 3 cần các giao diện này để làm việc song song từ đầu mà không chờ code của người khác.
- Test đối chứng với client thật và test tích hợp toàn hệ thống (mục 8): cả 3 người cùng làm.

## 10. Lưu ý

- `info_hash` phải tính từ đúng chuỗi byte gốc của `info` dict. Lệch 1 byte (do khoá không được sắp xếp, hoặc mã hoá lại khác bản gốc) là `info_hash` khác đi và không peer nào nhận.
- `info_hash` và `peer_id` là byte nhị phân; xử lý sai khi URL-encode hoặc khi đọc query của tracker là lỗi thường gặp.
- SHA-1 đã yếu (tấn công va chạm) nhưng vẫn bắt buộc với torrent v1; chấp nhận đánh đổi này để đổi lấy khả năng tương thích.
- `.torrent` và dữ liệu từ peer là dữ liệu không tin cậy: luôn kiểm tra kích thước, số lượng và đường dẫn trước khi xử lý.
- Đặc tả dày và có nhiều trường hợp biên. Đọc BEP trực tiếp từ bittorrent.org/beps và đối chiếu bằng cách bắt gói tin (Wireshark) khi qBittorrent nói chuyện với client của nhóm.
- Thí nghiệm so sánh chiến lược cần điều kiện tái lập được: dùng bộ giới hạn băng thông của `bench` và lặp nhiều lần, không dựa vào tốc độ mạng LAN ngẫu nhiên.
- Mạng LAN test nội bộ không cần NAT traversal; phần này chỉ cần khi các peer ở mạng khác nhau qua Internet.
- Chỉ dùng nội dung hợp pháp khi test, tránh vào swarm công khai với nội dung có bản quyền.
