#include "network/encryption.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

using namespace pbl4::network;

void test_rc4_cipher_basic_and_symmetric() {
	std::cout << "[TEST] RC4 cipher basic encryption and decryption... ";
	const Byte key[] = "MySecretRC4Key123";
	const std::size_t key_len = std::strlen(reinterpret_cast<const char*>(key));

	Rc4Cipher enc(key, key_len);
	Rc4Cipher dec(key, key_len);

	const std::string original = "BitTorrent Message Stream Encryption Test Payload 2026!";
	std::vector<Byte> buffer(original.begin(), original.end());

	enc.process(buffer.data(), buffer.size());
	// Sau khi mã hóa, buffer phải khác ban đầu
	assert(std::string(buffer.begin(), buffer.end()) != original);

	dec.process(buffer.data(), buffer.size());
	// Sau khi giải mã, buffer phải trùng khớp 100% với ban đầu
	assert(std::string(buffer.begin(), buffer.end()) == original);

	std::cout << "PASSED\n";
}

void test_rc4_cipher_discard() {
	std::cout << "[TEST] RC4 cipher discard (RC4-drop1024)... ";
	const Byte key[] = "StreamDropKey456";
	const std::size_t key_len = std::strlen(reinterpret_cast<const char*>(key));

	Rc4Cipher c1(key, key_len);
	Rc4Cipher c2(key, key_len);

	// c1 mã hóa 1024 byte dữ liệu giả
	std::vector<Byte> dummy(1024, 0);
	c1.process(dummy.data(), dummy.size());

	// c2 gọi discard(1024)
	c2.discard(1024);

	// Bây giờ output của c1 và c2 phải khớp nhau
	std::vector<Byte> test_data = {1, 2, 3, 4, 5, 6, 7, 8};
	auto copy1 = test_data;
	auto copy2 = test_data;

	c1.process(copy1.data(), copy1.size());
	c2.process(copy2.data(), copy2.size());

	assert(copy1 == copy2);
	std::cout << "PASSED\n";
}

void test_diffie_hellman_shared_secret() {
	std::cout << "[TEST] Diffie-Hellman 768-bit key exchange... ";
	MseSession alice;
	MseSession bob;

	const auto ya = alice.generate_dh_keypair();
	const auto yb = bob.generate_dh_keypair();

	assert(ya.size() == kDhKeySize);
	assert(yb.size() == kDhKeySize);
	assert(ya != yb);

	const bool ok_alice = alice.compute_secret(yb);
	const bool ok_bob = bob.compute_secret(ya);

	assert(ok_alice);
	assert(ok_bob);
	assert(alice.shared_secret().size() == kDhKeySize);
	assert(bob.shared_secret().size() == kDhKeySize);
	// Secret S mà Alice và Bob tính được bắt buộc phải trùng khớp nhau
	assert(alice.shared_secret() == bob.shared_secret());

	std::cout << "PASSED\n";
}

void test_mse_full_handshake_flow() {
	std::cout << "[TEST] MSE full 4-step handshake and stream ciphering... ";
	MseSession initiator;
	MseSession receiver;

	InfoHash info_hash{};
	for (std::size_t i = 0; i < info_hash.size(); ++i) {
		info_hash[i] = static_cast<Byte>(0xAA + i);
	}

	// Bước 1: Initiator gửi Ya + PadA
	const auto step1 = initiator.make_initiator_step1(16);
	assert(step1.size() == kDhKeySize + 16);

	// Bước 2: Receiver nhận Ya, tính S, gửi Yb + PadB
	const bool ok_rx = receiver.compute_secret(step1.data(), kDhKeySize);
	assert(ok_rx);
	const auto step2 = receiver.make_receiver_step2(32);
	assert(step2.size() == kDhKeySize + 32);

	// Initiator nhận Yb, tính S
	const bool ok_init = initiator.compute_secret(step2.data(), kDhKeySize);
	assert(ok_init);
	assert(initiator.shared_secret() == receiver.shared_secret());

	// Bước 3: Initiator gửi req1 + hash_skey + Encrypted(VC + crypto_provide + PadC + IA)
	const Bytes initial_handshake = {19, 'B', 'i', 't', 'T', 'o', 'r', 'r', 'e', 'n', 't'};
	const auto step3 = initiator.make_initiator_step3(
		info_hash, EncryptionCrypto::Both, initial_handshake, 8);

	// Receiver phân tích bước 3
	std::vector<InfoHash> known_hashes = {info_hash};
	InfoHash matched_hash{};
	EncryptionCrypto provided = EncryptionCrypto::None;
	Bytes decoded_initial_payload;

	const bool step3_ok = receiver.parse_initiator_step3(
		step3.data(), step3.size(), known_hashes, matched_hash, provided, decoded_initial_payload);
	assert(step3_ok);
	assert(matched_hash == info_hash);
	assert(provided == EncryptionCrypto::Both);
	assert(decoded_initial_payload == initial_handshake);

	// Bước 4: Receiver gửi Encrypted(VC + crypto_select + PadD)
	const auto step4 = receiver.make_receiver_step4(EncryptionCrypto::RC4, 12);

	// Initiator phân tích bước 4
	EncryptionCrypto selected = EncryptionCrypto::None;
	const bool step4_ok = initiator.parse_receiver_step4(step4.data(), step4.size(), selected);
	assert(step4_ok);
	assert(selected == EncryptionCrypto::RC4);

	// Hai bên bắt đầu trao đổi dữ liệu mã hóa qua lại
	std::string msg_from_initiator = "Request Piece 0 Begin 0 Length 16384";
	std::vector<Byte> buf1(msg_from_initiator.begin(), msg_from_initiator.end());

	initiator.encrypt_send(buf1.data(), buf1.size());
	receiver.decrypt_recv(buf1.data(), buf1.size());
	assert(std::string(buf1.begin(), buf1.end()) == msg_from_initiator);

	std::string msg_from_receiver = "Piece Data: Block 0 Received OK";
	std::vector<Byte> buf2(msg_from_receiver.begin(), msg_from_receiver.end());

	receiver.encrypt_send(buf2.data(), buf2.size());
	initiator.decrypt_recv(buf2.data(), buf2.size());
	assert(std::string(buf2.begin(), buf2.end()) == msg_from_receiver);

	std::cout << "PASSED\n";
}

int main() {
	std::cout << "================ MSE ENCRYPTION TESTS ================\n";
	test_rc4_cipher_basic_and_symmetric();
	test_rc4_cipher_discard();
	test_diffie_hellman_shared_secret();
	test_mse_full_handshake_flow();
	std::cout << "All MSE encryption tests PASSED!\n";
	return 0;
}
