#pragma once

#include "network/peer_protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pbl4::network {

// Chính sách mã hóa kết nối (MSE / PE)
enum class EncryptionPolicy {
	Disabled,  // Không dùng mã hóa (chỉ chấp nhận Plaintext BEP 3)
	Tolerant,  // Ưu tiên mã hóa MSE; nếu peer không hỗ trợ thì fallback về Plaintext
	Forced     // Bắt buộc mã hóa MSE; từ chối kết nối nếu không thể mã hóa
};

// Chế độ mật mã được thương lượng trong handshake MSE
enum class EncryptionCrypto : std::uint32_t {
	None = 0x00,
	Plaintext = 0x01,  // Chỉ mã hóa header trong handshake, payload truyền thô
	RC4 = 0x02,        // Mã hóa toàn bộ luồng (Header, Message, Piece data)
	Both = 0x03        // Hỗ trợ cả Plaintext và RC4
};

// Cài đặt chuẩn bộ mã hóa dòng RC4 (ARC4) tuân thủ đặc tả BitTorrent MSE.
// Tự chứa, tối ưu tốc độ và an toàn bộ nhớ.
class Rc4Cipher {
public:
	Rc4Cipher() noexcept;
	explicit Rc4Cipher(const Byte* key, std::size_t key_len) noexcept;

	void init(const Byte* key, std::size_t key_len) noexcept;
	void process(Byte* data, std::size_t len) noexcept;
	void process(const Byte* in, Byte* out, std::size_t len) noexcept;
	void discard(std::size_t count) noexcept;

private:
	std::array<Byte, 256> s_{};
	Byte i_{0};
	Byte j_{0};
};

// Độ dài khóa Diffie-Hellman 768-bit (96 byte) theo đặc tả BitTorrent MSE
constexpr std::size_t kDhKeySize = 96;
constexpr std::size_t kSha1DigestSize = 20;
constexpr std::size_t kMaxMsePadding = 512;
constexpr std::size_t kVerificationConstantSize = 8;

// Quản lý phiên bắt tay và mã hóa dòng Message Stream Encryption (MSE)
class MseSession {
public:
	MseSession();
	~MseSession();

	MseSession(const MseSession&) = delete;
	MseSession& operator=(const MseSession&) = delete;

	MseSession(MseSession&& other) noexcept;
	MseSession& operator=(MseSession&& other) noexcept;

	// 1. Sinh cặp khóa Diffie-Hellman (P 768-bit, G = 2). Trả về 96-byte public key (Ya / Yb)
	std::vector<Byte> generate_dh_keypair();

	// Lấy public key đã sinh (96 byte)
	const std::vector<Byte>& public_key() const noexcept { return local_public_key_; }

	// 2. Tính khóa bí mật chung S = (Y_remote)^X mod P (96 byte)
	bool compute_secret(const Byte* remote_public_key, std::size_t len);
	bool compute_secret(const std::vector<Byte>& remote_public_key) {
		return compute_secret(remote_public_key.data(), remote_public_key.size());
	}

	const std::vector<Byte>& shared_secret() const noexcept { return shared_secret_; }

	// 3. Khởi tạo các bộ mã hóa RC4 từ S và SKEY (info_hash), tự động bỏ 1024 byte đầu (RC4-drop1024)
	// is_initiator = true nếu là bên chủ động mở kết nối (A), false nếu là bên nhận (B)
	void init_ciphers(const InfoHash& info_hash, bool is_initiator);
	void init_ciphers(const Byte* info_hash_data, bool is_initiator);

	// 4. Mã hóa / Giải mã dữ liệu theo luồng RC4 (in-place)
	void encrypt_send(Byte* data, std::size_t len) noexcept;
	void decrypt_recv(Byte* data, std::size_t len) noexcept;

	// Trạng thái mã hóa
	bool is_active() const noexcept { return is_active_; }
	void set_active(bool active) noexcept { is_active_ = active; }

	EncryptionCrypto crypto_mode() const noexcept { return crypto_mode_; }
	void set_crypto_mode(EncryptionCrypto mode) noexcept { crypto_mode_ = mode; }

	// Các hàm tính toán hash theo chuẩn MSE:
	// req1 = SHA1("req1" + S)
	static std::array<Byte, 20> compute_req1(const std::vector<Byte>& shared_secret);
	// req2 = SHA1("req2" + SKEY)
	static std::array<Byte, 20> compute_req2(const InfoHash& info_hash);
	// req3 = SHA1("req3" + S)
	static std::array<Byte, 20> compute_req3(const std::vector<Byte>& shared_secret);
	// req2_xor_req3 = req2 ^ req3
	static std::array<Byte, 20> compute_hash_skey(const InfoHash& info_hash, const std::vector<Byte>& shared_secret);

	// Tạo gói tin Initiator Step 1: Ya + PadA
	std::vector<Byte> make_initiator_step1(std::size_t pad_len = 0);

	// Tạo gói tin Receiver Step 2: Yb + PadB
	std::vector<Byte> make_receiver_step2(std::size_t pad_len = 0);

	// Tạo gói tin Initiator Step 3:
	// req1 (20) + (req2 ^ req3) (20) + Encrypted(VC (8) + crypto_provide (4) + len(PadC) (2) + PadC + len(IA) (2) + IA)
	std::vector<Byte> make_initiator_step3(
		const InfoHash& info_hash,
		EncryptionCrypto crypto_provide = EncryptionCrypto::Both,
		const Bytes& initial_payload = {},
		std::size_t pad_c_len = 0);

	// Tạo gói tin Receiver Step 4:
	// Encrypted(VC (8) + crypto_select (4) + len(PadD) (2) + PadD)
	std::vector<Byte> make_receiver_step4(
		EncryptionCrypto crypto_select = EncryptionCrypto::RC4,
		std::size_t pad_d_len = 0);

	// Giải mã và phân tích phản hồi Receiver Step 4 trên Initiator
	bool parse_receiver_step4(const Byte* data, std::size_t len, EncryptionCrypto& out_selected);

	// Giải mã và phân tích phản hồi Initiator Step 3 trên Receiver
	bool parse_initiator_step3(
		const Byte* data,
		std::size_t len,
		const std::vector<InfoHash>& known_hashes,
		InfoHash& matched_hash,
		EncryptionCrypto& out_provided,
		Bytes& out_initial_payload);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;

	std::vector<Byte> local_public_key_;
	std::vector<Byte> shared_secret_;
	Rc4Cipher send_cipher_;
	Rc4Cipher recv_cipher_;
	bool is_active_{false};
	EncryptionCrypto crypto_mode_{EncryptionCrypto::RC4};
};

}  // namespace pbl4::network

