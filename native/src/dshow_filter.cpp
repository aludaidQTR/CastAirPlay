#include "aicam/dshow_filter.h"
#include "aicam/shared_memory.h"

#include <streams.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr LONG kWidth = 640;
constexpr LONG kHeight = 480;
constexpr LONG kOutputStride = (kWidth * 3 + 3) & ~3;
constexpr REFERENCE_TIME kFrameDuration = 333333;

class AiCamStream final : public CSourceStream,
                          public IKsPropertySet,
                          public IAMStreamConfig {
 public:
  DECLARE_IUNKNOWN

  AiCamStream(HRESULT* result, CSource* parent)
      : CSourceStream(L"AiCam stream", result, parent, L"Output") {}

  STDMETHODIMP NonDelegatingQueryInterface(REFIID interface_id,
                                           void** object) override {
    if (interface_id == IID_IKsPropertySet ||
        interface_id == IID_IAMStreamConfig) {
      if (interface_id == IID_IKsPropertySet) {
      return GetInterface(static_cast<IKsPropertySet*>(this), object);
      }
      return GetInterface(static_cast<IAMStreamConfig*>(this), object);
    }
    return CSourceStream::NonDelegatingQueryInterface(interface_id, object);
  }

  STDMETHODIMP Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) override {
    return E_NOTIMPL;
  }

  STDMETHODIMP Get(REFGUID property_set, DWORD property_id, LPVOID, DWORD,
                   LPVOID property_data, DWORD property_data_size,
                   DWORD* bytes_returned) override {
    if (property_set != AMPROPSETID_Pin ||
        property_id != AMPROPERTY_PIN_CATEGORY) {
      return E_NOTIMPL;
    }
    if (property_data == nullptr || bytes_returned == nullptr) {
      return E_POINTER;
    }
    if (property_data_size < sizeof(GUID)) {
      return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    const GUID category = PIN_CATEGORY_CAPTURE;
    CopyMemory(property_data, &category, sizeof(category));
    *bytes_returned = sizeof(category);
    return S_OK;
  }

  STDMETHODIMP QuerySupported(REFGUID property_set, DWORD property_id,
                              DWORD* support) override {
    if (property_set != AMPROPSETID_Pin ||
        property_id != AMPROPERTY_PIN_CATEGORY) {
      return E_NOTIMPL;
    }
    if (support == nullptr) {
      return E_POINTER;
    }
    *support = KSPROPERTY_SUPPORT_GET;
    return S_OK;
  }

  STDMETHODIMP SetFormat(AM_MEDIA_TYPE* media_type) override {
    if (media_type == nullptr) {
      return E_POINTER;
    }
    CMediaType requested(*media_type);
    const HRESULT result = CheckMediaType(&requested);
    if (FAILED(result)) {
      return result;
    }
    CAutoLock lock(m_pFilter->pStateLock());
    m_mt = requested;
    return S_OK;
  }

  STDMETHODIMP GetFormat(AM_MEDIA_TYPE** media_type) override {
    if (media_type == nullptr) {
      return E_POINTER;
    }
    *media_type = nullptr;
    CMediaType current;
    const HRESULT result = GetMediaType(&current);
    if (FAILED(result)) {
      return result;
    }
    *media_type = CreateMediaType(&current);
    return *media_type == nullptr ? E_OUTOFMEMORY : S_OK;
  }

  STDMETHODIMP GetNumberOfCapabilities(int* count, int* capability_size) override {
    if (count == nullptr || capability_size == nullptr) {
      return E_POINTER;
    }
    *count = 1;
    *capability_size = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
  }

  STDMETHODIMP GetStreamCaps(int index, AM_MEDIA_TYPE** media_type,
                             BYTE* capability_data) override {
    if (index != 0) {
      return S_FALSE;
    }
    if (media_type == nullptr || capability_data == nullptr) {
      return E_POINTER;
    }
    *media_type = nullptr;
    const HRESULT result = GetFormat(media_type);
    if (FAILED(result)) {
      return result;
    }

    auto* capabilities =
        reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(capability_data);
    ZeroMemory(capabilities, sizeof(*capabilities));
    capabilities->guid = FORMAT_VideoInfo;
    capabilities->VideoStandard = AnalogVideo_None;
    capabilities->InputSize = {kWidth, kHeight};
    capabilities->MinCroppingSize = {kWidth, kHeight};
    capabilities->MaxCroppingSize = {kWidth, kHeight};
    capabilities->MinOutputSize = {kWidth, kHeight};
    capabilities->MaxOutputSize = {kWidth, kHeight};
    capabilities->MinFrameInterval = kFrameDuration;
    capabilities->MaxFrameInterval = kFrameDuration;
    capabilities->MinBitsPerSecond = kOutputStride * kHeight * 8 * 30;
    capabilities->MaxBitsPerSecond = capabilities->MinBitsPerSecond;
    return S_OK;
  }

  HRESULT GetMediaType(CMediaType* media_type) override {
    if (media_type == nullptr) {
      return E_POINTER;
    }

    media_type->SetType(&MEDIATYPE_Video);
    media_type->SetSubtype(&MEDIASUBTYPE_RGB24);
    media_type->SetFormatType(&FORMAT_VideoInfo);
    media_type->SetTemporalCompression(FALSE);
    media_type->SetSampleSize(kOutputStride * kHeight);

    auto* info = reinterpret_cast<VIDEOINFOHEADER*>(
        media_type->AllocFormatBuffer(sizeof(VIDEOINFOHEADER)));
    if (info == nullptr) {
      return E_OUTOFMEMORY;
    }
    ZeroMemory(info, sizeof(*info));
    info->rcSource.right = kWidth;
    info->rcSource.bottom = kHeight;
    info->rcTarget.right = kWidth;
    info->rcTarget.bottom = kHeight;
    info->dwBitRate = kOutputStride * kHeight * 8 * 30;
    info->AvgTimePerFrame = kFrameDuration;
    info->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info->bmiHeader.biWidth = kWidth;
    info->bmiHeader.biHeight = kHeight;
    info->bmiHeader.biPlanes = 1;
    info->bmiHeader.biBitCount = 24;
    info->bmiHeader.biCompression = BI_RGB;
    info->bmiHeader.biSizeImage = kOutputStride * kHeight;
    return S_OK;
  }

  HRESULT DecideBufferSize(IMemAllocator* allocator,
                           ALLOCATOR_PROPERTIES* properties) override {
    if (allocator == nullptr || properties == nullptr) {
      return E_POINTER;
    }
    properties->cBuffers = 3;
    properties->cbBuffer = kOutputStride * kHeight;
    properties->cbAlign = 4;
    properties->cbPrefix = 0;

    ALLOCATOR_PROPERTIES actual{};
    const HRESULT result = allocator->SetProperties(properties, &actual);
    if (FAILED(result)) {
      return result;
    }
    if (actual.cbBuffer < properties->cbBuffer ||
        actual.cBuffers < properties->cBuffers) {
      return E_FAIL;
    }
    return S_OK;
  }

  HRESULT FillBuffer(IMediaSample* sample) override {
    if (sample == nullptr) {
      return E_POINTER;
    }

    if (reader_.TryOpen()) {
      const DWORD wait_result = WaitForSingleObject(reader_.frame_event(), 33);
      if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_TIMEOUT) {
        return HRESULT_FROM_WIN32(GetLastError());
      }
    } else {
      Sleep(33);
    }

    BYTE* destination = nullptr;
    HRESULT result = sample->GetPointer(&destination);
    if (FAILED(result)) {
      return result;
    }
    if (sample->GetSize() < kOutputStride * kHeight) {
      return E_FAIL;
    }

    aicam::FrameView frame;
    const bool has_frame = reader_.ReadLatest(&frame);
    if (has_frame) {
      ConvertNv12ToBgr24(frame, destination);
      last_frame_ = frame.frame_counter;
    } else {
      DrawWaitingFrame(destination);
    }

    REFERENCE_TIME start =
        static_cast<REFERENCE_TIME>(sample_index_) * kFrameDuration;
    REFERENCE_TIME end = start + kFrameDuration;
    ++sample_index_;
    sample->SetTime(&start, &end);
    sample->SetSyncPoint(TRUE);
    sample->SetDiscontinuity(has_frame ? FALSE : TRUE);
    return sample->SetActualDataLength(kOutputStride * kHeight);
  }

 private:
  static BYTE ClampColor(int value) {
    return static_cast<BYTE>(std::clamp(value, 0, 255));
  }

  static void ConvertNv12ToBgr24(const aicam::FrameView& frame,
                                 BYTE* destination) {
    const std::uint32_t width = frame.width;
    const std::uint32_t height = frame.height;
    const std::uint32_t stride = frame.stride;
    const std::uint8_t* y_plane = frame.nv12.data();
    const std::uint8_t* uv_plane =
        y_plane + static_cast<std::size_t>(height) * stride;

    for (LONG y = 0; y < kHeight; ++y) {
      const std::uint32_t source_y =
          static_cast<std::uint32_t>(y) * height / kHeight;
      BYTE* row = destination +
                  static_cast<std::size_t>(kHeight - 1 - y) * kOutputStride;
      const std::uint8_t* source_y_row = y_plane + source_y * stride;
      const std::uint8_t* source_uv_row =
          uv_plane + (source_y / 2) * stride;

      for (LONG x = 0; x < kWidth; ++x) {
        const std::uint32_t source_x =
            static_cast<std::uint32_t>(x) * width / kWidth;
        const int luminance = source_y_row[source_x];
        const std::size_t uv_offset = source_x & ~std::size_t(1);
        const int u = source_uv_row[uv_offset] - 128;
        const int v = source_uv_row[uv_offset + 1] - 128;
        const int c = std::max(0, luminance - 16);
        row[x * 3] = ClampColor((298 * c + 516 * u + 128) >> 8);
        row[x * 3 + 1] =
            ClampColor((298 * c - 100 * u - 208 * v + 128) >> 8);
        row[x * 3 + 2] = ClampColor((298 * c + 409 * v + 128) >> 8);
      }
    }
  }

  static void DrawWaitingFrame(BYTE* destination) {
    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = kWidth;
    bitmap_info.bmiHeader.biHeight = -kHeight;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &bitmap_info, DIB_RGB_COLORS,
                                      &pixels, nullptr, 0);
    if (bitmap == nullptr || pixels == nullptr) {
      if (bitmap != nullptr) {
        DeleteObject(bitmap);
      }
      std::fill(destination,
                destination + static_cast<std::size_t>(kOutputStride) *
                                  kHeight,
                static_cast<BYTE>(0));
      return;
    }

    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr) {
      DeleteObject(bitmap);
      std::fill(destination,
                destination + static_cast<std::size_t>(kOutputStride) *
                                  kHeight,
                static_cast<BYTE>(0));
      return;
    }
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    RECT bounds{0, 0, kWidth, kHeight};
    HBRUSH background = CreateSolidBrush(RGB(22, 30, 42));
    FillRect(dc, &bounds, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(238, 242, 247));
    HFONT font = CreateFontW(30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(dc, font);
    DrawTextW(dc, L"Waiting for phone...", -1, &bounds,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old_font);
    SelectObject(dc, old_bitmap);

    const auto* source = static_cast<const BYTE*>(pixels);
    for (LONG y = 0; y < kHeight; ++y) {
      const BYTE* source_row =
          source + static_cast<std::size_t>(y) * kWidth * 4;
      BYTE* destination_row =
          destination +
          static_cast<std::size_t>(kHeight - 1 - y) * kOutputStride;
      for (LONG x = 0; x < kWidth; ++x) {
        destination_row[x * 3] = source_row[x * 4];
        destination_row[x * 3 + 1] = source_row[x * 4 + 1];
        destination_row[x * 3 + 2] = source_row[x * 4 + 2];
      }
    }

    DeleteObject(font);
    DeleteDC(dc);
    DeleteObject(bitmap);
  }

  aicam::SharedMemoryReader reader_;
  std::uint64_t sample_index_ = 0;
  std::uint64_t last_frame_ = 0;
};

class AiCamSource final : public CSource {
 public:
  AiCamSource(LPUNKNOWN outer, HRESULT* result)
      : CSource(L"AiCam", outer, CLSID_AiCamDShowFilter, result) {
    if (FAILED(*result)) {
      return;
    }
    auto* stream = new (std::nothrow) AiCamStream(result, this);
    if (stream == nullptr) {
      *result = E_OUTOFMEMORY;
      return;
    }
    if (FAILED(*result)) {
      delete stream;
    }
  }
};

CUnknown* WINAPI CreateAiCamSource(LPUNKNOWN outer, HRESULT* result) {
  auto* filter = new (std::nothrow) AiCamSource(outer, result);
  if (filter == nullptr && result != nullptr) {
    *result = E_OUTOFMEMORY;
  }
  return filter;
}

const REGPINTYPES kMediaTypes[] = {
    {&MEDIATYPE_Video, &MEDIASUBTYPE_RGB24},
};

const REGFILTERPINS2 kPins[] = {
    {REG_PINFLAG_B_OUTPUT, 1, 1, kMediaTypes, 0, nullptr,
     &PIN_CATEGORY_CAPTURE},
};

HRESULT RegisterCameraCategory(bool register_filter) {
  IFilterMapper2* mapper = nullptr;
  HRESULT result = CoCreateInstance(CLSID_FilterMapper2, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_IFilterMapper2,
                                    reinterpret_cast<void**>(&mapper));
  if (FAILED(result)) {
    return result;
  }

  if (register_filter) {
    REGFILTER2 registration{};
    registration.dwVersion = 2;
    registration.dwMerit = MERIT_DO_NOT_USE;
    registration.cPins2 = static_cast<ULONG>(std::size(kPins));
    registration.rgPins2 = kPins;
    result = mapper->RegisterFilter(
        CLSID_AiCamDShowFilter, L"AiCam", nullptr,
        &CLSID_VideoInputDeviceCategory, L"AiCam", &registration);
  } else {
    result = mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory,
                                      L"AiCam", CLSID_AiCamDShowFilter);
    if (result == VFW_E_NOT_FOUND) {
      result = S_OK;
    }
  }
  mapper->Release();
  return result;
}

}  // namespace

extern HINSTANCE g_hInst;
void DllInitClasses(BOOL loading);

CFactoryTemplate g_Templates[] = {
    {L"AiCam", &CLSID_AiCamDShowFilter, CreateAiCamSource, nullptr, nullptr},
};
int g_cTemplates = static_cast<int>(std::size(g_Templates));

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_hInst = instance;
    DisableThreadLibraryCalls(instance);
    DllInitClasses(TRUE);
  } else if (reason == DLL_PROCESS_DETACH) {
    DllInitClasses(FALSE);
  }
  return TRUE;
}

STDAPI DllRegisterServer(void) {
  const HRESULT initialized = CoInitialize(nullptr);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
    return initialized;
  }

  HRESULT result = AMovieDllRegisterServer2(TRUE);
  if (SUCCEEDED(result)) {
    result = RegisterCameraCategory(true);
    if (FAILED(result)) {
      AMovieDllRegisterServer2(FALSE);
    }
  }
  if (SUCCEEDED(initialized)) {
    CoUninitialize();
  }
  return result;
}

STDAPI DllUnregisterServer(void) {
  const HRESULT initialized = CoInitialize(nullptr);
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
    return initialized;
  }

  const HRESULT category_result = RegisterCameraCategory(false);
  const HRESULT server_result = AMovieDllRegisterServer2(FALSE);
  if (SUCCEEDED(initialized)) {
    CoUninitialize();
  }
  return FAILED(category_result) ? category_result : server_result;
}
