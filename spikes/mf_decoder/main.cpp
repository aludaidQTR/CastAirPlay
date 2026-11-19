#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT32 kWidth = 320;
constexpr UINT32 kHeight = 240;

HRESULT SetVideoType(IMFMediaType* type, const GUID& subtype) {
  HRESULT hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
  if (SUCCEEDED(hr)) hr = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, kWidth, kHeight);
  if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type, MF_MT_FRAME_RATE, 1, 1);
  if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  return hr;
}

HRESULT WriteNv12(IMFSample* sample, const std::wstring& path) {
  ComPtr<IMFMediaBuffer> buffer;
  HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
  if (FAILED(hr)) return hr;

  ComPtr<IMF2DBuffer2> buffer2d;
  hr = buffer.As(&buffer2d);
  if (FAILED(hr)) return hr;

  BYTE* scanline0 = nullptr;
  BYTE* bufferStart = nullptr;
  LONG pitch = 0;
  DWORD bufferLength = 0;
  hr = buffer2d->Lock2DSize(
      MF2DBuffer_LockFlags_Read,
      &scanline0,
      &pitch,
      &bufferStart,
      &bufferLength);
  if (FAILED(hr)) return hr;

  const auto unlock = [&]() { buffer2d->Unlock2D(); };
  const size_t stride = static_cast<size_t>(pitch < 0 ? -pitch : pitch);
  const size_t yBytes = stride * kHeight;
  const size_t uvBytes = stride * (kHeight / 2);
  if (stride < kWidth || bufferLength < yBytes + uvBytes) {
    unlock();
    return E_UNEXPECTED;
  }

  std::vector<uint8_t> nv12(static_cast<size_t>(kWidth) * kHeight * 3 / 2);
  for (UINT32 row = 0; row < kHeight; ++row) {
    const BYTE* source = scanline0 + static_cast<ptrdiff_t>(row) * pitch;
    std::copy_n(source, kWidth, nv12.data() + static_cast<size_t>(row) * kWidth);
  }
  const BYTE* uv = bufferStart + yBytes;
  for (UINT32 row = 0; row < kHeight / 2; ++row) {
    std::copy_n(
        uv + static_cast<size_t>(row) * stride,
        kWidth,
        nv12.data() + static_cast<size_t>(kWidth) * kHeight +
            static_cast<size_t>(row) * kWidth);
  }
  unlock();

  std::ofstream output(path, std::ios::binary);
  if (!output) return HRESULT_FROM_WIN32(ERROR_CANNOT_MAKE);
  output.write(reinterpret_cast<const char*>(nv12.data()), nv12.size());
  return output ? S_OK : HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
}

HRESULT Decode(const std::wstring& inputPath, const std::wstring& outputPath) {
  std::ifstream input(inputPath, std::ios::binary);
  if (!input) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  const std::istreambuf_iterator<char> begin(input);
  const std::istreambuf_iterator<char> end;
  std::vector<BYTE> bitstream(begin, end);
  if (bitstream.empty()) return E_INVALIDARG;

  MFT_REGISTER_TYPE_INFO inputInfo{MFMediaType_Video, MFVideoFormat_H264};
  MFT_REGISTER_TYPE_INFO outputInfo{MFMediaType_Video, MFVideoFormat_NV12};
  IMFActivate** activations = nullptr;
  UINT32 count = 0;
  HRESULT hr = MFTEnumEx(
      MFT_CATEGORY_VIDEO_DECODER,
      MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
      &inputInfo,
      &outputInfo,
      &activations,
      &count);
  if (FAILED(hr)) return hr;
  if (count == 0) {
    std::wcerr << L"No synchronous Media Foundation H.264-to-NV12 decoder found.\n";
    return MF_E_TOPO_CODEC_NOT_FOUND;
  }

  ComPtr<IMFTransform> decoder;
  for (UINT32 index = 0; index < count; ++index) {
    WCHAR* name = nullptr;
    UINT32 nameLength = 0;
    if (SUCCEEDED(activations[index]->GetAllocatedString(
            MFT_FRIENDLY_NAME_Attribute, &name, &nameLength))) {
      std::wcout << L"Decoder: " << name << L"\n";
      CoTaskMemFree(name);
    }
    if (!decoder) activations[index]->ActivateObject(IID_PPV_ARGS(&decoder));
    activations[index]->Release();
  }
  CoTaskMemFree(activations);
  if (!decoder) return E_NOINTERFACE;

  ComPtr<IMFMediaType> inputType;
  hr = MFCreateMediaType(&inputType);
  if (SUCCEEDED(hr)) hr = SetVideoType(inputType.Get(), MFVideoFormat_H264);
  if (SUCCEEDED(hr)) hr = inputType->SetUINT32(MF_LOW_LATENCY, TRUE);
  if (SUCCEEDED(hr)) hr = decoder->SetInputType(0, inputType.Get(), 0);
  if (FAILED(hr)) return hr;

  ComPtr<IMFMediaType> outputType;
  hr = MFCreateMediaType(&outputType);
  if (SUCCEEDED(hr)) hr = SetVideoType(outputType.Get(), MFVideoFormat_NV12);
  if (SUCCEEDED(hr)) hr = decoder->SetOutputType(0, outputType.Get(), 0);
  if (FAILED(hr)) return hr;

  MFT_OUTPUT_STREAM_INFO streamInfo{};
  hr = decoder->GetOutputStreamInfo(0, &streamInfo);
  if (FAILED(hr)) return hr;

  ComPtr<IMFSample> inputSample;
  ComPtr<IMFMediaBuffer> inputBuffer;
  hr = MFCreateSample(&inputSample);
  if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(static_cast<DWORD>(bitstream.size()), &inputBuffer);
  if (SUCCEEDED(hr)) hr = inputBuffer->SetCurrentLength(static_cast<DWORD>(bitstream.size()));
  if (SUCCEEDED(hr)) {
    BYTE* destination = nullptr;
    DWORD maxLength = 0;
    DWORD currentLength = 0;
    hr = inputBuffer->Lock(&destination, &maxLength, &currentLength);
    if (SUCCEEDED(hr)) {
      std::copy(bitstream.begin(), bitstream.end(), destination);
      inputBuffer->Unlock();
    }
  }
  if (SUCCEEDED(hr)) hr = inputSample->AddBuffer(inputBuffer.Get());
  if (SUCCEEDED(hr)) hr = inputSample->SetSampleTime(0);
  if (SUCCEEDED(hr)) hr = inputSample->SetSampleDuration(10'000'000);
  if (FAILED(hr)) return hr;

  decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  hr = decoder->ProcessInput(0, inputSample.Get(), 0);
  if (FAILED(hr)) return hr;
  decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);

  UINT32 frames = 0;
  for (;;) {
    MFT_OUTPUT_DATA_BUFFER outputData{};
    ComPtr<IMFSample> outputSample;
    if ((streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
      hr = MFCreateSample(&outputSample);
      if (SUCCEEDED(hr)) {
        ComPtr<IMFMediaBuffer> outputBuffer;
        hr = MFCreate2DMediaBuffer(
            kWidth,
            kHeight,
            MFVideoFormat_NV12.Data1,
            FALSE,
            &outputBuffer);
        if (SUCCEEDED(hr)) hr = outputSample->AddBuffer(outputBuffer.Get());
      }
      if (FAILED(hr)) return hr;
      outputData.pSample = outputSample.Get();
    }

    DWORD status = 0;
    hr = decoder->ProcessOutput(0, 1, &outputData, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) return hr;
    if (FAILED(hr)) return hr;
    ComPtr<IMFSample> completed = outputData.pSample;
    if (outputData.pEvents) outputData.pEvents->Release();
    if ((streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0 &&
        outputData.pSample) {
      outputData.pSample->Release();
    }
    hr = WriteNv12(completed.Get(), outputPath);
    if (FAILED(hr)) return hr;
    ++frames;
  }

  std::wcout << L"Decoded " << frames << L" frame(s) to NV12.\n";
  return frames == 0 ? MF_E_TRANSFORM_NEED_MORE_INPUT : S_OK;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 3) {
    std::wcerr << L"Usage: mf_decoder.exe input.h264 output.nv12\n";
    return 2;
  }
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (SUCCEEDED(hr)) hr = MFStartup(MF_VERSION);
  if (SUCCEEDED(hr)) hr = Decode(argv[1], argv[2]);
  if (FAILED(hr)) std::wcerr << L"Media Foundation failed: 0x" << std::hex << hr << L"\n";
  MFShutdown();
  CoUninitialize();
  return FAILED(hr) ? 1 : 0;
}
