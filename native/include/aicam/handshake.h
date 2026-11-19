#pragma once

#include "aicam/crypto.h"

#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace aicam::protocol {

enum class Role : std::uint8_t { kSender = 1, kReceiver = 2 };
enum class Action : std::uint8_t { kPair = 1, kConnect = 2 };
enum class AuthRole : std::uint8_t { kPhone = 1, kReceiver = 2 };

struct Hello final {
  std::uint16_t version = 1;
  Role role = Role::kSender;
  Action action = Action::kPair;
  std::array<std::uint8_t, 32> nonce{};
  std::array<std::uint8_t, 65> public_key{};
  std::string device_name;
  std::string model;
  std::uint8_t platform = 0;
  std::uint32_t capabilities = 0;
  std::string device_id;
};

struct SessionKeys final {
  std::array<std::uint8_t, 32> phone_to_pc{};
  std::array<std::uint8_t, 32> pc_to_phone{};

  ~SessionKeys();
  SessionKeys() = default;
  SessionKeys(const SessionKeys&) = delete;
  SessionKeys& operator=(const SessionKeys&) = delete;
};

bool EncodeHello(const Hello& hello, std::vector<std::uint8_t>* payload,
                 std::string* error);
bool DecodeHello(std::span<const std::uint8_t> payload, Hello* hello,
                 std::string* error);
bool HashHandshakeTranscript(std::span<const std::uint8_t> phone_hello_frame,
                             std::span<const std::uint8_t> receiver_hello_frame,
                             std::array<std::uint8_t, crypto::kSha256Size>* hash,
                             std::wstring* error);
bool BuildAuthProof(std::span<const std::uint8_t> auth_secret, AuthRole role,
                    std::span<const std::uint8_t, crypto::kSha256Size> transcript_hash,
                    std::array<std::uint8_t, crypto::kSha256Size>* proof,
                    std::wstring* error);

class EphemeralP256KeyPair final {
 public:
  EphemeralP256KeyPair() = default;
  ~EphemeralP256KeyPair();
  EphemeralP256KeyPair(const EphemeralP256KeyPair&) = delete;
  EphemeralP256KeyPair& operator=(const EphemeralP256KeyPair&) = delete;

  bool Generate(std::wstring* error);
  bool PublicKey(std::array<std::uint8_t, 65>* public_key,
                 std::wstring* error) const;
  bool DeriveSecret(std::span<const std::uint8_t, 65> peer_public_key,
                    std::array<std::uint8_t, 32>* secret,
                    std::wstring* error) const;

 private:
  BCRYPT_ALG_HANDLE algorithm_ = nullptr;
  BCRYPT_KEY_HANDLE key_ = nullptr;
};

bool DeriveSessionKeys(
    const EphemeralP256KeyPair& key_pair,
    std::span<const std::uint8_t, 65> peer_public_key,
    std::span<const std::uint8_t> auth_secret,
    std::span<const std::uint8_t, crypto::kSha256Size> transcript_hash,
    SessionKeys* keys, std::wstring* error);
bool ExpandSessionKeys(
    std::span<const std::uint8_t, 32> shared_secret,
    std::span<const std::uint8_t> auth_secret,
    std::span<const std::uint8_t, crypto::kSha256Size> transcript_hash,
    SessionKeys* keys, std::wstring* error);

}  // namespace aicam::protocol
