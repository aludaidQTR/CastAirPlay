#include "aicam/shared_memory.h"

#include <Aclapi.h>
#include <Sddl.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>

namespace aicam {
namespace {

constexpr char kMagic[] = "AICAMSHM";

struct LocalFreeDeleter final {
  void operator()(void* value) const {
    if (value != nullptr) {
      LocalFree(value);
    }
  }
};

std::wstring CurrentUserSid() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return {};
  }
  const auto close_token = std::unique_ptr<void, decltype(&CloseHandle)>(
      token, CloseHandle);

  DWORD bytes = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
  if (bytes == 0) {
    return {};
  }
  std::vector<std::uint8_t> storage(bytes);
  if (!GetTokenInformation(token, TokenUser, storage.data(), bytes, &bytes)) {
    return {};
  }

  LPWSTR sid = nullptr;
  const auto* user = reinterpret_cast<const TOKEN_USER*>(storage.data());
  if (!ConvertSidToStringSidW(user->User.Sid, &sid)) {
    return {};
  }
  const auto free_sid = std::unique_ptr<void, LocalFreeDeleter>(sid);
  return sid;
}

std::wstring ObjectName(const wchar_t* suffix) {
  const std::wstring sid = CurrentUserSid();
  if (sid.empty()) {
    return {};
  }
  return L"Local\\CastAirPlay.AiCam.v1." + sid + suffix;
}

PSECURITY_DESCRIPTOR CreateSecurityDescriptor(const std::wstring& sid) {
  const std::wstring sddl =
      L"D:P(A;;GA;;;SY)(A;;GA;;;" + sid +
      L")(A;;GR;;;BU)";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
    return nullptr;
  }
  return descriptor;
}

bool ValidateHeader(const SharedHeader* header, std::uint64_t mapped_size) {
  return header != nullptr &&
         std::memcmp(header->magic, kMagic, sizeof(kMagic) - 1) == 0 &&
         header->version == kSharedMemoryVersion &&
         header->header_size == kSharedHeaderSize &&
         header->mapping_size == kMappingSize &&
         mapped_size >= kMappingSize;
}

std::uint64_t LoadSequence(const volatile LONG64* sequence) {
  auto& value = *const_cast<LONG64*>(sequence);
  return static_cast<std::uint64_t>(
      std::atomic_ref<LONG64>(value).load(std::memory_order_acquire));
}

}  // namespace

std::wstring SharedMappingName() { return ObjectName(L""); }

std::wstring SharedEventName() { return ObjectName(L".FrameReady"); }

SharedMemoryWriter::~SharedMemoryWriter() {
  if (header_ != nullptr) {
    UnmapViewOfFile(header_);
  }
  if (event_ != nullptr) {
    CloseHandle(event_);
  }
  if (mapping_ != nullptr) {
    CloseHandle(mapping_);
  }
}

bool SharedMemoryWriter::Create(std::wstring* error) {
  const std::wstring sid = CurrentUserSid();
  const std::wstring map_name = SharedMappingName();
  const std::wstring event_name = SharedEventName();
  if (sid.empty() || map_name.empty() || event_name.empty()) {
    *error = L"Could not determine the current Windows user SID.";
    return false;
  }

  PSECURITY_DESCRIPTOR descriptor = CreateSecurityDescriptor(sid);
  if (descriptor == nullptr) {
    *error = L"Could not create the shared-memory security descriptor.";
    return false;
  }
  const auto free_descriptor =
      std::unique_ptr<void, LocalFreeDeleter>(descriptor);
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.lpSecurityDescriptor = descriptor;

  mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes,
                                PAGE_READWRITE, 0,
                                static_cast<DWORD>(kMappingSize),
                                map_name.c_str());
  if (mapping_ == nullptr) {
    *error = L"CreateFileMappingW failed: " +
             std::to_wstring(GetLastError());
    return false;
  }

  event_ = CreateEventW(&attributes, FALSE, FALSE, event_name.c_str());
  if (event_ == nullptr) {
    *error = L"CreateEventW failed: " + std::to_wstring(GetLastError());
    return false;
  }

  header_ = static_cast<SharedHeader*>(
      MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, kMappingSize));
  if (header_ == nullptr) {
    *error = L"MapViewOfFile failed: " + std::to_wstring(GetLastError());
    return false;
  }

  ZeroMemory(header_, kSharedHeaderSize);
  std::memcpy(header_->magic, kMagic, sizeof(kMagic) - 1);
  header_->version = kSharedMemoryVersion;
  header_->header_size = kSharedHeaderSize;
  header_->mapping_size = kMappingSize;
  header_->pixel_format = kPixelFormatNv12;
  last_slot_ = 2;
  frame_counter_ = 0;
  return true;
}

bool SharedMemoryWriter::Publish(const std::uint8_t* nv12,
                                std::uint32_t width, std::uint32_t height,
                                std::uint32_t stride,
                                std::uint64_t timestamp_us,
                                std::wstring* error) {
  if (header_ == nullptr || nv12 == nullptr || width == 0 || height == 0 ||
      width > kMaximumWidth || height > kMaximumHeight || (width & 1u) != 0 ||
      (height & 1u) != 0 || stride < width || stride > kSlotStride) {
    *error = L"Frame dimensions, stride, or shared-memory state is invalid.";
    return false;
  }

  const std::uint32_t slot = (last_slot_ + 1) % kSlotCount;
  InterlockedIncrement64(&header_->slot_sequence[slot]);
  MemoryBarrier();
  auto* slot_data = reinterpret_cast<std::uint8_t*>(header_) +
                    kSharedHeaderSize + slot * kSlotCapacity;
  const std::size_t active_bytes =
      static_cast<std::size_t>(height) * stride * 3 / 2;
  const std::size_t slot_bytes =
      static_cast<std::size_t>(kSlotCapacity);
  std::memcpy(slot_data, nv12, active_bytes);
  if (active_bytes < slot_bytes) {
    std::memset(slot_data + active_bytes, 0, slot_bytes - active_bytes);
  }
  MemoryBarrier();
  InterlockedIncrement64(&header_->slot_sequence[slot]);

  InterlockedIncrement64(&header_->publication_sequence);
  MemoryBarrier();
  header_->width = width;
  header_->height = height;
  header_->pixel_format = kPixelFormatNv12;
  header_->stride = stride;
  header_->published_slot = slot;
  header_->frame_counter = ++frame_counter_;
  header_->frame_timestamp_us = timestamp_us;
  header_->producer_heartbeat_ms = GetTickCount64();
  MemoryBarrier();
  InterlockedIncrement64(&header_->publication_sequence);

  last_slot_ = slot;
  if (!SetEvent(event_)) {
    *error = L"SetEvent failed: " + std::to_wstring(GetLastError());
    return false;
  }
  return true;
}

SharedMemoryReader::~SharedMemoryReader() {
  if (header_ != nullptr) {
    UnmapViewOfFile(header_);
  }
  if (event_ != nullptr) {
    CloseHandle(event_);
  }
  if (mapping_ != nullptr) {
    CloseHandle(mapping_);
  }
}

bool SharedMemoryReader::TryOpen() {
  if (header_ != nullptr) {
    return true;
  }
  return OpenHandles();
}

bool SharedMemoryReader::OpenHandles() {
  const std::wstring map_name = SharedMappingName();
  const std::wstring event_name = SharedEventName();
  if (map_name.empty() || event_name.empty()) {
    return false;
  }
  mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, map_name.c_str());
  if (mapping_ == nullptr) {
    return false;
  }

  event_ = OpenEventW(SYNCHRONIZE, FALSE, event_name.c_str());
  if (event_ == nullptr) {
    CloseHandle(mapping_);
    mapping_ = nullptr;
    return false;
  }

  header_ = static_cast<const SharedHeader*>(
      MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, kMappingSize));
  if (header_ == nullptr) {
    CloseHandle(event_);
    CloseHandle(mapping_);
    event_ = nullptr;
    mapping_ = nullptr;
    return false;
  }
  mapped_size_ = kMappingSize;
  if (!ValidateHeader(header_, mapped_size_)) {
    UnmapViewOfFile(header_);
    CloseHandle(event_);
    CloseHandle(mapping_);
    header_ = nullptr;
    event_ = nullptr;
    mapping_ = nullptr;
    return false;
  }
  return true;
}

bool SharedMemoryReader::ReadLatest(FrameView* frame) {
  if (frame == nullptr || (!TryOpen()) ||
      !ValidateHeader(header_, mapped_size_)) {
    return false;
  }

  for (int attempt = 0; attempt < 3; ++attempt) {
    const std::uint64_t publication_before =
        LoadSequence(&header_->publication_sequence);
    if ((publication_before & 1u) != 0) {
      continue;
    }

    const std::uint32_t width = header_->width;
    const std::uint32_t height = header_->height;
    const std::uint32_t stride = header_->stride;
    const std::uint32_t format = header_->pixel_format;
    const std::uint32_t slot = header_->published_slot;
    const std::uint64_t counter = header_->frame_counter;
    const std::uint64_t timestamp = header_->frame_timestamp_us;
    const std::uint64_t heartbeat = header_->producer_heartbeat_ms;
    if (counter == 0 || width == 0 || height == 0 || width > kMaximumWidth ||
        height > kMaximumHeight || (width & 1u) != 0 ||
        (height & 1u) != 0 || stride < width || stride > kSlotStride ||
        format != kPixelFormatNv12 || slot >= kSlotCount) {
      return false;
    }

    const std::uint64_t slot_before =
        LoadSequence(&header_->slot_sequence[slot]);
    if ((slot_before & 1u) != 0) {
      continue;
    }
    const std::size_t frame_bytes =
        static_cast<std::size_t>(height) * stride * 3 / 2;
    frame->nv12.resize(frame_bytes);
    const auto* slot_data = reinterpret_cast<const std::uint8_t*>(header_) +
                            kSharedHeaderSize + slot * kSlotCapacity;
    std::memcpy(frame->nv12.data(), slot_data, frame_bytes);
    MemoryBarrier();

    const std::uint64_t slot_after =
        LoadSequence(&header_->slot_sequence[slot]);
    const std::uint64_t publication_after =
        LoadSequence(&header_->publication_sequence);
    if (slot_before != slot_after || publication_before != publication_after ||
        (slot_after & 1u) != 0 || (publication_after & 1u) != 0) {
      continue;
    }

    frame->width = width;
    frame->height = height;
    frame->stride = stride;
    frame->frame_counter = counter;
    frame->timestamp_us = timestamp;
    frame->heartbeat_ms = heartbeat;
    return GetTickCount64() - heartbeat <= kStaleAfterMs;
  }

  return false;
}

}  // namespace aicam
