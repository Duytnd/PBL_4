#include "network/encryption.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <openssl/bn.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace pbl4::network {

namespace {

constexpr char kMsePrimeHex[] =
	"FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD1"
	"29024E088A67CC74020BBEA63B139B22514A08798E3404DD"
	"EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245"
	"E485B576625E7EC6F44C42E9A63A3620FFFFFFFFFFFFFFFF";

std::array<Byte, 20> sha1_concat(const char* prefix, std::size_t prefix_len,
								 const Byte* data1, std::size_t len1,
								 const Byte* data2 = nullptr, std::size_t len2 = 0) {
	SHA_CTX ctx;
	SHA1_Init(&ctx);
	if (prefix && prefix_len > 0) {
		SHA1_Update(&ctx, prefix, prefix_len);
	}
	if (data1 && len1 > 0) {
		SHA1_Update(&ctx, data1, len1);
	}
	if (data2 && len2 > 0) {
		SHA1_Update(&ctx, data2, len2);
	}
	std::array<Byte, 20> digest{};
	SHA1_Final(digest.data(), &ctx);
	return digest;
}

}  // namespace

// ============================================================================
// Rc4Cipher Implementation
// ============================================================================

Rc4Cipher::Rc4Cipher() noexcept {
	for (std::size_t k = 0; k < 256; ++k) {
		s_[k] = static_cast<Byte>(k);
	}
}

Rc4Cipher::Rc4Cipher(const Byte* key, std::size_t key_len) noexcept {
	init(key, key_len);
}

void Rc4Cipher::init(const Byte* key, std::size_t key_len) noexcept {
	if (!key || key_len == 0) {
		return;
	}
	for (std::size_t k = 0; k < 256; ++k) {
		s_[k] = static_cast<Byte>(k);
	}
	Byte j = 0;
	for (std::size_t k = 0; k < 256; ++k) {
		j = static_cast<Byte>(j + s_[k] + key[k % key_len]);
		std::swap(s_[k], s_[j]);
	}
	i_ = 0;
	j_ = 0;
}

void Rc4Cipher::process(Byte* data, std::size_t len) noexcept {
	if (!data || len == 0) {
		return;
	}
	for (std::size_t n = 0; n < len; ++n) {
		i_ = static_cast<Byte>(i_ + 1);
		j_ = static_cast<Byte>(j_ + s_[i_]);
		std::swap(s_[i_], s_[j_]);
		const Byte k = s_[static_cast<Byte>(s_[i_] + s_[j_])];
		data[n] ^= k;
	}
}

void Rc4Cipher::process(const Byte* in, Byte* out, std::size_t len) noexcept {
	if (!in || !out || len == 0) {
		return;
	}
	if (in != out) {
		std::memcpy(out, in, len);
	}
	process(out, len);
}

void Rc4Cipher::discard(std::size_t count) noexcept {
	for (std::size_t n = 0; n < count; ++n) {
		i_ = static_cast<Byte>(i_ + 1);
		j_ = static_cast<Byte>(j_ + s_[i_]);
		std::swap(s_[i_], s_[j_]);
	}
}

// ============================================================================
// MseSession::Impl
// ============================================================================

struct MseSession::Impl {
	BIGNUM* p = nullptr;
	BIGNUM* g = nullptr;
	BIGNUM* x = nullptr;
	BIGNUM* y = nullptr;
	BN_CTX* ctx = nullptr;

	Impl() {
		ctx = BN_CTX_new();
		p = nullptr;
		BN_hex2bn(&p, kMsePrimeHex);
		g = BN_new();
		BN_set_word(g, 2);
		x = BN_new();
		y = BN_new();
	}

	~Impl() {
		if (p) BN_free(p);
		if (g) BN_free(g);
		if (x) BN_free(x);
		if (y) BN_free(y);
		if (ctx) BN_CTX_free(ctx);
	}
};

// ============================================================================
// MseSession Implementation
// ============================================================================

MseSession::MseSession() : impl_(std::make_unique<Impl>()) {}

MseSession::~MseSession() = default;

MseSession::MseSession(MseSession&& other) noexcept = default;
MseSession& MseSession::operator=(MseSession&& other) noexcept = default;

std::vector<Byte> MseSession::generate_dh_keypair() {
	if (!impl_) {
		impl_ = std::make_unique<Impl>();
	}
	// Sinh private key X ngẫu nhiên 160-bit (20 byte)
	BN_rand(impl_->x, 160, 0, 0);

	// Y = G^X mod P
	BN_mod_exp(impl_->y, impl_->g, impl_->x, impl_->p, impl_->ctx);

	local_public_key_.assign(kDhKeySize, 0);
	BN_bn2binpad(impl_->y, local_public_key_.data(), static_cast<int>(kDhKeySize));
	return local_public_key_;
}

bool MseSession::compute_secret(const Byte* remote_public_key, std::size_t len) {
	if (!remote_public_key || len != kDhKeySize || !impl_) {
		return false;
	}

	if (local_public_key_.empty()) {
		generate_dh_keypair();
	}

	BIGNUM* y_remote = BN_bin2bn(remote_public_key, static_cast<int>(len), nullptr);
	if (!y_remote) {
		return false;
	}

	// Kiểm tra phòng thủ: 1 < y_remote < p - 1
	if (BN_is_zero(y_remote) || BN_is_one(y_remote) || BN_cmp(y_remote, impl_->p) >= 0) {
		BN_free(y_remote);
		return false;
	}

	BIGNUM* s = BN_new();
	// S = (Y_remote)^X mod P
	const int ok = BN_mod_exp(s, y_remote, impl_->x, impl_->p, impl_->ctx);
	if (ok == 1) {
		shared_secret_.assign(kDhKeySize, 0);
		BN_bn2binpad(s, shared_secret_.data(), static_cast<int>(kDhKeySize));
	}

	BN_free(s);
	BN_free(y_remote);
	return ok == 1;
}

void MseSession::init_ciphers(const InfoHash& info_hash, bool is_initiator) {
	init_ciphers(info_hash.data(), is_initiator);
}

void MseSession::init_ciphers(const Byte* info_hash_data, bool is_initiator) {
	if (shared_secret_.empty() || !info_hash_data) {
		throw std::runtime_error("MSE shared secret or info_hash is empty");
	}

	// keyA = SHA1("keyA" + S + SKEY)
	const auto key_a = sha1_concat("keyA", 4, shared_secret_.data(), shared_secret_.size(),
								   info_hash_data, 20);
	// keyB = SHA1("keyB" + S + SKEY)
	const auto key_b = sha1_concat("keyB", 4, shared_secret_.data(), shared_secret_.size(),
								   info_hash_data, 20);

	if (is_initiator) {
		send_cipher_.init(key_a.data(), key_a.size());
		recv_cipher_.init(key_b.data(), key_b.size());
	} else {
		send_cipher_.init(key_b.data(), key_b.size());
		recv_cipher_.init(key_a.data(), key_a.size());
	}

	// Bỏ 1024 byte đầu theo đặc tả MSE (RC4-drop1024)
	send_cipher_.discard(1024);
	recv_cipher_.discard(1024);

	is_active_ = true;
}

void MseSession::encrypt_send(Byte* data, std::size_t len) noexcept {
	if (is_active_ && data && len > 0) {
		send_cipher_.process(data, len);
	}
}

void MseSession::decrypt_recv(Byte* data, std::size_t len) noexcept {
	if (is_active_ && data && len > 0) {
		recv_cipher_.process(data, len);
	}
}

std::array<Byte, 20> MseSession::compute_req1(const std::vector<Byte>& shared_secret) {
	return sha1_concat("req1", 4, shared_secret.data(), shared_secret.size());
}

std::array<Byte, 20> MseSession::compute_req2(const InfoHash& info_hash) {
	return sha1_concat("req2", 4, info_hash.data(), info_hash.size());
}

std::array<Byte, 20> MseSession::compute_req3(const std::vector<Byte>& shared_secret) {
	return sha1_concat("req3", 4, shared_secret.data(), shared_secret.size());
}

std::array<Byte, 20> MseSession::compute_hash_skey(const InfoHash& info_hash,
												   const std::vector<Byte>& shared_secret) {
	const auto req2 = compute_req2(info_hash);
	const auto req3 = compute_req3(shared_secret);
	std::array<Byte, 20> result{};
	for (std::size_t i = 0; i < 20; ++i) {
		result[i] = static_cast<Byte>(req2[i] ^ req3[i]);
	}
	return result;
}

std::vector<Byte> MseSession::make_initiator_step1(std::size_t pad_len) {
	if (local_public_key_.empty()) {
		generate_dh_keypair();
	}
	pad_len = std::min(pad_len, kMaxMsePadding);
	std::vector<Byte> packet;
	packet.reserve(kDhKeySize + pad_len);
	packet.insert(packet.end(), local_public_key_.begin(), local_public_key_.end());
	if (pad_len > 0) {
		std::vector<Byte> pad(pad_len);
		RAND_bytes(pad.data(), static_cast<int>(pad_len));
		packet.insert(packet.end(), pad.begin(), pad.end());
	}
	return packet;
}

std::vector<Byte> MseSession::make_receiver_step2(std::size_t pad_len) {
	if (local_public_key_.empty()) {
		generate_dh_keypair();
	}
	pad_len = std::min(pad_len, kMaxMsePadding);
	std::vector<Byte> packet;
	packet.reserve(kDhKeySize + pad_len);
	packet.insert(packet.end(), local_public_key_.begin(), local_public_key_.end());
	if (pad_len > 0) {
		std::vector<Byte> pad(pad_len);
		RAND_bytes(pad.data(), static_cast<int>(pad_len));
		packet.insert(packet.end(), pad.begin(), pad.end());
	}
	return packet;
}

std::vector<Byte> MseSession::make_initiator_step3(
	const InfoHash& info_hash,
	EncryptionCrypto crypto_provide,
	const Bytes& initial_payload,
	std::size_t pad_c_len) {
	if (shared_secret_.empty()) {
		throw std::runtime_error("MSE shared secret not established");
	}

	init_ciphers(info_hash, true);

	const auto req1 = compute_req1(shared_secret_);
	const auto hash_skey = compute_hash_skey(info_hash, shared_secret_);

	pad_c_len = std::min(pad_c_len, kMaxMsePadding);

	// Tạo payload cần mã hóa:
	// VC (8 bytes zero) + crypto_provide (4 bytes) + len(PadC) (2 bytes) + PadC + len(IA) (2 bytes) + IA
	std::vector<Byte> payload;
	payload.reserve(8 + 4 + 2 + pad_c_len + 2 + initial_payload.size());

	// VC = 8 byte 0
	payload.insert(payload.end(), 8, 0x00);

	// crypto_provide (4 byte big-endian)
	const auto cp = static_cast<std::uint32_t>(crypto_provide);
	payload.push_back(static_cast<Byte>((cp >> 24) & 0xFF));
	payload.push_back(static_cast<Byte>((cp >> 16) & 0xFF));
	payload.push_back(static_cast<Byte>((cp >> 8) & 0xFF));
	payload.push_back(static_cast<Byte>(cp & 0xFF));

	// len(PadC) (2 byte big-endian)
	payload.push_back(static_cast<Byte>((pad_c_len >> 8) & 0xFF));
	payload.push_back(static_cast<Byte>(pad_c_len & 0xFF));

	// PadC
	if (pad_c_len > 0) {
		std::vector<Byte> pad(pad_c_len);
		RAND_bytes(pad.data(), static_cast<int>(pad_c_len));
		payload.insert(payload.end(), pad.begin(), pad.end());
	}

	// len(IA) (2 byte big-endian)
	const auto ia_len = static_cast<std::uint16_t>(initial_payload.size());
	payload.push_back(static_cast<Byte>((ia_len >> 8) & 0xFF));
	payload.push_back(static_cast<Byte>(ia_len & 0xFF));

	// IA (Initial payload)
	if (!initial_payload.empty()) {
		payload.insert(payload.end(), initial_payload.begin(), initial_payload.end());
	}

	// Mã hóa payload bằng RC4 send_cipher_
	encrypt_send(payload.data(), payload.size());

	// Ghép kết quả: req1 + hash_skey + encrypted_payload
	std::vector<Byte> packet;
	packet.reserve(20 + 20 + payload.size());
	packet.insert(packet.end(), req1.begin(), req1.end());
	packet.insert(packet.end(), hash_skey.begin(), hash_skey.end());
	packet.insert(packet.end(), payload.begin(), payload.end());
	return packet;
}

std::vector<Byte> MseSession::make_receiver_step4(
	EncryptionCrypto crypto_select,
	std::size_t pad_d_len) {
	if (!is_active_) {
		throw std::runtime_error("MSE ciphers not initialized on receiver");
	}

	pad_d_len = std::min(pad_d_len, kMaxMsePadding);

	// Payload: VC (8 byte 0) + crypto_select (4 byte) + len(PadD) (2 byte) + PadD
	std::vector<Byte> payload;
	payload.reserve(8 + 4 + 2 + pad_d_len);
	payload.insert(payload.end(), 8, 0x00);

	const auto cs = static_cast<std::uint32_t>(crypto_select);
	payload.push_back(static_cast<Byte>((cs >> 24) & 0xFF));
	payload.push_back(static_cast<Byte>((cs >> 16) & 0xFF));
	payload.push_back(static_cast<Byte>((cs >> 8) & 0xFF));
	payload.push_back(static_cast<Byte>(cs & 0xFF));

	payload.push_back(static_cast<Byte>((pad_d_len >> 8) & 0xFF));
	payload.push_back(static_cast<Byte>(pad_d_len & 0xFF));

	if (pad_d_len > 0) {
		std::vector<Byte> pad(pad_d_len);
		RAND_bytes(pad.data(), static_cast<int>(pad_d_len));
		payload.insert(payload.end(), pad.begin(), pad.end());
	}

	encrypt_send(payload.data(), payload.size());
	return payload;
}

bool MseSession::parse_receiver_step4(const Byte* data, std::size_t len,
									  EncryptionCrypto& out_selected) {
	if (!data || len < 14) {  // 8 (VC) + 4 (crypto_select) + 2 (len PadD)
		return false;
	}

	std::vector<Byte> buf(data, data + len);
	decrypt_recv(buf.data(), buf.size());

	// Kiểm tra VC: 8 byte đầu phải là 0x00
	for (std::size_t i = 0; i < 8; ++i) {
		if (buf[i] != 0x00) {
			return false;
		}
	}

	const std::uint32_t cs = (static_cast<std::uint32_t>(buf[8]) << 24) |
							 (static_cast<std::uint32_t>(buf[9]) << 16) |
							 (static_cast<std::uint32_t>(buf[10]) << 8) |
							 static_cast<std::uint32_t>(buf[11]);
	out_selected = static_cast<EncryptionCrypto>(cs);
	crypto_mode_ = out_selected;
	return true;
}

bool MseSession::parse_initiator_step3(
	const Byte* data,
	std::size_t len,
	const std::vector<InfoHash>& known_hashes,
	InfoHash& matched_hash,
	EncryptionCrypto& out_provided,
	Bytes& out_initial_payload) {
	if (!data || len < 40 + 14) {  // req1 (20) + hash_skey (20) + VC (8) + crypto (4) + pad_len (2)
		return false;
	}

	// 1. Kiểm tra req1 khớp với shared_secret_
	const auto expected_req1 = compute_req1(shared_secret_);
	if (std::memcmp(data, expected_req1.data(), 20) != 0) {
		return false;
	}

	// 2. Tìm SKEY (info_hash) phù hợp từ danh sách known_hashes
	const auto req3 = compute_req3(shared_secret_);
	bool found = false;
	for (const auto& h : known_hashes) {
		const auto req2 = compute_req2(h);
		std::array<Byte, 20> expected_hash_skey{};
		for (std::size_t i = 0; i < 20; ++i) {
			expected_hash_skey[i] = static_cast<Byte>(req2[i] ^ req3[i]);
		}
		if (std::memcmp(data + 20, expected_hash_skey.data(), 20) == 0) {
			matched_hash = h;
			found = true;
			break;
		}
	}

	if (!found) {
		return false;
	}

	// 3. Khởi tạo cipher phía Receiver (is_initiator = false)
	init_ciphers(matched_hash, false);

	// 4. Giải mã phần payload còn lại
	const std::size_t payload_len = len - 40;
	std::vector<Byte> buf(data + 40, data + len);
	decrypt_recv(buf.data(), payload_len);

	// 5. Kiểm tra VC = 8 byte 0
	for (std::size_t i = 0; i < 8; ++i) {
		if (buf[i] != 0x00) {
			return false;
		}
	}

	// crypto_provide
	const std::uint32_t cp = (static_cast<std::uint32_t>(buf[8]) << 24) |
							 (static_cast<std::uint32_t>(buf[9]) << 16) |
							 (static_cast<std::uint32_t>(buf[10]) << 8) |
							 static_cast<std::uint32_t>(buf[11]);
	out_provided = static_cast<EncryptionCrypto>(cp);

	// len(PadC)
	const std::uint16_t pad_c_len = (static_cast<std::uint16_t>(buf[12]) << 8) |
									static_cast<std::uint16_t>(buf[13]);
	std::size_t offset = 14 + pad_c_len;
	if (offset + 2 > payload_len) {
		return false;
	}

	// len(IA)
	const std::uint16_t ia_len = (static_cast<std::uint16_t>(buf[offset]) << 8) |
								 static_cast<std::uint16_t>(buf[offset + 1]);
	offset += 2;
	if (offset + ia_len > payload_len) {
		return false;
	}

	if (ia_len > 0) {
		out_initial_payload.assign(buf.begin() + static_cast<std::ptrdiff_t>(offset),
								  buf.begin() + static_cast<std::ptrdiff_t>(offset + ia_len));
	} else {
		out_initial_payload.clear();
	}

	return true;
}

}  // namespace pbl4::network
