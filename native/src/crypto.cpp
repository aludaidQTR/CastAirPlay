#include "aicam/crypto.h"

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <climits>
#include <sstream>

#pragma comment(lib, "bcrypt.lib")

namespace aicam::crypto {
namespace {

class AlgorithmHandle final {
 public:
  ~AlgorithmHandle() {
    if (value_ != nullptr) {
      BCryptCloseAlgorithmProvider(value_, 0);
    }
  }
  BCRYPT_ALG_HANDLE* put() { return &value_; }
  BCRYPT_ALG_HANDLE get() const { return value_; }

 private:
  BCRYPT_ALG_HANDLE value_ = nullptr;
};

class HashHandle final {
 public:
  ~HashHandle() {
    if (value_ != nullptr) {
      BCryptDestroyHash(value_);
    }
  }
  BCRYPT_HASH_HANDLE* put() { return &value_; }
  BCRYPT_HASH_HANDLE get() const { return value_; }

 private:
  BCRYPT_HASH_HANDLE value_ = nullptr;
};

class KeyHandle final {
 public:
  ~KeyHandle() {
    if (value_ != nullptr) {
      BCryptDestroyKey(value_);
    }
  }
  BCRYPT_KEY_HANDLE* put() { return &value_; }
  BCRYPT_KEY_HANDLE get() const { return value_; }

 private:
  BCRYPT_KEY_HANDLE value_ = nullptr;
};

class VectorWiper final {
 public:
  explicit VectorWiper(std::vector<std::uint8_t>* value) : value_(value) {}
  ~VectorWiper() {
    if (value_ != nullptr && !value_->empty()) {
      SecureZeroMemory(value_->data(), value_->size());
    }
  }

 private:
  std::vector<std::uint8_t>* value_;
};

template <typename T, std::size_t Size>
class ArrayWiper final {
 public:
  explicit ArrayWiper(std::array<T, Size>* value) : value_(value) {}
  ~ArrayWiper() {
    if (value_ != nullptr) {
      SecureZeroMemory(value_->data(), value_->size() * sizeof(T));
    }
  }

 private:
  std::array<T, Size>* value_;
};

std::wstring StatusText(NTSTATUS status) {
  std::wostringstream stream;
  stream << L"Windows CNG operation failed (0x" << std::hex
         << static_cast<unsigned long>(status) << L").";
  return stream.str();
}

bool GetDwordProperty(BCRYPT_HANDLE handle, LPCWSTR property, ULONG* value,
                      std::wstring* error) {
  ULONG bytes = 0;
  const NTSTATUS status = BCryptGetProperty(
      handle, property, reinterpret_cast<PUCHAR>(value), sizeof(*value),
      &bytes, 0);
  if (status < 0 || bytes != sizeof(*value)) {
    *error = StatusText(status);
    return false;
  }
  return true;
}

bool HmacImpl(std::span<const std::uint8_t> key,
              std::span<const std::uint8_t> input,
              std::array<std::uint8_t, kSha256Size>* digest,
              std::wstring* error) {
  if (digest == nullptr || error == nullptr || key.size() > ULONG_MAX ||
      input.size() > ULONG_MAX) {
    return false;
  }

  AlgorithmHandle algorithm;
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr,
      BCRYPT_ALG_HANDLE_HMAC_FLAG);
  if (status < 0) {
    *error = StatusText(status);
    return false;
  }

  ULONG object_size = 0;
  ULONG hash_size = 0;
  if (!GetDwordProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH, &object_size,
                        error) ||
      !GetDwordProperty(algorithm.get(), BCRYPT_HASH_LENGTH, &hash_size,
                        error) ||
      hash_size != digest->size()) {
    if (error->empty()) {
      *error = L"Unexpected SHA-256 digest size.";
    }
    return false;
  }

  std::vector<std::uint8_t> hash_object(object_size);
  [[maybe_unused]] const VectorWiper wipe_hash_object(&hash_object);
  HashHandle hash;
  status = BCryptCreateHash(algorithm.get(), hash.put(), hash_object.data(),
                            object_size,
                            const_cast<PUCHAR>(key.data()),
                            static_cast<ULONG>(key.size()), 0);
  if (status < 0) {
    *error = StatusText(status);
    return false;
  }
  status = BCryptHashData(hash.get(), const_cast<PUCHAR>(input.data()),
                          static_cast<ULONG>(input.size()), 0);
  if (status < 0) {
    *error = StatusText(status);
    return false;
  }
  status = BCryptFinishHash(hash.get(), digest->data(),
                            static_cast<ULONG>(digest->size()), 0);
  if (status < 0) {
    *error = StatusText(status);
    return false;
  }
  return true;
}

bool HashImpl(std::span<const std::uint8_t> input,
              std::array<std::uint8_t, kSha256Size>* digest,
              std::wstring* error) {
  if (digest == nullptr || error == nullptr || input.size() > ULONG_MAX) {
    return false;
  }
  AlgorithmHandle algorithm;
  NTSTATUS status = BCryptOpenAlgorithmProvider(
      algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (status < 0) {
    *error = StatusText(status);
    return false;
  }

  ULONG object_size = 0;
  ULONG hash_size = 0;
  if (!GetDwordProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH, &object_size,
                        error) ||
      !GetDwordProperty(algorithm.get(), BCRYPT_HASH_LENGTH, &hash_size,
                        error) ||
      hash_size != digest->size()) {
    if (error->empty()) {
      *error = L"Unexpected SHA-256 digest size.";
    }
    return false;
  }

  std::vector<std::uint8_t> hash_object(object_size);
  [[maybe_unused]] const VectorWiper wipe_hash_object(&hash_object);
  HashHandle hash;
  status = BCryptCreateHash(algorithm.get(), hash.put(), hash_object.data(),
                            object_size, nullptr, 0, 0);
  if (status < 0) {
    *error = StatusText(status);
    return false;
  }
  status = BCryptHashData(hash.get(), const_cast<PUCHAR>(input.data()),
                          static_cast<ULONG>(input.size()), 0);
  if (status < 0) {
    *error = StatusText(status);
    return false;
  }
  status = BCryptFinishHash(hash.get(), digest->data(),
                            static_cast<ULONG>(digest->size()), 0);
  if (status < 0) {
    *error = StatusText(status);
    return false;
  }
  return true;
}

bool OpenAesKey(std::span<const std::uint8_t, kAes256KeySize> key,
                AlgorithmHandle* algorithm, KeyHandle* key_handle,
                std::vector<std::uint8_t>* key_object,
                std::wstring* error) {
  NTSTATUS status =
      BCryptOpenAlgorithmProvider(algorithm->put(), BCRYPT_AES_ALGORITHM,
                                  nullptr, 0);
  if (status < 0) {
    *error = StatusText(status);
    return false;
  }
  const wchar_t mode[] = BCRYPT_CHAIN_MODE_GCM;
  status = BCryptSetProperty(
      algorithm->get(), BCRYPT_CHAINING_MODE,
      reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(mode)), sizeof(mode), 0);
  if (status < 0) {
    *error = StatusText(status);
    return false;
  }

  ULONG object_size = 0;
  if (!GetDwordProperty(algorithm->get(), BCRYPT_OBJECT_LENGTH, &object_size,
                        error)) {
    return false;
  }
  key_object->resize(object_size);
  status = BCryptGenerateSymmetricKey(
      algorithm->get(), key_handle->put(), key_object->data(), object_size,
      const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0);
  if (status < 0) {
    *error = StatusText(status);
    return false;
  }
  return true;
}

}  // namespace

bool RandomBytes(std::span<std::uint8_t> output, std::wstring* error) {
  if (error == nullptr) {
    return false;
  }
  if (output.size() > ULONG_MAX) {
    *error = L"Random output exceeds the Windows CNG size limit.";
    return false;
  }
  if (output.empty()) {
    return true;
  }
  const NTSTATUS status = BCryptGenRandom(
      nullptr, output.data(), static_cast<ULONG>(output.size()),
      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status < 0) {
    *error = StatusText(status);
    return false;
  }
  return true;
}

bool Sha256(std::span<const std::uint8_t> input,
            std::array<std::uint8_t, kSha256Size>* digest,
            std::wstring* error) {
  return HashImpl(input, digest, error);
}

bool HmacSha256(std::span<const std::uint8_t> key,
                std::span<const std::uint8_t> input,
                std::array<std::uint8_t, kSha256Size>* digest,
                std::wstring* error) {
  return HmacImpl(key, input, digest, error);
}

bool HkdfSha256(std::span<const std::uint8_t> input_key_material,
                std::span<const std::uint8_t> salt,
                std::span<const std::uint8_t> info, std::size_t output_size,
                std::vector<std::uint8_t>* output, std::wstring* error) {
  if (output == nullptr || error == nullptr ||
      output_size > 255 * kSha256Size) {
    if (error != nullptr) {
      *error = L"HKDF output length must be at most 255 SHA-256 blocks.";
    }
    return false;
  }

  std::array<std::uint8_t, kSha256Size> zero_salt{};
  std::array<std::uint8_t, kSha256Size> pseudorandom_key{};
  [[maybe_unused]] const ArrayWiper wipe_zero_salt(&zero_salt);
  [[maybe_unused]] const ArrayWiper wipe_pseudorandom_key(&pseudorandom_key);
  if (!HmacImpl(salt.empty() ? std::span<const std::uint8_t>(zero_salt) : salt,
                input_key_material, &pseudorandom_key, error)) {
    return false;
  }

  output->clear();
  output->reserve(output_size);
  std::array<std::uint8_t, kSha256Size> previous{};
  [[maybe_unused]] const ArrayWiper wipe_previous(&previous);
  std::size_t previous_size = 0;
  for (std::uint16_t block = 1; output->size() < output_size; ++block) {
    std::vector<std::uint8_t> input;
    input.reserve(previous_size + info.size() + 1);
    input.insert(input.end(), previous.begin(),
                 previous.begin() + previous_size);
    input.insert(input.end(), info.begin(), info.end());
    input.push_back(static_cast<std::uint8_t>(block));
    if (!HmacImpl(pseudorandom_key, input, &previous, error)) {
      output->clear();
      return false;
    }
    previous_size = previous.size();
    const std::size_t remaining = output_size - output->size();
    const std::size_t count = std::min(remaining, previous.size());
    output->insert(output->end(), previous.begin(), previous.begin() + count);
  }
  return true;
}

bool Aes256GcmEncrypt(
    std::span<const std::uint8_t, kAes256KeySize> key,
    std::span<const std::uint8_t, kGcmNonceSize> nonce,
    std::span<const std::uint8_t> associated_data,
    std::span<const std::uint8_t> plaintext, GcmCiphertext* ciphertext,
    std::wstring* error) {
  if (ciphertext == nullptr || error == nullptr ||
      associated_data.size() > ULONG_MAX || plaintext.size() > ULONG_MAX) {
    return false;
  }
  AlgorithmHandle algorithm;
  std::vector<std::uint8_t> key_object;
  [[maybe_unused]] const VectorWiper wipe_key_object(&key_object);
  KeyHandle key_handle;
  if (!OpenAesKey(key, &algorithm, &key_handle, &key_object, error)) {
    return false;
  }

  ciphertext->bytes.resize(plaintext.size());
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth_info;
  BCRYPT_INIT_AUTH_MODE_INFO(auth_info);
  auth_info.pbNonce = const_cast<PUCHAR>(nonce.data());
  auth_info.cbNonce = static_cast<ULONG>(nonce.size());
  auth_info.pbAuthData = const_cast<PUCHAR>(associated_data.data());
  auth_info.cbAuthData = static_cast<ULONG>(associated_data.size());
  auth_info.pbTag = ciphertext->tag.data();
  auth_info.cbTag = static_cast<ULONG>(ciphertext->tag.size());
  ULONG written = 0;
  NTSTATUS status = BCryptEncrypt(
      key_handle.get(), const_cast<PUCHAR>(plaintext.data()),
      static_cast<ULONG>(plaintext.size()), &auth_info, nullptr, 0,
      ciphertext->bytes.data(), static_cast<ULONG>(ciphertext->bytes.size()),
      &written, 0);
  if (status < 0) {
    *error = StatusText(status);
    ciphertext->bytes.clear();
    return false;
  }
  ciphertext->bytes.resize(written);
  return true;
}

bool Aes256GcmDecrypt(
    std::span<const std::uint8_t, kAes256KeySize> key,
    std::span<const std::uint8_t, kGcmNonceSize> nonce,
    std::span<const std::uint8_t> associated_data,
    std::span<const std::uint8_t> ciphertext,
    std::span<const std::uint8_t, kGcmTagSize> tag,
    std::vector<std::uint8_t>* plaintext, std::wstring* error) {
  if (plaintext == nullptr || error == nullptr ||
      associated_data.size() > ULONG_MAX || ciphertext.size() > ULONG_MAX) {
    return false;
  }
  AlgorithmHandle algorithm;
  std::vector<std::uint8_t> key_object;
  [[maybe_unused]] const VectorWiper wipe_key_object(&key_object);
  KeyHandle key_handle;
  if (!OpenAesKey(key, &algorithm, &key_handle, &key_object, error)) {
    return false;
  }

  plaintext->resize(ciphertext.size());
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth_info;
  BCRYPT_INIT_AUTH_MODE_INFO(auth_info);
  auth_info.pbNonce = const_cast<PUCHAR>(nonce.data());
  auth_info.cbNonce = static_cast<ULONG>(nonce.size());
  auth_info.pbAuthData = const_cast<PUCHAR>(associated_data.data());
  auth_info.cbAuthData = static_cast<ULONG>(associated_data.size());
  auth_info.pbTag = const_cast<PUCHAR>(tag.data());
  auth_info.cbTag = static_cast<ULONG>(tag.size());
  ULONG written = 0;
  NTSTATUS status = BCryptDecrypt(
      key_handle.get(), const_cast<PUCHAR>(ciphertext.data()),
      static_cast<ULONG>(ciphertext.size()), &auth_info, nullptr, 0,
      plaintext->data(), static_cast<ULONG>(plaintext->size()), &written, 0);
  if (status < 0) {
    *error = StatusText(status);
    plaintext->clear();
    return false;
  }
  plaintext->resize(written);
  return true;
}

}  // namespace aicam::crypto
