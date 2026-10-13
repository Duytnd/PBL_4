#include "choker.h"
#include <algorithm>
#include <random>

namespace p2p {
namespace strategy {

std::vector<std::string> TitForTatChoker::update_chokes(const std::vector<PeerStatus>& peers, bool is_seeding_only) {
    std::vector<std::string> unchoked_peers;
    std::vector<PeerStatus> interested_peers;

    // Bước 1: Chỉ quan tâm đến những người đang muốn tải dữ liệu từ mình (is_interested = true)
    for (const auto& peer : peers) {
        if (peer.is_interested) {
            interested_peers.push_back(peer);
        }
    }

    // Bước 2: Sắp xếp danh sách dựa trên tốc độ
    std::sort(interested_peers.begin(), interested_peers.end(),
              [is_seeding_only](const PeerStatus& a, const PeerStatus& b) {
                  // Nếu mình đã tải xong file (chỉ seed), ưu tiên người mình đẩy file cho họ nhanh nhất
                  if (is_seeding_only) {
                      return a.upload_rate > b.upload_rate;
                  } 
                  // Nếu mình đang tải, ưu tiên người gửi dữ liệu cho mình nhanh nhất
                  else {
                      return a.download_rate > b.download_rate;
                  }
              });

    // Bước 3: Cấp slot chính thức (Mặc định là 3 slot)
    std::vector<PeerStatus> remaining_peers;
    for (size_t i = 0; i < interested_peers.size(); ++i) {
        if (i < num_regular_slots) {
            unchoked_peers.push_back(interested_peers[i].peer_id);
        } else {
            remaining_peers.push_back(interested_peers[i]);
        }
    }

    // Bước 4: Cấp slot ngẫu nhiên (Optimistic unchoke - 1 slot)
    // Giúp dò tìm các peer mới có khả năng cho tốc độ cao hơn
    if (!remaining_peers.empty()) {
        // Trong môi trường thực tế, luồng thời gian bên ngoài sẽ gọi hàm xoay vòng 30s/lần.
        // Ở đây ta cài đặt logic chọn ngẫu nhiên 1 peer từ danh sách còn lại.
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> distrib(0, remaining_peers.size() - 1);
        
        current_optimistic_peer = remaining_peers[distrib(gen)].peer_id;
        unchoked_peers.push_back(current_optimistic_peer);
    }

    return unchoked_peers;
}

} // namespace strategy
} // namespace p2p