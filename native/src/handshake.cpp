#include "aicam/handshake.h"
#include "aicam/protocol.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>

#pragma comment(lib, "bcrypt.lib")

namespace aicam::protocol {
namespace {

constexpr std::size_t kMaxHelloTextBytes = 128;
constexpr std::string_view kTranscriptContext = "AiCam/v1/handshake";
constexpr std::string_view kSessionContext = "AiCam/v1/session";
constexpr std::string_view kPhoneProofContext = "AiCam/v1/PHONE";
constexpr std::string_view kReceiverProofContext = "AiCam/v1/RECEIVER";

class SecretHandle final {
 public:
  ~SecretHandle() {
    if (value_ != nullptr) {
      BCryptDestroySecret(value_);
    }
  }
  BCRYPT_SECRET_HANDLE* put() { return &value_; }
  BCRYPT_SECRET_HANDLE get() const { return value_; }

 private:
  BCRYPT_SECRET_HANDLE value_ = nullptr;
};

class KeyHandle final {
 public:
  ~KeyHandle() {
    if (value_ != nullptr) {
      BCryptDestroyKey(value_);
    }
  }
  BCRYPT_KEY_HANDLE* put() { return &value_; }
  BCRYPT_KEY_HANDLE get() const { return value_; }

 private:
  BCRYPT_KEY_HANDLE value_ = nullptr;
};

class ByteVectorWiper final {
 public:
  explicit ByteVectorWiper(std::vector<std::uint8_t>* value) : value_(value) {}
  ~ByteVectorWiper() {
    if (value_ != nullptr && !value_->empty()) {
      SecureZeroMemory(value_->data(), value_->size());
    }
  }

 private:
  std::vector<std::uint8_t>* value_;
};

class SecretWiper final {
 public:
  explicit SecretWiper(std::array<std::uint8_t, 32>* value) : value_(value) {}
  ~SecretWiper() {
    if (value_ != nullptr) {
      SecureZeroMemory(value_->data(), value_->size());
    }
  }

 private:
  std::array<std::uint8_t, 32>* value_;
};

void AppendBigEndian16(std::uint16_t value,
                       std::vector<std::uint8_t>* output) {
  output->push_back(static_cast<std::uint8_t>(value >> 8));
  output->push_back(static_cast<std::uint8_t>(value));
}

void AppendBigEndian32(std::uint32_t value,
                       std::vector<std::uint8_t>* output) {
  output->push_back(static_cast<std::uint8_t>(value >> 24));
  output->push_back(static_cast<std::uint8_t>(value >> 16));
  output->push_back(static_cast<std::uint8_t>(value >> 8));
  output->push_back(static_cast<std::uint8_t>(value));
}

std::uint16_t ReadBigEndian16(const std::uint8_t* input) {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(input[0]) << 8) | input[1]);
}

std::uint32_t ReadBigEndian32(const std::uint8_t* input) {
  return (static_cast<std::uint32_t>(input[0]) << 24) |
         (static_cast<std::uint32_t>(input[1]) << 16) |
         (static_cast<std::uint32_t>(input[2]) << 8) |
         static_cast<std::uint32_t>(input[3]);
}

bool IsValidUtf8(std::string_view value) {
  std::size_t index = 0;
  while (index < value.size()) {
    const auto lead = static_cast<std::uint8_t>(value[index]);
    if (lead <= 0x7f) {
      ++index;
      continue;
    }

    std::size_t continuation_count = 0;
    std::uint32_t code_point = 0;
    if ((lead & 0xe0) == 0xc0) {
      continuation_count = 1;
      code_point = lead & 0x1f;
      if (code_point < 2) {
        return false;
      }
    } else if ((lead & 0xf0) == 0xe0) {
      continuation_count = 2;
      code_point = lead & 0x0f;
    } else if ((lead & 0xf8) == 0xf0) {
      continuation_count = 3;
      code_point = lead & 0x07;
    } else {
      return false;
    }

    if (index + continuation_count >= value.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
      const auto continuation =
          static_cast<std::uint8_t>(value[index + offset]);
      if ((continuation & 0xc0) != 0x80) {
        return false;
      }
      code_point = (code_point << 6) | (continuation & 0x3f);
    }
    if ((continuation_count == 2 && code_point < 0x800) ||
        (continuation_count == 3 && code_point < 0x10000) ||
        code_point > 0x10ffff ||
        (code_point >= 0xd800 && code_point <= 0xdfff)) {
      return false;
    }
    index += continuation_count + 1;
  }
  return true;
}

bool AppendText(std::string_view value, std::vector<std::uint8_t>* output,
                std::string* error) {
  if (value.size() > kMaxHelloTextBytes ||
      value.size() > std::numeric_limits<std::uint16_t>::max() ||
      !IsValidUtf8(value)) {
    *error = "HELLO text fields must be valid UTF-8 and at most 128 bytes.";
    return false;
  }
  AppendBigEndian16(static_cast<std::uint16_t>(value.size()), output);
  output->insert(output->end(), value.begin(), value.end());
  return true;
}

bool ReadText(std::span<const std::uint8_t> payload, std::size_t* offset,
              std::string* output, std::string* error) {
  if (*offset + 2 > payload.size()) {
    *error = "Truncated HELLO text length.";
    return false;
  }
  const std::uint16_t size = ReadBigEndian16(payload.data() + *offset);
  *offset += 2;
  if (size > kMaxHelloTextBytes || *offset + size > payload.size()) {
    *error = "HELLO text length is invalid.";
    return false;
  }
  output->assign(reinterpret_cast<const char*>(payload.data() + *offset), size);
  *offset += size;
  if (!IsValidUtf8(*output)) {
    *error = "HELLO text is not valid UTF-8.";
    return false;
  }
  return true;
}

bool IsValidHello(const Hello& hello, std::string* error) {
  if (hello.role != Role::kSender && hello.role != Role::kReceiver) {
    *error = "HELLO role is unknown.";
    return false;
  }
  if (hello.action != Action::kPair && hello.action != Action::kConnect) {
    *error = "HELLO action is unknown.";
    return false;
  }
  if (hello.public_key[0] != 0x04) {
    *error = "HELLO P-256 public key is not SEC1 uncompressed form.";
    return false;
  }
  if (hello.action == Action::kPair && !hello.device_id.empty()) {
    *error = "Pairing HELLO must not contain a device ID.";
    return false;
  }
  if (hello.action == Action::kConnect && hello.device_id.empty()) {
    *error = "Connect HELLO must contain a device ID.";
    return false;
  }
  if (hello.device_name.size() > kMaxHelloTextBytes ||
      hello.model.size() > kMaxHelloTextBytes ||
      hello.device_id.size() > kMaxHelloTextBytes ||
      !IsValidUtf8(hello.device_name) || !IsValidUtf8(hello.model) ||
      !IsValidUtf8(hello.device_id)) {
    *error = "HELLO text fields must be valid UTF-8 and at most 128 bytes.";
    return false;
  }
  return true;
}

}  // namespace

SessionKeys::~SessionKeys() {
  SecureZeroMemory(phone_to_pc.data(), phone_to_pc.size());
  SecureZeroMemory(pc_to_phone.data(), pc_to_phone.size());
}

bool EncodeHello(const Hello& hello, std::vector<std::uint8_t>* payload,
                 std::string* error) {
  if (payload == nullptr || error == nullptr) {
    return false;
  }
  if (!IsValidHello(hello, error)) {
    return false;
  }
  payload->clear();
  payload->reserve(2 + 2 + hello.nonce.size() + hello.public_key.size() + 8 +
                   hello.device_name.size() + hello.model.size() +
                   hello.device_id.size());
  AppendBigEndian16(hello.version, payload);
  payload->push_back(static_cast<std::uint8_t>(hello.role));
  payload->push_back(static_cast<std::uint8_t>(hello.action));
  payload->insert(payload->end(), hello.nonce.begin(), hello.nonce.end());
  payload->insert(payload->end(), hello.public_key.begin(),
                  hello.public_key.end());
  if (!AppendText(hello.device_name, payload, error) ||
      !AppendText(hello.model, payload, error)) {
    return false;
  }
  payload->push_back(hello.platform);
  AppendBigEndian32(hello.capabilities, payload);
  return AppendText(hello.device_id, payload, error);
}

bool DecodeHello(std::span<const std::uint8_t> payload, Hello* hello,
                 std::string* error) {
  constexpr std::size_t kFixedSize = 2 + 2 + 32 + 65;
  if (hello == nullptr || error == nullptr || payload.size() < kFixedSize) {
    if (error != nullptr) {
      *error = "HELLO payload is truncated.";
    }
    return false;
  }

  Hello parsed;
  std::size_t offset = 0;
  parsed.version = ReadBigEndian16(payload.data() + offset);
  offset += 2;
  parsed.role = static_cast<Role>(payload[offset++]);
  parsed.action = static_cast<Action>(payload[offset++]);
  std::copy_n(payload.begin() + offset, parsed.nonce.size(),
              parsed.nonce.begin());
  offset += parsed.nonce.size();
  std::copy_n(payload.begin() + offset, parsed.public_key.size(),
              parsed.public_key.begin());
  offset += parsed.public_key.size();

  if (!ReadText(payload, &offset, &parsed.device_name, error) ||
      !ReadText(payload, &offset, &parsed.model, error)) {
    return false;
  }
  if (offset + 5 > payload.size()) {
    *error = "HELLO platform or capabilities are truncated.";
    return false;
  }
  parsed.platform = payload[offset++];
  parsed.capabilities = ReadBigEndian32(payload.data() + offset);
  offset += 4;
  if (!ReadText(payload, &offset, &parsed.device_id, error) ||
      offset != payload.size() || !IsValidHello(parsed, error)) {
    return false;
  }
  *hello = std::move(parsed);
  return true;
}

bool HashHandshakeTranscript(
    std::span<const std::uint8_t> phone_hello_frame,
    std::span<const std::uint8_t> receiver_hello_frame,
    std::array<std::uint8_t, crypto::kSha256Size>* hash,
    std::wstring* error) {
  if (hash == nullptr || error == nullptr ||
      phone_hello_frame.size() > kMaximumFrameSize ||
      receiver_hello_frame.size() > kMaximumFrameSize ||
      phone_hello_frame.size() + receiver_hello_frame.size() >
          std::numeric_limits<std::size_t>::max() -
              kTranscriptContext.size()) {
    if (error != nullptr) {
      *error = L"Handshake transcript is invalid or exceeds the frame limit.";
    }
    return false;
  }
  std::vector<std::uint8_t> transcript;
  transcript.reserve(kTranscriptContext.size() + phone_hello_frame.size() +
                     receiver_hello_frame.size());
  transcript.insert(transcript.end(), kTranscriptContext.begin(),
                    kTranscriptContext.end());
  transcript.insert(transcript.end(), phone_hello_frame.begin(),
                    phone_hello_frame.end());
  transcript.insert(transcript.end(), receiver_hello_frame.begin(),
                    receiver_hello_frame.end());
  return crypto::Sha256(transcript, hash, error);
}

bool BuildAuthProof(
    std::span<const std::uint8_t> auth_secret, AuthRole role,
    std::span<const std::uint8_t, crypto::kSha256Size> transcript_hash,
    std::array<std::uint8_t, crypto::kSha256Size>* proof,
    std::wstring* error) {
  if (proof == nullptr || error == nullptr ||
      auth_secret.size() != 32 ||
      (role != AuthRole::kPhone && role != AuthRole::kReceiver)) {
    if (error != nullptr) {
      *error = L"AUTH proof requires a 32-byte secret, known role, and output.";
    }
    return false;
  }
  const std::string_view context =
      role == AuthRole::kPhone ? kPhoneProofContext : kReceiverProofContext;
  std::vector<std::uint8_t> message(context.begin(), context.end());
  message.insert(message.end(), transcript_hash.begin(), transcript_hash.end());
  return crypto::HmacSha256(auth_secret, message, proof, error);
}

EphemeralP256KeyPair::~EphemeralP256KeyPair() {
  if (key_ != nullptr) {
    BCryptDestroyKey(key_);
  }
  if (algorithm_ != nullptr) {
    BCryptCloseAlgorithmProvider(algorithm_, 0);
  }
}

bool EphemeralP256KeyPair::Generate(std::wstring* error) {
  if (error == nullptr || key_ != nullptr || algorithm_ != nullptr) {
    return false;
  }
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      &algorithm_, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0);
  if (status < 0) {
    *error = L"Could not open the Windows CNG P-256 provider.";
    return false;
  }
  status = BCryptGenerateKeyPair(algorithm_, &key_, 256, 0);
  if (status < 0) {
    *error = L"Could not generate an ephemeral P-256 key.";
    return false;
  }
  status = BCryptFinalizeKeyPair(key_, 0);
  if (status < 0) {
    BCryptDestroyKey(key_);
    key_ = nullptr;
    *error = L"Could not finalize the ephemeral P-256 key.";
    return false;
  }
  return true;
}

bool EphemeralP256KeyPair::PublicKey(
    std::array<std::uint8_t, 65>* public_key, std::wstring* error) const {
  if (public_key == nullptr || error == nullptr || key_ == nullptr) {
    return false;
  }
  std::array<std::uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + 2 * 32> blob{};
  ULONG blob_size = 0;
  NTSTATUS status = BCryptExportKey(
      key_, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(),
      static_cast<ULONG>(blob.size()), &blob_size, 0);
  if (status < 0 || blob_size != blob.size()) {
    *error = L"Could not export the P-256 public key.";
    return false;
  }
  BCRYPT_ECCKEY_BLOB header{};
  std::memcpy(&header, blob.data(), sizeof(header));
  if (header.dwMagic != BCRYPT_ECDH_PUBLIC_P256_MAGIC ||
      header.cbKey != 32) {
    *error = L"CNG returned an unexpected P-256 public-key format.";
    return false;
  }
  (*public_key)[0] = 0x04;
  std::copy_n(blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB), 64,
              public_key->begin() + 1);
  return true;
}

bool EphemeralP256KeyPair::DeriveSecret(
    std::span<const std::uint8_t, 65> peer_public_key,
    std::array<std::uint8_t, 32>* secret, std::wstring* error) const {
  if (secret == nullptr || error == nullptr || key_ == nullptr ||
      algorithm_ == nullptr || peer_public_key[0] != 0x04) {
    return false;
  }

  std::array<std::uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + 2 * 32> peer_blob{};
  const BCRYPT_ECCKEY_BLOB peer_header{BCRYPT_ECDH_PUBLIC_P256_MAGIC, 32};
  std::memcpy(peer_blob.data(), &peer_header, sizeof(peer_header));
  std::copy_n(peer_public_key.begin() + 1, 64,
              peer_blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB));

  KeyHandle peer;
  NTSTATUS status = BCryptImportKeyPair(
      algorithm_, nullptr, BCRYPT_ECCPUBLIC_BLOB, peer.put(),
      peer_blob.data(), static_cast<ULONG>(peer_blob.size()), 0);
  if (status < 0) {
    *error = L"Peer P-256 public key is invalid.";
    return false;
  }

  SecretHandle shared;
  status = BCryptSecretAgreement(key_, peer.get(), shared.put(), 0);
  if (status < 0) {
    *error = L"P-256 key agreement failed.";
    return false;
  }

  ULONG secret_size = 0;
  status = BCryptDeriveKey(shared.get(), BCRYPT_KDF_RAW_SECRET, nullptr,
                           nullptr, 0, &secret_size, 0);
  if (status < 0 || secret_size != secret->size()) {
    *error = L"CNG returned an unexpected P-256 shared-secret size.";
    return false;
  }
  std::array<std::uint8_t, 32> little_endian_secret{};
  const SecretWiper wipe_secret(&little_endian_secret);
  status = BCryptDeriveKey(
      shared.get(), BCRYPT_KDF_RAW_SECRET, nullptr,
      little_endian_secret.data(), static_cast<ULONG>(little_endian_secret.size()),
      &secret_size, 0);
  if (status < 0) {
    *error = L"Could not derive the P-256 raw shared secret.";
    return false;
  }
  std::reverse_copy(little_endian_secret.begin(), little_endian_secret.end(),
                    secret->begin());
  return true;
}

bool DeriveSessionKeys(
    const EphemeralP256KeyPair& key_pair,
    std::span<const std::uint8_t, 65> peer_public_key,
    std::span<const std::uint8_t> auth_secret,
    std::span<const std::uint8_t, crypto::kSha256Size> transcript_hash,
    SessionKeys* keys, std::wstring* error) {
  if (keys == nullptr || error == nullptr) {
    return false;
  }
  if (auth_secret.size() != 32) {
    *error = L"Session derivation requires a 32-byte authentication secret.";
    return false;
  }
  std::array<std::uint8_t, 32> shared_secret{};
  const SecretWiper wipe_shared_secret(&shared_secret);
  if (!key_pair.DeriveSecret(peer_public_key, &shared_secret, error)) {
    return false;
  }

  return ExpandSessionKeys(shared_secret, auth_secret, transcript_hash, keys,
                           error);
}

bool ExpandSessionKeys(
    std::span<const std::uint8_t, 32> shared_secret,
    std::span<const std::uint8_t> auth_secret,
    std::span<const std::uint8_t, crypto::kSha256Size> transcript_hash,
    SessionKeys* keys, std::wstring* error) {
  if (keys == nullptr || error == nullptr) {
    return false;
  }
  if (auth_secret.size() != 32) {
    *error = L"Session expansion requires a 32-byte authentication secret.";
    return false;
  }
  std::vector<std::uint8_t> info(kSessionContext.begin(),
                                 kSessionContext.end());
  info.insert(info.end(), transcript_hash.begin(), transcript_hash.end());
  std::vector<std::uint8_t> key_material;
  const ByteVectorWiper wipe_key_material(&key_material);
  if (!crypto::HkdfSha256(shared_secret, auth_secret, info, 64,
                          &key_material, error)) {
    return false;
  }
  std::copy_n(key_material.begin(), keys->phone_to_pc.size(),
              keys->phone_to_pc.begin());
  std::copy_n(key_material.begin() + keys->phone_to_pc.size(),
              keys->pc_to_phone.size(), keys->pc_to_phone.begin());
  return true;
}

}  // namespace aicam::protocol
