#include "aicam/receiver.h"

#include "aicam/crypto.h"
#include "aicam/handshake.h"
#include "aicam/protocol.h"
#include "aicam/session_cipher.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <span>
#include <vector>

namespace aicam::protocol {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kHandshakeTimeout = std::chrono::seconds(5);
constexpr std::uint8_t kAuthProofStage = 0x01;
constexpr std::uint8_t kPairAcceptStage = 0x02;
constexpr std::uint8_t kPairStoredStage = 0x03;

class SecretWiper final {
 public:
  explicit SecretWiper(std::array<std::uint8_t, 32>* value) : value_(value) {}
  ~SecretWiper() { SecureZeroMemory(value_->data(), value_->size()); }

 private:
  std::array<std::uint8_t, 32>* value_;
};

bool WaitForSocket(SOCKET socket, bool writing, Clock::time_point deadline) {
  const auto remaining = deadline - Clock::now();
  if (remaining <= Clock::duration::zero()) {
    return false;
  }
  const auto micros =
      std::chrono::duration_cast<std::chrono::microseconds>(remaining);
  timeval timeout{};
  timeout.tv_sec = static_cast<long>(micros.count() / 1'000'000);
  timeout.tv_usec = static_cast<long>(micros.count() % 1'000'000);
  fd_set sockets;
  FD_ZERO(&sockets);
  FD_SET(socket, &sockets);
  return select(0, writing ? nullptr : &sockets, writing ? &sockets : nullptr,
                nullptr, &timeout) > 0;
}

bool SendAll(SOCKET socket, std::span<const std::uint8_t> bytes,
             Clock::time_point deadline) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (!WaitForSocket(socket, true, deadline)) {
      return false;
    }
    const int count = send(
        socket, reinterpret_cast<const char*>(bytes.data() + offset),
        static_cast<int>(bytes.size() - offset), 0);
    if (count <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

bool ReadExact(SOCKET socket, std::span<std::uint8_t> bytes,
               Clock::time_point deadline) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (!WaitForSocket(socket, false, deadline)) {
      return false;
    }
    const int count =
        recv(socket, reinterpret_cast<char*>(bytes.data() + offset),
             static_cast<int>(bytes.size() - offset), 0);
    if (count <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

std::uint32_t ReadBigEndian32(const std::uint8_t* input) {
  return (static_cast<std::uint32_t>(input[0]) << 24) |
         (static_cast<std::uint32_t>(input[1]) << 16) |
         (static_cast<std::uint32_t>(input[2]) << 8) |
         static_cast<std::uint32_t>(input[3]);
}

bool ReceiveFrame(SOCKET socket, Frame* frame,
                  std::vector<std::uint8_t>* raw_wire,
                  Clock::time_point deadline) {
  std::array<std::uint8_t, kWirePrefixSize> prefix{};
  if (!ReadExact(socket, prefix, deadline)) {
    return false;
  }
  const std::uint32_t body_size = ReadBigEndian32(prefix.data());
  if (body_size == 0 || body_size > kMaximumFrameSize - kWirePrefixSize) {
    return false;
  }
  std::vector<std::uint8_t> wire(prefix.begin(), prefix.end());
  wire.resize(prefix.size() + body_size);
  if (!ReadExact(socket,
                 std::span<std::uint8_t>(wire).subspan(prefix.size()),
                 deadline)) {
    return false;
  }

  FrameParser parser;
  std::vector<Frame> frames;
  std::string parse_error;
  if (!parser.Push(wire, &frames, &parse_error) || frames.size() != 1) {
    return false;
  }
  *frame = std::move(frames.front());
  if (raw_wire != nullptr) {
    *raw_wire = std::move(wire);
  }
  return true;
}

bool SendFrame(SOCKET socket, const Frame& frame,
               Clock::time_point deadline) {
  std::vector<std::uint8_t> wire;
  std::string encode_error;
  return EncodeFrame(frame, &wire, &encode_error) &&
         SendAll(socket, wire, deadline);
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

bool VerifyAuthProof(
    const Frame& frame, AuthRole role,
    std::span<const std::uint8_t> auth_secret,
    std::span<const std::uint8_t, crypto::kSha256Size> transcript_hash) {
  if (frame.type != static_cast<std::uint8_t>(MessageType::kAuth) ||
      frame.body.size() != 4 + crypto::kSha256Size ||
      frame.body[0] != kAuthProofStage || frame.body[1] != 0 ||
      frame.body[2] != 0 || frame.body[3] != 0) {
    return false;
  }
  std::array<std::uint8_t, crypto::kSha256Size> expected{};
  std::wstring error;
  if (!BuildAuthProof(auth_secret, role, transcript_hash, &expected, &error)) {
    return false;
  }
  const bool matches = ConstantTimeEqual(
      expected, std::span<const std::uint8_t>(frame.body).subspan(4));
  SecureZeroMemory(expected.data(), expected.size());
  return matches;
}

bool MakeAuthProofFrame(
    std::span<const std::uint8_t> auth_secret,
    std::span<const std::uint8_t, crypto::kSha256Size> transcript_hash,
    Frame* frame) {
  std::array<std::uint8_t, crypto::kSha256Size> proof{};
  std::wstring error;
  if (!BuildAuthProof(auth_secret, AuthRole::kReceiver, transcript_hash,
                      &proof, &error)) {
    return false;
  }
  frame->type = static_cast<std::uint8_t>(MessageType::kAuth);
  frame->body = {kAuthProofStage, 0, 0, 0};
  frame->body.insert(frame->body.end(), proof.begin(), proof.end());
  SecureZeroMemory(proof.data(), proof.size());
  return true;
}

bool MakeDeviceId(std::string* device_id) {
  std::array<std::uint8_t, 16> random{};
  std::wstring error;
  if (!crypto::RandomBytes(random, &error)) {
    return false;
  }
  random[6] = static_cast<std::uint8_t>((random[6] & 0x0f) | 0x40);
  random[8] = static_cast<std::uint8_t>((random[8] & 0x3f) | 0x80);
  std::ostringstream text;
  text << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < random.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      text << '-';
    }
    text << std::setw(2) << static_cast<unsigned int>(random[index]);
  }
  *device_id = text.str();
  SecureZeroMemory(random.data(), random.size());
  return true;
}

void AppendBigEndian16(std::uint16_t value, std::vector<std::uint8_t>* output) {
  output->push_back(static_cast<std::uint8_t>(value >> 8));
  output->push_back(static_cast<std::uint8_t>(value));
}

bool ParsePairStored(const std::vector<std::uint8_t>& payload) {
  return payload.size() == 4 && payload[0] == kPairStoredStage &&
         payload[1] == 0 && payload[2] == 0 && payload[3] == 0;
}

void SendIncompatibleVersion(SOCKET socket, Clock::time_point deadline) {
  const Frame rejection{static_cast<std::uint8_t>(MessageType::kAuth),
                        {0x7f, 1, 0, 1}};
  SendFrame(socket, rejection, deadline);
}

}  // namespace

PairingCredential::~PairingCredential() {
  SecureZeroMemory(token.data(), token.size());
}

bool RunPairingSession(SOCKET socket, PairingCredential* credential,
                       const PersistTrustedDevice& persist_device,
                       std::wstring* error) {
  if (socket == INVALID_SOCKET || credential == nullptr ||
      !persist_device || error == nullptr) {
    if (error != nullptr) {
      *error = L"Pairing session configuration is invalid.";
    }
    return false;
  }
  if (credential->consumed || Clock::now() >= credential->expires_at) {
    *error = L"Pairing token is expired or already used.";
    return false;
  }
  const auto deadline = Clock::now() + kHandshakeTimeout;

  Frame phone_frame;
  std::vector<std::uint8_t> phone_wire;
  if (!ReceiveFrame(socket, &phone_frame, &phone_wire, deadline) ||
      phone_frame.type != static_cast<std::uint8_t>(MessageType::kHello)) {
    *error = L"Phone HELLO was invalid or timed out.";
    return false;
  }
  Hello phone;
  std::string parse_error;
  if (!DecodeHello(phone_frame.body, &phone, &parse_error)) {
    *error = L"Phone HELLO is incompatible with pairing.";
    return false;
  }
  if (phone.version != kVersion) {
    SendIncompatibleVersion(socket, deadline);
    *error = L"Phone protocol version is incompatible.";
    return false;
  }
  if (phone.role != Role::kSender || phone.action != Action::kPair) {
    *error = L"Phone HELLO is incompatible with pairing.";
    return false;
  }

  EphemeralP256KeyPair receiver_key;
  Hello receiver;
  receiver.role = Role::kReceiver;
  receiver.device_name = "AiCam";
  receiver.model = "Windows";
  receiver.platform = 3;
  std::wstring crypto_error;
  if (!receiver_key.Generate(&crypto_error) ||
      !crypto::RandomBytes(receiver.nonce, &crypto_error) ||
      !receiver_key.PublicKey(&receiver.public_key, &crypto_error)) {
    *error = L"Could not initialize receiver cryptography.";
    return false;
  }
  std::vector<std::uint8_t> receiver_payload;
  std::vector<std::uint8_t> receiver_wire;
  std::string hello_error;
  if (!EncodeHello(receiver, &receiver_payload, &hello_error) ||
      !EncodeFrame({static_cast<std::uint8_t>(MessageType::kHello),
                    std::move(receiver_payload)},
                   &receiver_wire, &hello_error) ||
      !SendAll(socket, receiver_wire, deadline)) {
    *error = L"Could not send receiver HELLO.";
    return false;
  }

  std::array<std::uint8_t, crypto::kSha256Size> transcript_hash{};
  if (!HashHandshakeTranscript(phone_wire, receiver_wire, &transcript_hash,
                               &crypto_error)) {
    *error = L"Could not hash the handshake transcript.";
    return false;
  }
  SessionKeys keys;
  if (!DeriveSessionKeys(receiver_key, phone.public_key, credential->token,
                         transcript_hash, &keys, &crypto_error)) {
    *error = L"Phone key agreement failed.";
    return false;
  }

  Frame proof;
  if (!ReceiveFrame(socket, &proof, nullptr, deadline) ||
      !VerifyAuthProof(proof, AuthRole::kPhone, credential->token,
                       transcript_hash) ||
      !MakeAuthProofFrame(credential->token, transcript_hash, &proof) ||
      !SendFrame(socket, proof, deadline)) {
    *error = L"Phone authentication failed or timed out.";
    return false;
  }

  TrustedDevice device;
  const SecretWiper wipe_device_secret(&device.secret);
  if (!MakeDeviceId(&device.device_id) ||
      !crypto::RandomBytes(device.secret, &crypto_error)) {
    *error = L"Could not provision trusted-device credentials.";
    return false;
  }
  device.device_name = phone.device_name;
  device.model = phone.model;
  device.platform = phone.platform;

  SessionCipher session(keys.pc_to_phone, keys.phone_to_pc, 1, 0);
  std::vector<std::uint8_t> accept_payload{kPairAcceptStage, 0, 0, 0};
  AppendBigEndian16(static_cast<std::uint16_t>(device.device_id.size()),
                    &accept_payload);
  accept_payload.insert(accept_payload.end(), device.device_id.begin(),
                        device.device_id.end());
  accept_payload.insert(accept_payload.end(), device.secret.begin(),
                        device.secret.end());
  Frame encrypted;
  if (!session.Encrypt(static_cast<std::uint8_t>(MessageType::kAuth),
                       accept_payload, &encrypted, &crypto_error) ||
      !SendFrame(socket, encrypted, deadline)) {
    *error = L"Could not send pairing credentials to the phone.";
    return false;
  }

  Frame stored_frame;
  std::vector<std::uint8_t> stored_payload;
  if (!ReceiveFrame(socket, &stored_frame, nullptr, deadline) ||
      stored_frame.type != static_cast<std::uint8_t>(MessageType::kAuth) ||
      !session.Decrypt(stored_frame, &stored_payload, &crypto_error) ||
      !ParsePairStored(stored_payload)) {
    *error = L"Phone did not confirm secure credential storage.";
    return false;
  }
  if (credential->consumed || Clock::now() >= credential->expires_at) {
    *error = L"Pairing token expired before pairing could be committed.";
    return false;
  }

  std::wstring storage_error;
  if (!persist_device(device, &storage_error)) {
    *error = L"Could not persist the trusted device; pairing token remains usable.";
    return false;
  }
  credential->consumed = true;
  return true;
}

}  // namespace aicam::protocol
