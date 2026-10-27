#include "choker.h"
#include <algorithm>

namespace p2p {
namespace strategy {

TitForTatChoker::TitForTatChoker(TitForTatConfig config, unsigned int seed)
    : config_(config), rng_(seed) {
    if (config_.regular_slots < 0) config_.regular_slots = 0;
    if (config_.optimistic_rotation_rounds < 1) config_.optimistic_rotation_rounds = 1;
}

std::vector<std::string> TitForTatChoker::update_chokes(const std::vector<PeerStatus>& peers, bool is_seeding_only) {
    std::vector<std::string> unchoked_peers;

    // Bước 1: Chỉ quan tâm đến những người đang muốn tải dữ liệu từ mình.
    std::vector<PeerStatus> interested_peers;
    for (const auto& peer : peers) {
        if (peer.is_interested) {
            interested_peers.push_back(peer);
        }
    }

    // Bước 2: Sắp xếp theo tốc độ. Hòa thì xếp theo peer_id để kết quả ổn định,
    // không phụ thuộc thứ tự danh sách đầu vào.
    std::sort(interested_peers.begin(), interested_peers.end(),
              [is_seeding_only](const PeerStatus& a, const PeerStatus& b) {
                  const double ra = is_seeding_only ? a.upload_rate : a.download_rate;
                  const double rb = is_seeding_only ? b.upload_rate : b.download_rate;
                  if (ra != rb) return ra > rb;
                  return a.peer_id < b.peer_id;
              });

    // Bước 3: Cấp slot chính thức theo tốc độ.
    const size_t regular = static_cast<size_t>(config_.regular_slots);
    std::vector<std::string> candidates;  // những peer interested chưa được slot chính thức
    for (size_t i = 0; i < interested_peers.size(); ++i) {
        if (i < regular) {
            unchoked_peers.push_back(interested_peers[i].peer_id);
        } else {
            candidates.push_back(interested_peers[i].peer_id);
        }
    }

    // Bước 4: Slot optimistic. Giữ nguyên peer cũ cho đủ N vòng nếu nó còn hợp lệ
    // (còn kết nối, còn interested, chưa lọt vào top theo tốc độ).
    if (candidates.empty()) {
        current_optimistic_peer_.clear();
        rounds_held_ = 0;
        return unchoked_peers;
    }

    const bool still_valid =
        !current_optimistic_peer_.empty() &&
        std::find(candidates.begin(), candidates.end(), current_optimistic_peer_) != candidates.end();

    if (still_valid && rounds_held_ < config_.optimistic_rotation_rounds) {
        ++rounds_held_;
    } else {
        // Đến lúc xoay (hoặc peer cũ không còn hợp lệ): bốc người mới.
        // Nếu còn lựa chọn khác thì tránh bốc lại đúng người vừa giữ.
        std::vector<std::string> pool;
        for (const auto& id : candidates) {
            if (id != current_optimistic_peer_) pool.push_back(id);
        }
        if (pool.empty()) pool = candidates;

        std::uniform_int_distribution<size_t> distrib(0, pool.size() - 1);
        current_optimistic_peer_ = pool[distrib(rng_)];
        rounds_held_ = 1;
    }

    unchoked_peers.push_back(current_optimistic_peer_);
    return unchoked_peers;
}

} // namespace strategy
} // namespace p2p
