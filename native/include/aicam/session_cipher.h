#pragma once

#include "aicam/protocol.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace aicam::protocol {

class SessionCipher final {
 public:
  SessionCipher(std::span<const std::uint8_t, 32> send_key,
                std::span<const std::uint8_t, 32> receive_key,
                std::uint8_t send_direction,
                std::uint8_t receive_direction);
  ~SessionCipher();
  SessionCipher(const SessionCipher&) = delete;
  SessionCipher& operator=(const SessionCipher&) = delete;

  bool Encrypt(std::uint8_t type, std::span<const std::uint8_t> plaintext,
               Frame* frame, std::wstring* error);
  bool Decrypt(const Frame& frame, std::vector<std::uint8_t>* plaintext,
               std::wstring* error);

 private:
  std::array<std::uint8_t, 32> send_key_{};
  std::array<std::uint8_t, 32> receive_key_{};
  std::uint8_t send_direction_ = 0;
  std::uint8_t receive_direction_ = 0;
  std::uint64_t send_counter_ = 0;
  bool send_exhausted_ = false;
  ReceiveCounter receive_counter_;
};

}  // namespace aicam::protocol
