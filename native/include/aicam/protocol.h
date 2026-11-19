#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace aicam::protocol {

inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kMaximumFrameSize = 16u * 1024u * 1024u;
inline constexpr std::size_t kWirePrefixSize = 4;
inline constexpr std::size_t kEncryptedNonceSize = 12;

enum class MessageType : std::uint8_t {
  kHello = 0x01,
  kAuth = 0x02,
  kConfig = 0x10,
  kVideoFrame = 0x11,
  kKeyframeRequest = 0x12,
  kControl = 0x13,
  kStats = 0x14,
  kPing = 0x15,
  kBye = 0x16,
};

struct Frame final {
  std::uint8_t type = 0;
  std::vector<std::uint8_t> body;
};

bool EncodeFrame(const Frame& frame, std::vector<std::uint8_t>* wire,
                 std::string* error);

std::array<std::uint8_t, kEncryptedNonceSize> BuildNonce(
    std::uint8_t direction, std::uint64_t counter);
std::array<std::uint8_t, 4 + 1 + kEncryptedNonceSize> BuildAssociatedData(
    std::uint32_t frame_length, std::uint8_t type,
    const std::array<std::uint8_t, kEncryptedNonceSize>& nonce);

class FrameParser final {
 public:
  bool Push(std::span<const std::uint8_t> bytes,
            std::vector<Frame>* completed, std::string* error);
  void Reset();
  bool failed() const { return failed_; }

 private:
  std::vector<std::uint8_t> pending_;
  std::uint32_t expected_length_ = 0;
  bool failed_ = false;
};

class ReceiveCounter final {
 public:
  bool Accept(std::uint64_t counter);
  void Reset();

 private:
  std::uint64_t expected_ = 0;
  bool exhausted_ = false;
};

}  // namespace aicam::protocol
