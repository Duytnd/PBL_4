#include <iostream>
#include <vector>
#include "../src/strategy/piece_picker.h"

using namespace p2p::strategy;

int main() {
    RarestFirstPicker picker;
    
    // Giả lập trạng thái của Thịnh: Cần tải file gồm 4 mảnh. Đã tải xong mảnh 0 và 2. 
    // Đang thiếu mảnh 1 và 3.
    std::vector<bool> my_bitfield = {true, false, true, false};
    
    // Giả lập trạng thái mạng: Có 3 máy trạm khác đang kết nối
    std::vector<std::vector<bool>> peers = {
        {true, true, true, false},  // Máy A: Đang giữ mảnh 1
        {false, false, false, false}, // Máy B: Mới vào mạng, chưa có gì
        {true, true, true, true}    // Máy C: Đã tải xong 100%, có cả mảnh 1 và mảnh 3
    };
    
    // Phân tích logic:
    // - Mình thiếu mảnh 1 và 3.
    // - Mảnh 1 đang có 2 người giữ (Máy A và Máy C).
    // - Mảnh 3 cực hiếm, chỉ có 1 người giữ (Máy C).
    // => KỲ VỌNG: Thuật toán phải chỉ định tải mảnh số 3.

    int next_piece = picker.pick_next_piece(my_bitfield, peers);
    
    std::cout << "Kiem tra Rarest-first picker:" << std::endl;
    std::cout << "Manh tiep theo duoc chon la: " << next_piece << std::endl;
    
    if (next_piece == 3) {
        std::cout << "=> KET QUA: TEST PASS! Thuat toan hoat dong chinh xac." << std::endl;
    } else {
        std::cout << "=> KET QUA: TEST FAIL! Kiem tra lai logic." << std::endl;
    }
    
    return 0;
}