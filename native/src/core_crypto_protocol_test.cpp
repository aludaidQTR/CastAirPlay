#include "aicam/crypto.h"
#include "aicam/handshake.h"
#include "aicam/protocol.h"
#include "aicam/session_cipher.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool Fail(const char* message, const std::wstring* detail = nullptr) {
  std::cerr << "FAIL: " << message;
  if (detail != nullptr && !detail->empty()) {
    std::wcerr << L" " << *detail;
  }
  std::cerr << '\n';
  return false;
}

std::vector<std::uint8_t> FromHex(const std::string& value) {
  if ((value.size() & 1u) != 0) {
    return {};
  }
  std::vector<std::uint8_t> output;
  output.reserve(value.size() / 2);
  for (std::size_t index = 0; index < value.size(); index += 2) {
    output.push_back(static_cast<std::uint8_t>(
        std::stoul(value.substr(index, 2), nullptr, 16)));
  }
  return output;
}

std::string ToHex(std::span<const std::uint8_t> value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string output;
  output.reserve(value.size() * 2);
  for (const std::uint8_t byte : value) {
    output.push_back(kDigits[byte >> 4]);
    output.push_back(kDigits[byte & 0x0f]);
  }
  return output;
}

bool ReadTestVector(std::string_view key, std::string* value) {
  static const std::string json = [] {
    std::ifstream input("test_vectors/aicam_protocol_v1.json",
                        std::ios::binary);
    if (!input) {
      return std::string();
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
  }();
  if (value == nullptr || json.empty()) {
    return false;
  }
  const std::string marker = "\"" + std::string(key) + "\": \"";
  const std::size_t start = json.find(marker);
  if (start == std::string::npos) {
    return false;
  }
  const std::size_t value_start = start + marker.size();
  const std::size_t end = json.find('"', value_start);
  if (end == std::string::npos) {
    return false;
  }
  *value = json.substr(value_start, end - value_start);
  return true;
}

bool CheckCryptoVectors() {
  std::wstring error;
  const auto abc = FromHex("616263");
  std::array<std::uint8_t, aicam::crypto::kSha256Size> digest{};
  if (!aicam::crypto::Sha256(abc, &digest, &error) ||
      ToHex(digest) !=
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
    return Fail("SHA-256 known-answer vector mismatch.", &error);
  }

  const std::vector<std::uint8_t> hmac_key(20, 0x0b);
  const auto hmac_data = FromHex("4869205468657265");
  if (!aicam::crypto::HmacSha256(hmac_key, hmac_data, &digest, &error) ||
      ToHex(digest) !=
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7") {
    return Fail("HMAC-SHA-256 known-answer vector mismatch.", &error);
  }

  const std::vector<std::uint8_t> ikm(22, 0x0b);
  const auto salt = FromHex("000102030405060708090a0b0c");
  const auto info = FromHex("f0f1f2f3f4f5f6f7f8f9");
  std::vector<std::uint8_t> okm;
  if (!aicam::crypto::HkdfSha256(ikm, salt, info, 42, &okm, &error)) {
    return Fail("HKDF vector execution failed.", &error);
  }
  if (ToHex(okm) !=
      "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5b"
      "f34007208d5b887185865") {
    return Fail("HKDF RFC 5869 test vector mismatch.");
  }

  std::array<std::uint8_t, 32> key{};
  std::array<std::uint8_t, 12> nonce{};
  const std::vector<std::uint8_t> plaintext(16, 0);
  aicam::crypto::GcmCiphertext encrypted;
  if (!aicam::crypto::Aes256GcmEncrypt(key, nonce, {}, plaintext, &encrypted,
                                       &error)) {
    return Fail("AES-256-GCM encryption failed.", &error);
  }
  if (ToHex(encrypted.bytes) != "cea7403d4d606b6e074ec5d3baf39d18" ||
      ToHex(encrypted.tag) != "d0d1c8a799996bf0265b98b5d48ab919") {
    return Fail("AES-256-GCM NIST vector mismatch.");
  }

  std::vector<std::uint8_t> decrypted;
  if (!aicam::crypto::Aes256GcmDecrypt(
          key, nonce, {}, encrypted.bytes, encrypted.tag, &decrypted, &error) ||
      decrypted != plaintext) {
    return Fail("AES-256-GCM decryption did not recover plaintext.", &error);
  }
  encrypted.tag[0] ^= 0x01;
  if (aicam::crypto::Aes256GcmDecrypt(
          key, nonce, {}, encrypted.bytes, encrypted.tag, &decrypted, &error)) {
    return Fail("AES-256-GCM accepted a modified authentication tag.");
  }
  return true;
}

bool CheckProtocolVectors() {
  using aicam::protocol::Frame;
  std::string error;
  Frame frame{0x01, {'t', 'e', 's', 't'}};
  std::vector<std::uint8_t> wire;
  if (!aicam::protocol::EncodeFrame(frame, &wire, &error) ||
      ToHex(wire) != "000000050174657374") {
    return Fail("Protocol framing vector mismatch.");
  }

  const auto nonce = aicam::protocol::BuildNonce(0, 42);
  if (ToHex(nonce) != "41494300000000000000002a") {
    return Fail("Protocol nonce vector mismatch.");
  }
  const auto aad = aicam::protocol::BuildAssociatedData(45, 0x11, nonce);
  if (ToHex(aad) != "0000002d1141494300000000000000002a") {
    return Fail("Protocol associated-data vector mismatch.");
  }

  aicam::protocol::FrameParser parser;
  std::vector<Frame> decoded;
  if (!parser.Push(std::span(wire).first(2), &decoded, &error) ||
      !decoded.empty() ||
      !parser.Push(std::span(wire).subspan(2), &decoded, &error) ||
      decoded.size() != 1 || decoded[0].type != frame.type ||
      decoded[0].body != frame.body) {
    return Fail("Fragmented TCP frame parsing failed.");
  }

  aicam::protocol::ReceiveCounter counter;
  if (!counter.Accept(0) || !counter.Accept(1) || counter.Accept(1) ||
      counter.Accept(3) || counter.Accept(0)) {
    return Fail("Receive counter accepted a replay, gap, or rollback.");
  }

  const auto invalid_length = FromHex("ffffffff");
  parser.Reset();
  if (parser.Push(invalid_length, &decoded, &error) || !parser.failed()) {
    return Fail("Oversized frame length was not rejected.");
  }
  return true;
}

bool CheckEncryptedFrameVector() {
  std::array<std::uint8_t, 32> key{};
  aicam::protocol::SessionCipher sender(key, key, 0, 1);
  aicam::protocol::SessionCipher receiver(key, key, 1, 0);
  const auto plaintext = FromHex("70696e67");
  aicam::protocol::Frame encrypted;
  std::wstring error;
  if (!sender.Encrypt(0x15, plaintext, &encrypted, &error)) {
    return Fail("Encrypted frame vector execution failed.", &error);
  }
  if (ToHex(encrypted.body) !=
      "414943000000000000000000ec1a8a9ca4ec00e531a96e73ccd1a8c3d9d21ed4") {
    return Fail("Encrypted protocol frame vector mismatch.");
  }

  std::vector<std::uint8_t> decrypted;
  if (!receiver.Decrypt(encrypted, &decrypted, &error) ||
      decrypted != plaintext) {
    return Fail("Encrypted frame decryption failed.", &error);
  }
  if (receiver.Decrypt(encrypted, &decrypted, &error) ||
      !decrypted.empty()) {
    return Fail("Encrypted frame replay was accepted.");
  }
  return true;
}

bool CheckHandshakeVectors() {
  using namespace aicam::protocol;
  std::string encode_error;
  std::wstring crypto_error;
  Hello phone;
  phone.role = Role::kSender;
  phone.action = Action::kPair;
  phone.nonce = [] {
    std::array<std::uint8_t, 32> value{};
    for (std::size_t i = 0; i < value.size(); ++i) {
      value[i] = static_cast<std::uint8_t>(i);
    }
    return value;
  }();
  phone.public_key = [] {
    std::array<std::uint8_t, 65> value{};
    const auto bytes = FromHex(
        "046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
        "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
    std::copy(bytes.begin(), bytes.end(), value.begin());
    return value;
  }();
  phone.device_name = "Phone";
  phone.model = "Probe";
  phone.platform = 1;
  phone.capabilities = 1;

  Hello receiver;
  receiver.role = Role::kReceiver;
  receiver.action = Action::kPair;
  receiver.nonce = [] {
    std::array<std::uint8_t, 32> value{};
    for (std::size_t i = 0; i < value.size(); ++i) {
      value[i] = static_cast<std::uint8_t>(i + 32);
    }
    return value;
  }();
  receiver.public_key = [] {
    std::array<std::uint8_t, 65> value{};
    const auto bytes = FromHex(
        "047cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc47669978"
        "07775510db8ed040293d9ac69f7430dbba7dade63ce982299e04b79d227873d1");
    std::copy(bytes.begin(), bytes.end(), value.begin());
    return value;
  }();
  receiver.device_name = "PC";
  receiver.model = "Windows";
  receiver.platform = 3;
  receiver.capabilities = 0x01020304;

  std::vector<std::uint8_t> phone_payload;
  std::vector<std::uint8_t> receiver_payload;
  std::vector<std::uint8_t> phone_wire;
  std::vector<std::uint8_t> receiver_wire;
  if (!EncodeHello(phone, &phone_payload, &encode_error) ||
      !EncodeHello(receiver, &receiver_payload, &encode_error) ||
      !EncodeFrame({0x01, phone_payload}, &phone_wire, &encode_error) ||
      !EncodeFrame({0x01, receiver_payload}, &receiver_wire, &encode_error)) {
    return Fail("HELLO encoding failed.", &crypto_error);
  }
  std::string expected_phone_wire;
  std::string expected_receiver_wire;
  std::string expected_transcript_hash;
  std::string expected_phone_proof;
  std::string expected_receiver_proof;
  std::string expected_phone_to_pc_key;
  std::string expected_pc_to_phone_key;
  if (!ReadTestVector("phoneHelloFrameHex", &expected_phone_wire) ||
      !ReadTestVector("receiverHelloFrameHex", &expected_receiver_wire) ||
      !ReadTestVector("transcriptHashHex", &expected_transcript_hash) ||
      !ReadTestVector("phoneProofHex", &expected_phone_proof) ||
      !ReadTestVector("receiverProofHex", &expected_receiver_proof) ||
      !ReadTestVector("phoneToPcKeyHex", &expected_phone_to_pc_key) ||
      !ReadTestVector("pcToPhoneKeyHex", &expected_pc_to_phone_key)) {
    return Fail("Could not read the shared handshake vector file.");
  }
  if (ToHex(phone_wire) != expected_phone_wire ||
      ToHex(receiver_wire) != expected_receiver_wire) {
    return Fail("HELLO wire-format vector mismatch.");
  }

  Hello decoded;
  if (!DecodeHello(phone_payload, &decoded, &encode_error) ||
      decoded.device_name != phone.device_name ||
      decoded.model != phone.model || decoded.nonce != phone.nonce ||
      decoded.public_key != phone.public_key ||
      decoded.capabilities != phone.capabilities) {
    return Fail("HELLO decode round-trip failed.");
  }
  auto invalid_utf8 = phone_payload;
  invalid_utf8[103] = 0xc0;
  if (DecodeHello(invalid_utf8, &decoded, &encode_error)) {
    return Fail("HELLO accepted invalid UTF-8.");
  }

  std::array<std::uint8_t, aicam::crypto::kSha256Size> transcript_hash{};
  if (!HashHandshakeTranscript(phone_wire, receiver_wire, &transcript_hash,
                               &crypto_error) ||
      ToHex(transcript_hash) != expected_transcript_hash) {
    return Fail("Handshake transcript hash vector mismatch.", &crypto_error);
  }
  const std::vector<std::uint8_t> auth_secret(32, 0xaa);
  std::array<std::uint8_t, aicam::crypto::kSha256Size> proof{};
  if (!BuildAuthProof(auth_secret, AuthRole::kPhone, transcript_hash, &proof,
                      &crypto_error) ||
      ToHex(proof) !=
          expected_phone_proof ||
      !BuildAuthProof(auth_secret, AuthRole::kReceiver, transcript_hash,
                      &proof, &crypto_error) ||
      ToHex(proof) !=
          expected_receiver_proof) {
    return Fail("Role-bound AUTH proof vector mismatch.", &crypto_error);
  }

  std::array<std::uint8_t, 32> shared_secret{};
  const auto secret_bytes =
      FromHex("7cf27b188d034f7e8a52380304b51ac3c08969e277f21b35a60b48fc47669978");
  std::copy(secret_bytes.begin(), secret_bytes.end(), shared_secret.begin());
  SessionKeys known_keys;
  if (!ExpandSessionKeys(shared_secret, auth_secret, transcript_hash,
                         &known_keys, &crypto_error) ||
      ToHex(known_keys.phone_to_pc) !=
          expected_phone_to_pc_key ||
      ToHex(known_keys.pc_to_phone) !=
          expected_pc_to_phone_key) {
    return Fail("Handshake session-key derivation vector mismatch.",
                &crypto_error);
  }

  EphemeralP256KeyPair phone_key;
  EphemeralP256KeyPair receiver_key;
  std::array<std::uint8_t, 65> phone_public{};
  std::array<std::uint8_t, 65> receiver_public{};
  SessionKeys phone_keys;
  SessionKeys receiver_keys;
  if (!phone_key.Generate(&crypto_error) ||
      !receiver_key.Generate(&crypto_error) ||
      !phone_key.PublicKey(&phone_public, &crypto_error) ||
      !receiver_key.PublicKey(&receiver_public, &crypto_error) ||
      !DeriveSessionKeys(phone_key, receiver_public, auth_secret,
                         transcript_hash, &phone_keys, &crypto_error) ||
      !DeriveSessionKeys(receiver_key, phone_public, auth_secret,
                         transcript_hash, &receiver_keys, &crypto_error) ||
      phone_keys.phone_to_pc != receiver_keys.phone_to_pc ||
      phone_keys.pc_to_phone != receiver_keys.pc_to_phone) {
    return Fail("P-256 agreement produced different session keys.",
                &crypto_error);
  }
  return true;
}

}  // namespace

int main() {
  if (!CheckCryptoVectors() || !CheckProtocolVectors() ||
      !CheckEncryptedFrameVector() || !CheckHandshakeVectors()) {
    return 1;
  }
  std::cout << "PASS: AES-GCM, HKDF, framing, nonce/AAD, counter, HELLO, "
               "transcript, AUTH, and P-256 handshake vectors\n";
  return 0;
}
