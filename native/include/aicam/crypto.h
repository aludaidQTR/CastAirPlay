#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace aicam::crypto {

inline constexpr std::size_t kSha256Size = 32;
inline constexpr std::size_t kAes256KeySize = 32;
inline constexpr std::size_t kGcmNonceSize = 12;
inline constexpr std::size_t kGcmTagSize = 16;

bool RandomBytes(std::span<std::uint8_t> output, std::wstring* error);

struct GcmCiphertext final {
  std::vector<std::uint8_t> bytes;
  std::array<std::uint8_t, kGcmTagSize> tag{};
};

bool Sha256(std::span<const std::uint8_t> input,
            std::array<std::uint8_t, kSha256Size>* digest,
            std::wstring* error);
bool HmacSha256(std::span<const std::uint8_t> key,
                std::span<const std::uint8_t> input,
                std::array<std::uint8_t, kSha256Size>* digest,
                std::wstring* error);
bool HkdfSha256(std::span<const std::uint8_t> input_key_material,
                std::span<const std::uint8_t> salt,
                std::span<const std::uint8_t> info, std::size_t output_size,
                std::vector<std::uint8_t>* output, std::wstring* error);
bool Aes256GcmEncrypt(
    std::span<const std::uint8_t, kAes256KeySize> key,
    std::span<const std::uint8_t, kGcmNonceSize> nonce,
    std::span<const std::uint8_t> associated_data,
    std::span<const std::uint8_t> plaintext, GcmCiphertext* ciphertext,
    std::wstring* error);
bool Aes256GcmDecrypt(
    std::span<const std::uint8_t, kAes256KeySize> key,
    std::span<const std::uint8_t, kGcmNonceSize> nonce,
    std::span<const std::uint8_t> associated_data,
    std::span<const std::uint8_t> ciphertext,
    std::span<const std::uint8_t, kGcmTagSize> tag,
    std::vector<std::uint8_t>* plaintext, std::wstring* error);

}  // namespace aicam::crypto
