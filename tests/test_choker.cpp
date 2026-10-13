#include <iostream>
#include <vector>
#include <string>
#include "../src/strategy/choker.h"

using namespace p2p::strategy;

int main() {
    TitForTatChoker choker;
    
    // Giả lập trạng thái của 5 máy trạm trong mạng
    std::vector<PeerStatus> peers = {
        {"Peer_A", true, true, 50.0, 10.0},   // Tốc độ tải về: 50 KB/s
        {"Peer_B", true, true, 120.0, 15.0},  // Tốc độ tải về: 120 KB/s (Nhanh nhất)
        {"Peer_C", true, true, 80.0, 12.0},   // Tốc độ tải về: 80 KB/s (Nhanh thứ hai)
        {"Peer_D", false, true, 200.0, 20.0}, // Rất nhanh, nhưng KHÔNG muốn tải (is_interested = false)
        {"Peer_E", true, true, 60.0, 5.0}     // Tốc độ tải về: 60 KB/s (Nhanh thứ ba)
    };
    
    // Chạy thuật toán lúc đang tải (is_seeding_only = false)
    std::vector<std::string> unchoked = choker.update_chokes(peers, false);
    
    std::cout << "Kiem tra thuat toan Tit-for-tat:" << std::endl;
    std::cout << "Danh sach Peer duoc mo khoa (unchoked):" << std::endl;
    
    bool has_b = false, has_c = false, has_e = false;
    for (const auto& id : unchoked) {
        std::cout << "- " << id << std::endl;
        if (id == "Peer_B") has_b = true;
        if (id == "Peer_C") has_c = true;
        if (id == "Peer_E") has_e = true;
    }
    
    // Kỳ vọng: Peer_B, Peer_C, Peer_E phải lọt vào top 3 vì tốc độ cao nhất (bỏ qua D).
    // Peer_A có thể được chọn ngẫu nhiên vào slot thứ 4 (optimistic).
    if (has_b && has_c && has_e) {
        std::cout << "=> KET QUA: TEST PASS! Thuat toan uu tien dung nguoi." << std::endl;
    } else {
        std::cout << "=> KET QUA: TEST FAIL! Kiem tra lai logic sap xep." << std::endl;
    }
    
    return 0;
}