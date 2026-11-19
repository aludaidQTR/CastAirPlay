#include "aicam/shared_memory.h"

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint32_t kWidth = 640;
constexpr std::uint32_t kHeight = 480;
constexpr std::uint32_t kStride = kWidth;

void MakePattern(std::vector<std::uint8_t>* frame, std::uint32_t phase) {
  const std::size_t y_size = static_cast<std::size_t>(kStride) * kHeight;
  frame->resize(y_size + y_size / 2);
  auto* y_plane = frame->data();
  auto* uv_plane = y_plane + y_size;

  constexpr std::uint32_t kBars[] = {81, 145, 41, 210, 170, 106, 170, 106};
  for (std::uint32_t y = 0; y < kHeight; ++y) {
    for (std::uint32_t x = 0; x < kWidth; ++x) {
      const std::uint32_t moving_x = (x + phase * 7) % kWidth;
      const std::uint32_t bar = moving_x * std::size(kBars) / kWidth;
      const std::uint8_t luminance =
          static_cast<std::uint8_t>(kBars[bar] + ((y / 24) % 2) * 8);
      y_plane[static_cast<std::size_t>(y) * kStride + x] = luminance;
    }
  }

  for (std::uint32_t y = 0; y < kHeight / 2; ++y) {
    for (std::uint32_t x = 0; x < kWidth; x += 2) {
      const std::uint32_t bar =
          ((x + phase * 7) % kWidth) * std::size(kBars) / kWidth;
      const std::size_t offset =
          static_cast<std::size_t>(y) * kStride + x;
      uv_plane[offset] = static_cast<std::uint8_t>(96 + (bar * 17) % 96);
      uv_plane[offset + 1] =
          static_cast<std::uint8_t>(160 - (bar * 13) % 96);
    }
  }

  const std::uint32_t marker_x = (phase * 9) % (kWidth - 80);
  for (std::uint32_t y = 30; y < 54; ++y) {
    for (std::uint32_t x = marker_x; x < marker_x + 80; ++x) {
      y_plane[static_cast<std::size_t>(y) * kStride + x] = 235;
    }
  }
}

}  // namespace

int wmain() {
  aicam::SharedMemoryWriter writer;
  std::wstring error;
  if (!writer.Create(&error)) {
    std::wcerr << L"Could not create AiCam shared memory: " << error << L'\n';
    return 1;
  }

  std::vector<std::uint8_t> frame;
  std::uint32_t phase = 0;
  const auto period = std::chrono::milliseconds(33);
  auto next_frame = std::chrono::steady_clock::now();
  std::wcout << L"Publishing 640x480 NV12 test pattern; press Ctrl+C to stop.\n";
  while (true) {
    MakePattern(&frame, phase++);
    const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    if (!writer.Publish(frame.data(), kWidth, kHeight, kStride,
                        static_cast<std::uint64_t>(timestamp), &error)) {
      std::wcerr << L"Could not publish test frame: " << error << L'\n';
      return 1;
    }
    next_frame += period;
    std::this_thread::sleep_until(next_frame);
  }
}
