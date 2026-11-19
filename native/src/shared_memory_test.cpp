#include "aicam/shared_memory.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

namespace {

int Fail(const wchar_t* message) {
  std::wcerr << L"FAIL: " << message << L'\n';
  return 1;
}

}  // namespace

int wmain() {
  aicam::SharedMemoryWriter writer;
  std::wstring error;
  if (!writer.Create(&error)) {
    std::wcerr << L"FAIL: writer creation: " << error << L'\n';
    return 1;
  }

  aicam::SharedMemoryReader reader;
  if (reader.ReadLatest(nullptr)) {
    return Fail(L"null output unexpectedly succeeded");
  }

  constexpr std::uint32_t width = 320;
  constexpr std::uint32_t height = 240;
  constexpr std::uint32_t stride = width;
  std::vector<std::uint8_t> frame(
      static_cast<std::size_t>(height) * stride * 3 / 2);

  for (std::uint64_t counter = 1; counter <= 5; ++counter) {
    std::fill(frame.begin(), frame.end(),
              static_cast<std::uint8_t>(counter * 17));
    if (!writer.Publish(frame.data(), width, height, stride,
                        counter * 33333, &error)) {
      std::wcerr << L"FAIL: publish: " << error << L'\n';
      return 1;
    }

    aicam::FrameView output;
    if (!reader.ReadLatest(&output)) {
      return Fail(L"fresh frame was not readable");
    }
    if (output.width != width || output.height != height ||
        output.stride != stride || output.frame_counter != counter ||
        output.nv12 != frame) {
      return Fail(L"latest frame metadata or pixels did not match");
    }
  }

  std::this_thread::sleep_for(
      std::chrono::milliseconds(aicam::kStaleAfterMs + 50));
  aicam::FrameView stale;
  if (reader.ReadLatest(&stale)) {
    return Fail(L"stale producer heartbeat was accepted");
  }

  std::wcout << L"PASS: triple-buffer publication, latest-frame reads, and "
                L"stale-heartbeat rejection\n";
  return 0;
}
