#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aicam {

inline constexpr std::uint32_t kSharedMemoryVersion = 1;
inline constexpr std::uint32_t kSharedHeaderSize = 256;
inline constexpr std::uint32_t kMaximumWidth = 1920;
inline constexpr std::uint32_t kMaximumHeight = 1080;
inline constexpr std::uint32_t kPixelFormatNv12 = 1;
inline constexpr std::uint32_t kSlotCount = 3;
inline constexpr std::uint32_t kSlotStride = (kMaximumWidth + 63u) & ~63u;
inline constexpr std::uint64_t kSlotCapacity =
    static_cast<std::uint64_t>(kSlotStride) * kMaximumHeight * 3 / 2;
inline constexpr std::uint64_t kMappingSize =
    kSharedHeaderSize + kSlotCount * kSlotCapacity;
inline constexpr std::uint64_t kStaleAfterMs = 1000;

struct alignas(8) SharedHeader final {
  char magic[8];
  std::uint32_t version;
  std::uint32_t header_size;
  std::uint64_t mapping_size;
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t pixel_format;
  std::uint32_t stride;
  std::uint32_t published_slot;
  std::uint32_t reserved0;
  std::uint64_t frame_counter;
  std::uint64_t frame_timestamp_us;
  std::uint64_t producer_heartbeat_ms;
  volatile LONG64 publication_sequence;
  volatile LONG64 slot_sequence[kSlotCount];
  std::uint8_t reserved[152];
};

static_assert(sizeof(SharedHeader) == kSharedHeaderSize);
static_assert(offsetof(SharedHeader, frame_counter) == 48);
static_assert(offsetof(SharedHeader, publication_sequence) == 72);
static_assert(offsetof(SharedHeader, slot_sequence) == 80);

struct FrameView final {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t stride = 0;
  std::uint64_t frame_counter = 0;
  std::uint64_t timestamp_us = 0;
  std::uint64_t heartbeat_ms = 0;
  std::vector<std::uint8_t> nv12;
};

class SharedMemoryWriter final {
 public:
  SharedMemoryWriter() = default;
  ~SharedMemoryWriter();
  SharedMemoryWriter(const SharedMemoryWriter&) = delete;
  SharedMemoryWriter& operator=(const SharedMemoryWriter&) = delete;

  bool Create(std::wstring* error);
  bool Publish(const std::uint8_t* nv12, std::uint32_t width,
               std::uint32_t height, std::uint32_t stride,
               std::uint64_t timestamp_us, std::wstring* error);

 private:
  HANDLE mapping_ = nullptr;
  HANDLE event_ = nullptr;
  SharedHeader* header_ = nullptr;
  std::uint32_t last_slot_ = 2;
  std::uint64_t frame_counter_ = 0;
};

class SharedMemoryReader final {
 public:
  SharedMemoryReader() = default;
  ~SharedMemoryReader();
  SharedMemoryReader(const SharedMemoryReader&) = delete;
  SharedMemoryReader& operator=(const SharedMemoryReader&) = delete;

  bool TryOpen();
  bool ReadLatest(FrameView* frame);
  HANDLE frame_event() const { return event_; }

 private:
  bool OpenHandles();
  HANDLE mapping_ = nullptr;
  HANDLE event_ = nullptr;
  const SharedHeader* header_ = nullptr;
  std::uint64_t mapped_size_ = 0;
};

std::wstring SharedMappingName();
std::wstring SharedEventName();

}  // namespace aicam
