#include <winsock2.h>
#include <ws2tcpip.h>

#include "aicam/crypto.h"
#include "aicam/handshake.h"
#include "aicam/receiver.h"
#include "aicam/session_cipher.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint8_t kAuthProofStage = 1;
constexpr std::uint8_t kAuthSuccess = 0;
constexpr std::uint16_t kNoAuthError = 0;

bool SetSocketTimeout(SOCKET socket) {
  const DWORD timeout_ms = 3000;
  return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                    reinterpret_cast<const char*>(&timeout_ms),
                    sizeof(timeout_ms)) == 0 &&
         setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                    reinterpret_cast<const char*>(&timeout_ms),
                    sizeof(timeout_ms)) == 0;
}

bool SendAll(SOCKET socket, const std::uint8_t* bytes, std::size_t size) {
  std::size_t offset = 0;
  while (offset < size) {
    const int count = send(
        socket, reinterpret_cast<const char*>(bytes + offset),
        static_cast<int>(size - offset), 0);
    if (count <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

bool ReadExact(SOCKET socket, std::uint8_t* bytes, std::size_t size) {
  std::size_t offset = 0;
  while (offset < size) {
    const int count =
        recv(socket, reinterpret_cast<char*>(bytes + offset),
             static_cast<int>(size - offset), 0);
    if (count <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

std::uint32_t ReadBigEndian32(const std::uint8_t* bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24) |
         (static_cast<std::uint32_t>(bytes[1]) << 16) |
         (static_cast<std::uint32_t>(bytes[2]) << 8) |
         static_cast<std::uint32_t>(bytes[3]);
}

bool SendFrame(SOCKET socket, const aicam::protocol::Frame& frame) {
  std::vector<std::uint8_t> wire;
  std::string error;
  return aicam::protocol::EncodeFrame(frame, &wire, &error) &&
         SendAll(socket, wire.data(), wire.size());
}

bool ReceiveFrame(SOCKET socket, aicam::protocol::Frame* frame,
                  std::vector<std::uint8_t>* raw_wire = nullptr) {
  std::array<std::uint8_t, 4> prefix{};
  if (!ReadExact(socket, prefix.data(), prefix.size())) {
    return false;
  }
  const std::uint32_t body_size = ReadBigEndian32(prefix.data());
  if (body_size == 0 ||
      body_size > aicam::protocol::kMaximumFrameSize -
                      aicam::protocol::kWirePrefixSize) {
    return false;
  }

  std::vector<std::uint8_t> wire(prefix.begin(), prefix.end());
  wire.resize(prefix.size() + body_size);
  if (!ReadExact(socket, wire.data() + prefix.size(), body_size)) {
    return false;
  }
  aicam::protocol::FrameParser parser;
  std::vector<aicam::protocol::Frame> frames;
  std::string error;
  if (!parser.Push(wire, &frames, &error) || frames.size() != 1) {
    return false;
  }
  *frame = std::move(frames.front());
  if (raw_wire != nullptr) {
    *raw_wire = std::move(wire);
  }
  return true;
}

bool MakeHelloFrame(const aicam::protocol::Hello& hello,
                    std::vector<std::uint8_t>* wire) {
  std::vector<std::uint8_t> payload;
  std::string error;
  return aicam::protocol::EncodeHello(hello, &payload, &error) &&
         aicam::protocol::EncodeFrame({0x01, std::move(payload)}, wire,
                                      &error);
}

bool ConstantTimeEqual(std::span<const std::uint8_t> left,
                       std::span<const std::uint8_t> right) {
  if (left.size() != right.size()) {
    return false;
  }
  std::uint8_t difference = 0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    difference |= left[index] ^ right[index];
  }
  return difference == 0;
}

bool MakeAuthProofFrame(
    aicam::protocol::AuthRole role,
    std::span<const std::uint8_t> auth_secret,
    std::span<const std::uint8_t, aicam::crypto::kSha256Size> transcript_hash,
    aicam::protocol::Frame* frame) {
  std::array<std::uint8_t, aicam::crypto::kSha256Size> proof{};
  std::wstring error;
  if (!aicam::protocol::BuildAuthProof(auth_secret, role, transcript_hash,
                                       &proof, &error)) {
    return false;
  }
  frame->type = 0x02;
  frame->body = {kAuthProofStage, kAuthSuccess,
                 static_cast<std::uint8_t>(kNoAuthError >> 8),
                 static_cast<std::uint8_t>(kNoAuthError)};
  frame->body.insert(frame->body.end(), proof.begin(), proof.end());
  SecureZeroMemory(proof.data(), proof.size());
  return true;
}

bool VerifyAuthProof(
    const aicam::protocol::Frame& frame, aicam::protocol::AuthRole role,
    std::span<const std::uint8_t> auth_secret,
    std::span<const std::uint8_t, aicam::crypto::kSha256Size> transcript_hash) {
  if (frame.type != 0x02 || frame.body.size() != 4 + 32 ||
      frame.body[0] != kAuthProofStage || frame.body[1] != kAuthSuccess ||
      frame.body[2] != 0 || frame.body[3] != 0) {
    return false;
  }
  std::array<std::uint8_t, aicam::crypto::kSha256Size> expected{};
  std::wstring error;
  if (!aicam::protocol::BuildAuthProof(auth_secret, role, transcript_hash,
                                       &expected, &error)) {
    return false;
  }
  const bool matches = ConstantTimeEqual(
      expected, std::span<const std::uint8_t>(frame.body).subspan(4));
  SecureZeroMemory(expected.data(), expected.size());
  return matches;
}

bool MakeHello(aicam::protocol::Role role,
               const aicam::protocol::EphemeralP256KeyPair& key_pair,
               aicam::protocol::Hello* hello) {
  if (hello == nullptr) {
    return false;
  }
  hello->role = role;
  hello->action = aicam::protocol::Action::kPair;
  std::wstring error;
  return aicam::crypto::RandomBytes(hello->nonce, &error) &&
         key_pair.PublicKey(&hello->public_key, &error);
}

bool RunReceiverHandshake(SOCKET socket,
                          aicam::protocol::PairingCredential* credential,
                          bool* device_persisted) {
  const auto persist_device =
      [device_persisted](const aicam::protocol::TrustedDevice& device,
                         std::wstring*) {
        *device_persisted = !device.device_id.empty() &&
                            device.device_name == "Local Test Phone" &&
                            device.model == "AiCam Test Sender" &&
                            device.platform == 1;
        return *device_persisted;
      };
  std::wstring error;
  return aicam::protocol::RunPairingSession(socket, credential,
                                             persist_device, &error);
}

bool RunPhoneHandshake(SOCKET socket,
                       std::span<const std::uint8_t> auth_secret) {
  using namespace aicam::protocol;
  EphemeralP256KeyPair phone_key;
  Hello phone;
  std::vector<std::uint8_t> phone_wire;
  std::wstring crypto_error;
  if (!phone_key.Generate(&crypto_error) ||
      !MakeHello(Role::kSender, phone_key, &phone)) {
    return false;
  }
  phone.device_name = "Local Test Phone";
  phone.model = "AiCam Test Sender";
  phone.platform = 1;
  if (!MakeHelloFrame(phone, &phone_wire) ||
      !SendAll(socket, phone_wire.data(), phone_wire.size())) {
    return false;
  }

  Frame receiver_frame;
  std::vector<std::uint8_t> receiver_wire;
  if (!ReceiveFrame(socket, &receiver_frame, &receiver_wire) ||
      receiver_frame.type != 0x01) {
    return false;
  }
  Hello receiver;
  std::string parse_error;
  if (!DecodeHello(receiver_frame.body, &receiver, &parse_error) ||
      receiver.version != kVersion || receiver.role != Role::kReceiver ||
      receiver.action != Action::kPair) {
    return false;
  }

  std::array<std::uint8_t, aicam::crypto::kSha256Size> transcript_hash{};
  if (!HashHandshakeTranscript(phone_wire, receiver_wire, &transcript_hash,
                               &crypto_error)) {
    return false;
  }
  SessionKeys keys;
  if (!DeriveSessionKeys(phone_key, receiver.public_key, auth_secret,
                         transcript_hash, &keys, &crypto_error)) {
    return false;
  }

  Frame proof;
  if (!MakeAuthProofFrame(AuthRole::kPhone, auth_secret, transcript_hash,
                          &proof) ||
      !SendFrame(socket, proof) ||
      !ReceiveFrame(socket, &proof) ||
      !VerifyAuthProof(proof, AuthRole::kReceiver, auth_secret,
                       transcript_hash)) {
    return false;
  }

  SessionCipher session(keys.phone_to_pc, keys.pc_to_phone, 0, 1);
  if (!ReceiveFrame(socket, &proof)) {
    return false;
  }
  std::vector<std::uint8_t> pair_accept;
  if (!session.Decrypt(proof, &pair_accept, &crypto_error) ||
      proof.type != static_cast<std::uint8_t>(MessageType::kAuth) ||
      pair_accept.size() < 4 + 2 + 1 + 32 || pair_accept[0] != 0x02 ||
      pair_accept[1] != 0 || pair_accept[2] != 0 || pair_accept[3] != 0) {
    return false;
  }
  const std::size_t device_id_size =
      (static_cast<std::size_t>(pair_accept[4]) << 8) | pair_accept[5];
  if (device_id_size == 0 || device_id_size > 128 ||
      pair_accept.size() != 4 + 2 + device_id_size + 32) {
    return false;
  }

  const std::vector<std::uint8_t> pair_stored{0x03, 0, 0, 0};
  if (!session.Encrypt(static_cast<std::uint8_t>(MessageType::kAuth),
                       pair_stored, &proof, &crypto_error)) {
    return false;
  }
  return SendFrame(socket, proof);
}

int RunTcpSelfTest() {
  WSADATA winsock_data{};
  if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
    std::cerr << "FAIL: WSAStartup\n";
    return 1;
  }

  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    WSACleanup();
    std::cerr << "FAIL: create listener socket\n";
    return 1;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(listener, reinterpret_cast<const sockaddr*>(&address),
           sizeof(address)) == SOCKET_ERROR ||
      listen(listener, 1) == SOCKET_ERROR) {
    closesocket(listener);
    WSACleanup();
    std::cerr << "FAIL: bind/listen on loopback\n";
    return 1;
  }

  int address_size = sizeof(address);
  if (getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                  &address_size) == SOCKET_ERROR) {
    closesocket(listener);
    WSACleanup();
    std::cerr << "FAIL: query loopback port\n";
    return 1;
  }

  const std::vector<std::uint8_t> auth_secret(32, 0xaa);
  aicam::protocol::PairingCredential credential;
  std::copy(auth_secret.begin(), auth_secret.end(), credential.token.begin());
  credential.expires_at =
      std::chrono::steady_clock::now() + std::chrono::minutes(5);
  bool server_ok = false;
  bool device_persisted = false;
  std::thread server([&] {
    SOCKET peer = accept(listener, nullptr, nullptr);
    if (peer == INVALID_SOCKET) {
      return;
    }
    server_ok = SetSocketTimeout(peer) &&
                RunReceiverHandshake(peer, &credential, &device_persisted);
    shutdown(peer, SD_BOTH);
    closesocket(peer);
  });

  SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  bool client_ok = client != INVALID_SOCKET && SetSocketTimeout(client) &&
                   connect(client, reinterpret_cast<const sockaddr*>(&address),
                           sizeof(address)) != SOCKET_ERROR &&
                   RunPhoneHandshake(client, auth_secret);
  if (client != INVALID_SOCKET) {
    shutdown(client, SD_BOTH);
    closesocket(client);
  }
  server.join();
  closesocket(listener);
  WSACleanup();

  if (!client_ok || !server_ok || !device_persisted || !credential.consumed) {
    std::cerr << "FAIL: authenticated pairing and encrypted credential "
                 "confirmation over loopback TCP\n";
    return 1;
  }
  std::cout << "PASS: receiver authenticated the phone, provisioned and "
               "persisted a trusted device only after encrypted storage "
               "confirmation, and consumed the one-time pairing token. "
               "QR display, LAN discovery, and phone transport remain "
               "unimplemented.\n";
  return 0;
}

int RunUnauthorizedPairingTest() {
  using namespace aicam::protocol;
  WSADATA winsock_data{};
  if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
    std::cerr << "FAIL: WSAStartup for unauthorized-pairing test\n";
    return 1;
  }
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    WSACleanup();
    return 1;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(listener, reinterpret_cast<const sockaddr*>(&address),
           sizeof(address)) == SOCKET_ERROR ||
      listen(listener, 1) == SOCKET_ERROR) {
    closesocket(listener);
    WSACleanup();
    return 1;
  }
  int address_size = sizeof(address);
  if (getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                  &address_size) == SOCKET_ERROR) {
    closesocket(listener);
    WSACleanup();
    return 1;
  }

  PairingCredential credential;
  credential.token.fill(0xaa);
  credential.expires_at =
      std::chrono::steady_clock::now() + std::chrono::minutes(5);
  bool server_ok = false;
  bool device_persisted = false;
  std::thread server([&] {
    SOCKET peer = accept(listener, nullptr, nullptr);
    if (peer == INVALID_SOCKET) {
      return;
    }
    const PersistTrustedDevice persist_device =
        [&device_persisted](const TrustedDevice&, std::wstring*) {
          device_persisted = true;
          return true;
        };
    std::wstring error;
    server_ok =
        RunPairingSession(peer, &credential, persist_device, &error);
    shutdown(peer, SD_BOTH);
    closesocket(peer);
  });

  SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  std::vector<std::uint8_t> wrong_secret(32, 0xbb);
  const bool connected =
      client != INVALID_SOCKET && SetSocketTimeout(client) &&
      connect(client, reinterpret_cast<const sockaddr*>(&address),
              sizeof(address)) != SOCKET_ERROR;
  const bool client_ok =
      connected && RunPhoneHandshake(client, wrong_secret);
  if (client != INVALID_SOCKET) {
    shutdown(client, SD_BOTH);
    closesocket(client);
  }
  server.join();
  closesocket(listener);
  WSACleanup();

  if (client_ok || server_ok || device_persisted || credential.consumed) {
    std::cerr << "FAIL: invalid pairing token was accepted or committed\n";
    return 1;
  }
  std::cout << "PASS: invalid pairing token was rejected before device "
               "persistence and token consumption.\n";
  return 0;
}

}  // namespace

int main() {
  if (RunTcpSelfTest() != 0) {
    return 1;
  }
  return RunUnauthorizedPairingTest();
}
