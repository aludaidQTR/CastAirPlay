#include "aicam/session_cipher.h"

#include "aicam/crypto.h"

#include <algorithm>
#include <limits>

namespace aicam::protocol {
namespace {

std::uint64_t ReadBigEndian64(const std::uint8_t* input) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value = (value << 8) | input[index];
  }
  return value;
}

bool IsSupportedDirection(std::uint8_t direction) {
  return direction == 0 || direction == 1;
}

}  // namespace

SessionCipher::SessionCipher(std::span<const std::uint8_t, 32> send_key,
                             std::span<const std::uint8_t, 32> receive_key,
                             std::uint8_t send_direction,
                             std::uint8_t receive_direction)
    : send_direction_(send_direction),
      receive_direction_(receive_direction) {
  std::copy(send_key.begin(), send_key.end(), send_key_.begin());
  std::copy(receive_key.begin(), receive_key.end(), receive_key_.begin());
}

SessionCipher::~SessionCipher() {
  SecureZeroMemory(send_key_.data(), send_key_.size());
  SecureZeroMemory(receive_key_.data(), receive_key_.size());
}

bool SessionCipher::Encrypt(std::uint8_t type,
                            std::span<const std::uint8_t> plaintext,
                            Frame* frame, std::wstring* error) {
  if (frame == nullptr || error == nullptr ||
      !IsSupportedDirection(send_direction_) || send_exhausted_) {
    if (error != nullptr) {
      *error = L"Encrypted frame state or direction is invalid.";
    }
    return false;
  }

  constexpr std::size_t overhead =
      kEncryptedNonceSize + crypto::kGcmTagSize + 1;
  if (plaintext.size() > kMaximumFrameSize - kWirePrefixSize - overhead) {
    *error = L"Encrypted frame exceeds the 16 MiB wire limit.";
    return false;
  }
  const std::uint32_t frame_length =
      static_cast<std::uint32_t>(overhead + plaintext.size());
  const auto nonce = BuildNonce(send_direction_, send_counter_);
  const auto associated_data = BuildAssociatedData(frame_length, type, nonce);
  crypto::GcmCiphertext encrypted;
  if (!crypto::Aes256GcmEncrypt(send_key_, nonce, associated_data, plaintext,
                                &encrypted, error)) {
    return false;
  }

  frame->type = type;
  frame->body.clear();
  frame->body.reserve(nonce.size() + encrypted.bytes.size() +
                      encrypted.tag.size());
  frame->body.insert(frame->body.end(), nonce.begin(), nonce.end());
  frame->body.insert(frame->body.end(), encrypted.bytes.begin(),
                     encrypted.bytes.end());
  frame->body.insert(frame->body.end(), encrypted.tag.begin(),
                     encrypted.tag.end());

  if (send_counter_ == std::numeric_limits<std::uint64_t>::max()) {
    send_exhausted_ = true;
  } else {
    ++send_counter_;
  }
  return true;
}

bool SessionCipher::Decrypt(const Frame& frame,
                            std::vector<std::uint8_t>* plaintext,
                            std::wstring* error) {
  constexpr std::size_t minimum_body =
      kEncryptedNonceSize + crypto::kGcmTagSize;
  if (plaintext == nullptr || error == nullptr ||
      !IsSupportedDirection(receive_direction_) ||
      frame.body.size() < minimum_body ||
      frame.body.size() >
          kMaximumFrameSize - kWirePrefixSize - 1) {
    if (error != nullptr) {
      *error = L"Encrypted frame size or session state is invalid.";
    }
    return false;
  }

  std::array<std::uint8_t, kEncryptedNonceSize> nonce{};
  std::copy_n(frame.body.begin(), nonce.size(), nonce.begin());
  if (nonce[0] != 'A' || nonce[1] != 'I' || nonce[2] != 'C' ||
      nonce[3] != receive_direction_) {
    *error = L"Encrypted frame nonce marker or direction is invalid.";
    return false;
  }

  const std::uint64_t counter = ReadBigEndian64(nonce.data() + 4);
  const std::uint32_t frame_length =
      static_cast<std::uint32_t>(frame.body.size() + 1);
  const auto associated_data =
      BuildAssociatedData(frame_length, frame.type, nonce);

  const std::size_t encrypted_size =
      frame.body.size() - minimum_body;
  const auto ciphertext = std::span<const std::uint8_t>(
      frame.body.data() + kEncryptedNonceSize, encrypted_size);
  const auto tag = std::span<const std::uint8_t, crypto::kGcmTagSize>(
      frame.body.data() + kEncryptedNonceSize + encrypted_size,
      crypto::kGcmTagSize);

  if (!crypto::Aes256GcmDecrypt(receive_key_, nonce, associated_data,
                                ciphertext, tag, plaintext, error)) {
    return false;
  }
  if (!receive_counter_.Accept(counter)) {
    plaintext->clear();
    *error = L"Encrypted frame counter is replayed or out of sequence.";
    return false;
  }
  return true;
}

}  // namespace aicam::protocol
