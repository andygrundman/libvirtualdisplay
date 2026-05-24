#include "virtual_display/driver/windows_control_client.h"

#ifdef _WIN32

#include <SetupAPI.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace virtual_display::driver {
  namespace {
    struct DevInfoSet {
      HDEVINFO value {INVALID_HANDLE_VALUE};

      explicit DevInfoSet(HDEVINFO handle):
          value {handle} {
      }

      ~DevInfoSet() {
        if (value != INVALID_HANDLE_VALUE) {
          SetupDiDestroyDeviceInfoList(value);
        }
      }

      DevInfoSet(const DevInfoSet &) = delete;
      DevInfoSet &operator=(const DevInfoSet &) = delete;
    };

    HANDLE invalid_handle() {
      return INVALID_HANDLE_VALUE;
    }

    std::vector<std::wstring> enumerate_control_device_paths(std::uint32_t &native_error) {
      const GUID interface_guid = to_windows_guid(kDeviceInterfaceGuid);
      DevInfoSet device_info_set {
        SetupDiGetClassDevsW(
          &interface_guid,
          nullptr,
          nullptr,
          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
        )
      };
      if (device_info_set.value == INVALID_HANDLE_VALUE) {
        native_error = GetLastError();
        return {};
      }

      std::vector<std::wstring> paths;
      native_error = ERROR_FILE_NOT_FOUND;
      SP_DEVICE_INTERFACE_DATA interface_data {};
      interface_data.cbSize = sizeof(interface_data);

      for (DWORD index = 0; SetupDiEnumDeviceInterfaces(device_info_set.value, nullptr, &interface_guid, index, &interface_data); ++index) {
        DWORD detail_size = 0;
        (void) SetupDiGetDeviceInterfaceDetailW(device_info_set.value, &interface_data, nullptr, 0, &detail_size, nullptr);
        if (detail_size == 0) {
          native_error = GetLastError();
          continue;
        }

        std::vector<std::byte> detail_buffer(detail_size);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(detail_buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(device_info_set.value, &interface_data, detail, detail_size, &detail_size, nullptr)) {
          native_error = GetLastError();
          continue;
        }

        paths.emplace_back(detail->DevicePath);
        native_error = ERROR_SUCCESS;
      }

      if (paths.empty() && native_error == ERROR_NO_MORE_ITEMS) {
        native_error = ERROR_FILE_NOT_FOUND;
      }
      return paths;
    }
  }  // namespace

  WindowsControlTransport::WindowsControlTransport(HANDLE handle):
      handle_ {handle} {
  }

  WindowsControlTransport::~WindowsControlTransport() {
    if (valid()) {
      CloseHandle(handle_);
    }
  }

  WindowsControlTransport::WindowsControlTransport(WindowsControlTransport &&other) noexcept:
      handle_ {other.handle_} {
    other.handle_ = invalid_handle();
  }

  WindowsControlTransport &WindowsControlTransport::operator=(WindowsControlTransport &&other) noexcept {
    if (this != &other) {
      if (valid()) {
        CloseHandle(handle_);
      }
      handle_ = other.handle_;
      other.handle_ = invalid_handle();
    }
    return *this;
  }

  bool WindowsControlTransport::valid() const {
    return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
  }

  bool WindowsControlTransport::ioctl(
    const std::uint32_t ioctl_code,
    const void *input,
    const std::size_t input_size,
    void *output,
    const std::size_t output_size,
    std::size_t &bytes_returned,
    std::uint32_t &native_error
  ) {
    bytes_returned = 0;
    native_error = 0;
    if (!valid() ||
        input_size > (std::numeric_limits<DWORD>::max)() ||
        output_size > (std::numeric_limits<DWORD>::max)()) {
      native_error = ERROR_INVALID_PARAMETER;
      return false;
    }

    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(
      handle_,
      static_cast<DWORD>(ioctl_code),
      const_cast<void *>(input),
      static_cast<DWORD>(input_size),
      output,
      static_cast<DWORD>(output_size),
      &returned,
      nullptr
    );
    bytes_returned = returned;
    if (!ok) {
      native_error = GetLastError();
      return false;
    }
    return true;
  }

  std::vector<WindowsControlDeviceInfo> enumerate_control_devices(std::uint32_t *native_error) {
    std::uint32_t enumerate_error = ERROR_SUCCESS;
    auto paths = enumerate_control_device_paths(enumerate_error);
    if (native_error) {
      *native_error = enumerate_error;
    }

    std::vector<WindowsControlDeviceInfo> devices;
    devices.reserve(paths.size());
    for (const auto &path : paths) {
      WindowsControlDeviceInfo info {};
      info.device_path = path;

      HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
      );
      if (handle != INVALID_HANDLE_VALUE) {
        info.openable = true;
        CloseHandle(handle);
      } else {
        info.native_error = GetLastError();
      }
      devices.push_back(std::move(info));
    }

    return devices;
  }

  WindowsControlOpenResult open_first_control_device() {
    std::uint32_t last_error = ERROR_SUCCESS;
    const auto paths = enumerate_control_device_paths(last_error);

    for (const auto &path : paths) {
      HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
      );
      if (handle != INVALID_HANDLE_VALUE) {
        return {
          ControlStatus::Success,
          std::make_unique<WindowsControlTransport>(handle),
          0,
          path
        };
      }

      last_error = GetLastError();
    }

    if (last_error == ERROR_SUCCESS || last_error == ERROR_NO_MORE_ITEMS) {
      last_error = ERROR_FILE_NOT_FOUND;
    }
    return {ControlStatus::TransportFailed, {}, last_error, {}};
  }
}  // namespace virtual_display::driver

#endif
