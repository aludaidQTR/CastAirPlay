#pragma once

#include <winsock2.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace aicam::protocol {

struct PairingCredential final {
  std::array<std::uint8_t, 32> token{};
  std::chrono::steady_clock::time_point expires_at{};
  bool consumed = false;

  PairingCredential() = default;
  ~PairingCredential();
  PairingCredential(const PairingCredential&) = delete;
  PairingCredential& operator=(const PairingCredential&) = delete;
};

struct TrustedDevice final {
  std::string device_id;
  std::string device_name;
  std::string model;
  std::uint8_t platform = 0;
  std::array<std::uint8_t, 32> secret{};
};

using PersistTrustedDevice =
    std::function<bool(const TrustedDevice&, std::wstring*)>;

// Runs one bounded PAIR session on an accepted TCP socket. The caller must
// serialize sessions for a credential and provide a durable user-scope store.
bool RunPairingSession(SOCKET socket, PairingCredential* credential,
                       const PersistTrustedDevice& persist_device,
                       std::wstring* error);

}  // namespace aicam::protocol
