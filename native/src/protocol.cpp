#include "aicam/protocol.h"

#include <algorithm>
#include <limits>

namespace aicam::protocol {
namespace {

void WriteBigEndian32(std::uint32_t value, std::uint8_t* output) {
  output[0] = static_cast<std::uint8_t>(value >> 24);
  output[1] = static_cast<std::uint8_t>(value >> 16);
  output[2] = static_cast<std::uint8_t>(value >> 8);
  output[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t ReadBigEndian32(const std::uint8_t* input) {
  return (static_cast<std::uint32_t>(input[0]) << 24) |
         (static_cast<std::uint32_t>(input[1]) << 16) |
         (static_cast<std::uint32_t>(input[2]) << 8) |
         static_cast<std::uint32_t>(input[3]);
}

}  // namespace

bool EncodeFrame(const Frame& frame, std::vector<std::uint8_t>* wire,
                 std::string* error) {
  if (wire == nullptr || error == nullptr) {
    return false;
  }
  if (frame.body.size() >
      static_cast<std::size_t>(kMaximumFrameSize - kWirePrefixSize - 1)) {
    *error = "Frame exceeds the 16 MiB wire limit.";
    return false;
  }

  const auto length =
      static_cast<std::uint32_t>(frame.body.size() + sizeof(frame.type));
  wire->resize(kWirePrefixSize + length);
  WriteBigEndian32(length, wire->data());
  (*wire)[kWirePrefixSize] = frame.type;
  std::copy(frame.body.begin(), frame.body.end(),
            wire->begin() + kWirePrefixSize + sizeof(frame.type));
  return true;
}

std::array<std::uint8_t, kEncryptedNonceSize> BuildNonce(
    std::uint8_t direction, std::uint64_t counter) {
  std::array<std::uint8_t, kEncryptedNonceSize> nonce{};
  nonce[0] = 'A';
  nonce[1] = 'I';
  nonce[2] = 'C';
  nonce[3] = direction;
  for (int index = 0; index < 8; ++index) {
    nonce[4 + index] =
        static_cast<std::uint8_t>(counter >> ((7 - index) * 8));
  }
  return nonce;
}

std::array<std::uint8_t, 4 + 1 + kEncryptedNonceSize> BuildAssociatedData(
    std::uint32_t frame_length, std::uint8_t type,
    const std::array<std::uint8_t, kEncryptedNonceSize>& nonce) {
  std::array<std::uint8_t, 4 + 1 + kEncryptedNonceSize> result{};
  WriteBigEndian32(frame_length, result.data());
  result[4] = type;
  std::copy(nonce.begin(), nonce.end(), result.begin() + 5);
  return result;
}

bool FrameParser::Push(std::span<const std::uint8_t> bytes,
                       std::vector<Frame>* completed, std::string* error) {
  if (completed == nullptr || error == nullptr || failed_) {
    return false;
  }

  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t target_size =
        expected_length_ == 0 ? kWirePrefixSize
                              : kWirePrefixSize + expected_length_;
    const std::size_t needed = target_size - pending_.size();
    const std::size_t available = bytes.size() - offset;
    const std::size_t count = std::min(needed, available);
    pending_.insert(pending_.end(), bytes.begin() + offset,
                    bytes.begin() + offset + count);
    offset += count;

    if (expected_length_ == 0 && pending_.size() == kWirePrefixSize) {
      expected_length_ = ReadBigEndian32(pending_.data());
      if (expected_length_ == 0 ||
          expected_length_ > kMaximumFrameSize - kWirePrefixSize) {
        *error = "Frame length is zero or exceeds the 16 MiB wire limit.";
        failed_ = true;
        return false;
      }
    }

    if (expected_length_ != 0 &&
        pending_.size() == kWirePrefixSize + expected_length_) {
      Frame frame;
      frame.type = pending_[kWirePrefixSize];
      frame.body.assign(pending_.begin() + kWirePrefixSize + 1,
                        pending_.end());
      completed->push_back(std::move(frame));
      pending_.clear();
      expected_length_ = 0;
    }
  }
  return true;
}

void FrameParser::Reset() {
  pending_.clear();
  expected_length_ = 0;
  failed_ = false;
}

bool ReceiveCounter::Accept(std::uint64_t counter) {
  if (exhausted_ || counter != expected_) {
    return false;
  }
  if (expected_ == std::numeric_limits<std::uint64_t>::max()) {
    exhausted_ = true;
  } else {
    ++expected_;
  }
  return true;
}

void ReceiveCounter::Reset() {
  expected_ = 0;
  exhausted_ = false;
}

}  // namespace aicam::protocol
