#include <winsock2.h>
#include <ws2tcpip.h>

#include "aicam/crypto.h"
#include "aicam/handshake.h"
#include "aicam/session_cipher.h"

#include <array>
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
                          std::span<const std::uint8_t> auth_secret) {
  using namespace aicam::protocol;
  Frame phone_frame;
  std::vector<std::uint8_t> phone_wire;
  if (!ReceiveFrame(socket, &phone_frame, &phone_wire) ||
      phone_frame.type != 0x01) {
    return false;
  }
  Hello phone;
  std::string parse_error;
  if (!DecodeHello(phone_frame.body, &phone, &parse_error) ||
      phone.version != kVersion || phone.role != Role::kSender ||
      phone.action != Action::kPair) {
    return false;
  }

  EphemeralP256KeyPair receiver_key;
  Hello receiver;
  receiver.role = Role::kReceiver;
  receiver.device_name = "Local Test PC";
  receiver.model = "AiCam Test Receiver";
  receiver.platform = 3;
  std::vector<std::uint8_t> receiver_wire;
  std::wstring crypto_error;
  if (!receiver_key.Generate(&crypto_error) ||
      !MakeHello(Role::kReceiver, receiver_key, &receiver) ||
      !MakeHelloFrame(receiver, &receiver_wire) ||
      !SendAll(socket, receiver_wire.data(), receiver_wire.size())) {
    return false;
  }

  std::array<std::uint8_t, aicam::crypto::kSha256Size> transcript_hash{};
  if (!HashHandshakeTranscript(phone_wire, receiver_wire, &transcript_hash,
                               &crypto_error)) {
    return false;
  }
  SessionKeys keys;
  if (!DeriveSessionKeys(receiver_key, phone.public_key, auth_secret,
                         transcript_hash, &keys, &crypto_error)) {
    return false;
  }

  Frame proof;
  if (!ReceiveFrame(socket, &proof) ||
      !VerifyAuthProof(proof, AuthRole::kPhone, auth_secret, transcript_hash) ||
      !MakeAuthProofFrame(AuthRole::kReceiver, auth_secret, transcript_hash,
                          &proof) ||
      !SendFrame(socket, proof)) {
    return false;
  }

  SessionCipher session(keys.pc_to_phone, keys.phone_to_pc, 1, 0);
  Frame encrypted;
  std::vector<std::uint8_t> plaintext;
  if (!ReceiveFrame(socket, &encrypted) ||
      !session.Decrypt(encrypted, &plaintext, &crypto_error) ||
      encrypted.type != 0x15 ||
      plaintext != std::vector<std::uint8_t>{'p', 'i', 'n', 'g'}) {
    return false;
  }
  const std::vector<std::uint8_t> response{'p', 'o', 'n', 'g'};
  return session.Encrypt(0x15, response, &encrypted, &crypto_error) &&
         SendFrame(socket, encrypted);
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
  const std::vector<std::uint8_t> request{'p', 'i', 'n', 'g'};
  if (!session.Encrypt(0x15, request, &proof, &crypto_error) ||
      !SendFrame(socket, proof) || !ReceiveFrame(socket, &proof)) {
    return false;
  }
  std::vector<std::uint8_t> response;
  return session.Decrypt(proof, &response, &crypto_error) &&
         proof.type == 0x15 &&
         response == std::vector<std::uint8_t>{'p', 'o', 'n', 'g'};
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
  bool server_ok = false;
  std::thread server([&] {
    SOCKET peer = accept(listener, nullptr, nullptr);
    if (peer == INVALID_SOCKET) {
      return;
    }
    server_ok = SetSocketTimeout(peer) &&
                RunReceiverHandshake(peer, auth_secret);
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

  if (!client_ok || !server_ok) {
    std::cerr << "FAIL: authenticated handshake and encrypted loopback "
                 "PING/PONG\n";
    return 1;
  }
  std::cout << "PASS: P-256 ECDH, transcript-bound role proofs, HKDF session "
               "keys, and encrypted PING/PONG over loopback TCP. This "
               "self-test does not implement QR pairing, LAN discovery, or "
               "phone transport.\n";
  return 0;
}

}  // namespace

int main() { return RunTcpSelfTest(); }
