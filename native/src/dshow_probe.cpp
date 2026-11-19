#include <Windows.h>
#include <dshow.h>
#include <objbase.h>

#include <iostream>

namespace {

void PrintResult(const wchar_t* operation, HRESULT result) {
  std::wcout << operation << L": 0x" << std::hex
             << static_cast<unsigned long>(result) << std::dec << std::endl;
}

}  // namespace

int wmain() {
  std::wcout.setf(std::ios::unitbuf);
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(initialized)) {
    PrintResult(L"CoInitializeEx", initialized);
    return 1;
  }

  ICreateDevEnum* device_enum = nullptr;
  HRESULT result = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_ICreateDevEnum,
                                    reinterpret_cast<void**>(&device_enum));
  PrintResult(L"Create system device enumerator", result);
  if (FAILED(result)) {
    CoUninitialize();
    return 1;
  }

  IEnumMoniker* monikers = nullptr;
  result = device_enum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                              &monikers, 0);
  PrintResult(L"Enumerate video input category", result);
  if (result != S_OK || monikers == nullptr) {
    device_enum->Release();
    CoUninitialize();
    return 1;
  }

  bool found = false;
  IMoniker* moniker = nullptr;
  ULONG fetched = 0;
  while (monikers->Next(1, &moniker, &fetched) == S_OK) {
    IPropertyBag* properties = nullptr;
    result = moniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag,
                                    reinterpret_cast<void**>(&properties));
    PrintResult(L"Bind device property bag", result);
    if (SUCCEEDED(result)) {
      VARIANT name;
      VariantInit(&name);
      result = properties->Read(L"FriendlyName", &name, nullptr);
      PrintResult(L"Read FriendlyName", result);
      if (SUCCEEDED(result) && name.vt == VT_BSTR) {
        std::wcout << L"Device: " << name.bstrVal << L'\n';
        if (wcscmp(name.bstrVal, L"AiCam") == 0) {
          found = true;
          IBaseFilter* filter = nullptr;
          result = moniker->BindToObject(nullptr, nullptr, IID_IBaseFilter,
                                         reinterpret_cast<void**>(&filter));
          PrintResult(L"Activate AiCam IBaseFilter", result);
          if (SUCCEEDED(result)) {
            IEnumPins* pins = nullptr;
            result = filter->EnumPins(&pins);
            PrintResult(L"Enumerate AiCam pins", result);
            ULONG pin_count = 0;
            if (SUCCEEDED(result)) {
              IPin* pin = nullptr;
              while (pins->Next(1, &pin, &fetched) == S_OK) {
                ++pin_count;
                IKsPropertySet* property_set = nullptr;
                result = pin->QueryInterface(IID_IKsPropertySet,
                                             reinterpret_cast<void**>(&property_set));
                PrintResult(L"Query pin IKsPropertySet", result);
                if (SUCCEEDED(result)) {
                  GUID category{};
                  DWORD bytes_returned = 0;
                  result = property_set->Get(
                      AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, nullptr, 0,
                      &category, sizeof(category), &bytes_returned);
                  PrintResult(L"Read pin category", result);
                  std::wcout << L"Capture category: "
                             << (category == PIN_CATEGORY_CAPTURE ? L"yes" : L"no")
                             << L'\n';
                  property_set->Release();
                }
                pin->Release();
                pin = nullptr;
              }
              pins->Release();
            }
            std::wcout << L"AiCam pin count: " << pin_count << L'\n';
            filter->Release();
            std::wcout << L"Released AiCam filter.\n";
          }
        }
      }
      VariantClear(&name);
      properties->Release();
    }
    moniker->Release();
    std::wcout << L"Released device moniker.\n";
    moniker = nullptr;
  }

  monikers->Release();
  std::wcout << L"Released moniker enumerator.\n";
  device_enum->Release();
  std::wcout << L"Released device enumerator.\n";
  CoUninitialize();
  std::wcout << L"Uninitialized COM.\n";
  std::wcout << (found ? L"AiCam enumerated.\n" : L"AiCam not found.\n");
  return found ? 0 : 2;
}
