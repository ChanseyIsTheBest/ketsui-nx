/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem/device.h>
#include <rex/filesystem/devices/null_entry.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xtypes.h>

REXCVAR_DEFINE_UINT32(license_mask, 0, "Kernel", "Set license mask for activated content");

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

u32 XamContentGetLicenseMask_entry(mapped_u32 mask_ptr, mapped_void overlapped_ptr) {
  // Each bit in the mask represents a granted license. Available licenses
  // seems to vary from game to game, but most appear to use bit 0 to indicate
  // if the game is purchased or not.
  *mask_ptr = REXCVAR_GET(license_mask);

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(),
                                                    X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  } else {
    return X_ERROR_SUCCESS;
  }
}

namespace {

// The packages XamContentResolve names (ketsui-nx, 2026-10-26). On the Xbox 360 a content package is one STFS file,
// and a title can resolve its path and open it: Ketsui opens its save package (NtOpenFile, then
// NtQueryInformationFile FileNetworkOpenInformation) and loads only when its size is 61,440 bytes. The SDK keeps a
// package as a folder of its files, so this device shows each resolved package as a read-only file of the size its
// STFS package would have on the console (ContentManager::StfsPackageSize), present only while the folder exists.
// Reading it gives no bytes: the titles seen so far only ask for its size.
std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

class ContentPackageEntry : public rex::filesystem::NullEntry {
 public:
  ContentPackageEntry(rex::filesystem::Device* device, rex::filesystem::Entry* parent, std::string path,
                      bool directory)
      : NullEntry(device, parent, std::move(path)) {
    attributes_ = directory ? rex::filesystem::kFileAttributeDirectory
                            : rex::filesystem::kFileAttributeNormal | rex::filesystem::kFileAttributeReadOnly;
  }
  void SetSize(uint64_t size) {
    size_ = size;
    allocation_size_ = size;
  }
  // A folder's package: in children_ (what Entry::GetChild searches) while its files exist, else kept aside (never
  // destroyed, so a File opened on it earlier stays valid).
  ContentPackageEntry* Package(const std::string& path, bool present) {
    for (auto* list : {&children_, &hidden_}) {
      for (auto it = list->begin(); it != list->end(); ++it) {
        if ((*it)->path() != path) continue;
        auto* entry = static_cast<ContentPackageEntry*>(it->get());
        auto* target = present ? &children_ : &hidden_;
        if (list != target) {
          target->push_back(std::move(*it));
          list->erase(it);
        }
        return entry;
      }
    }
    auto entry = std::make_unique<ContentPackageEntry>(device_, this, path, false);
    auto* raw = entry.get();
    (present ? children_ : hidden_).push_back(std::move(entry));
    return raw;
  }

 private:
  std::vector<std::unique_ptr<rex::filesystem::Entry>> hidden_;
};

class ContentPackageDevice : public rex::filesystem::Device {
 public:
  static constexpr const char* kMountPath = "\\Device\\XamContentPackages";

  ContentPackageDevice() : Device(kMountPath) {}

  bool Initialize() override {
    root_ = std::make_unique<ContentPackageEntry>(this, nullptr, "", true);
    return true;
  }
  void Dump(rex::string::StringBuffer* string_buffer) override {}
  bool is_read_only() const override { return true; }
  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return 0; }
  uint32_t component_name_max_length() const override { return 40; }
  uint32_t total_allocation_units() const override { return 0x10; }
  uint32_t available_allocation_units() const override { return 0; }
  uint32_t sectors_per_allocation_unit() const override { return 0x80; }
  uint32_t bytes_per_sector() const override { return 0x200; }

  // `key` (<xuid>\<title>\<type>\<file name>) names the package under the mount path; `folder` holds its files on the
  // host. Each folder level above it resolves as a directory (NtCreateFile resolves the parent, then asks it for the
  // file by name).
  void Register(const std::string& key, const std::filesystem::path& folder) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string lower = Lower(key);
    packages_[lower] = Package{key, folder};
    for (size_t at = lower.find('\\'); at != std::string::npos; at = lower.find('\\', at + 1)) {
      auto& dir = dirs_[lower.substr(0, at)];
      if (!dir) dir = std::make_unique<ContentPackageEntry>(this, root_.get(), key.substr(0, at), true);
    }
  }

  rex::filesystem::Entry* ResolvePath(std::string_view path) override {
    // The VFS passes what follows the mount path, separator included.
    while (!path.empty() && (path.front() == '\\' || path.front() == '/')) path.remove_prefix(1);
    if (path.empty()) return root_.get();
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = Lower(std::string(path));
    if (auto dir = dirs_.find(key); dir != dirs_.end()) {
      Refresh(key, dir->second.get());
      return dir->second.get();
    }
    const auto package = packages_.find(key);
    if (package == packages_.end()) return nullptr;
    const size_t at = key.rfind('\\');
    if (at == std::string::npos) return nullptr;
    auto* dir = dirs_[key.substr(0, at)].get();
    if (!dir) return nullptr;
    Refresh(key.substr(0, at), dir);
    auto* entry = dir->Package(package->second.path, true);
    return entry->size() ? entry : nullptr;
  }

 private:
  struct Package {
    std::string path;  // as resolved (the entry's path keeps its case)
    std::filesystem::path folder;
  };

  // The packages of folder `dir_key`, sized from their files now; a package with no files is not in the folder.
  void Refresh(const std::string& dir_key, ContentPackageEntry* dir) {
    for (const auto& [key, package] : packages_) {
      if (key.size() <= dir_key.size() || key.compare(0, dir_key.size(), dir_key) != 0 || key[dir_key.size()] != '\\' ||
          key.find('\\', dir_key.size() + 1) != std::string::npos) {
        continue;
      }
      const uint64_t size = rex::system::xam::ContentManager::StfsPackageSize(package.folder);
      dir->Package(package.path, size != 0)->SetSize(size);
    }
  }

  std::string name_ = "XamContentPackages";
  std::unique_ptr<ContentPackageEntry> root_;
  std::mutex mutex_;
  std::unordered_map<std::string, Package> packages_;
  std::unordered_map<std::string, std::unique_ptr<ContentPackageEntry>> dirs_;
};

ContentPackageDevice* PackageDevice() {
  static ContentPackageDevice* device = [] {
    auto owned = std::make_unique<ContentPackageDevice>();
    ContentPackageDevice* raw = owned.get();
    if (!owned->Initialize() || !REX_KERNEL_STATE()->file_system()->RegisterDevice(std::move(owned))) {
      REXKRNL_ERROR("XamContentResolve: cannot register {}", ContentPackageDevice::kMountPath);
      return static_cast<ContentPackageDevice*>(nullptr);
    }
    return raw;
  }();
  return device;
}

}  // namespace

// The path of a content package (xenia-edge's design: <xuid>\<title>\<type>\<file name>, the xuid a profile's for saved
// games only), here under the device above. Result of buffer_ptr is sent to RtlInitAnsiString; buffer_size is usually
// 260 (max path). The package itself need not exist: opening the path then fails, as on the console.
u32 XamContentResolve_entry(u32 user_index, mapped_void content_data_ptr, mapped_void buffer_ptr,
                            u32 buffer_size, u32 create_directory, mapped_void root_name_ptr,
                            mapped_void overlapped_ptr) {
  if (!content_data_ptr || !buffer_ptr || !buffer_size || root_name_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();
  uint64_t xuid = 0;
  if (content_data.content_type == XContentType::kSavedGame && user_index < 4) {
    xuid = REX_KERNEL_STATE()->user_profile()->xuid();
  }
  auto* device = PackageDevice();
  if (!device) {
    return X_ERROR_NOT_FOUND;
  }
  const std::string key = fmt::format("{:016X}\\{:08X}\\{:08X}\\{}", xuid, REX_KERNEL_STATE()->title_id(),
                                      static_cast<uint32_t>(content_data.content_type.get()),
                                      content_data.file_name());
  device->Register(key, REX_KERNEL_STATE()->content_manager()->PackageHostPath(xuid, content_data));
  const std::string path = std::string(ContentPackageDevice::kMountPath) + "\\" + key;
  char* out = buffer_ptr.as<char*>();
  const size_t n = std::min<size_t>(path.size(), buffer_size - 1);
  std::memcpy(out, path.data(), n);
  out[n] = '\0';
  REXKRNL_DEBUG("XamContentResolve({}, '{}') -> {}", user_index, content_data.file_name(), path);
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

// https://github.com/MrColdbird/gameservice/blob/master/ContentManager.cpp
// https://github.com/LestaD/SourceEngine2007/blob/master/se2007/engine/xboxsystem.cpp#L499
u32 XamContentCreateEnumerator_entry(u32 user_index, u32 device_id, u32 content_type,
                                     u32 content_flags, u32 items_per_enumerate,
                                     mapped_u32 buffer_size_ptr, mapped_u32 handle_out) {
  assert_not_null(handle_out);

  auto device_info = device_id == 0 ? nullptr : GetDummyDeviceInfo(device_id);
  if ((device_id && device_info == nullptr) || !handle_out) {
    if (buffer_size_ptr) {
      *buffer_size_ptr = 0;
    }
    return X_E_INVALIDARG;
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = sizeof(XCONTENT_DATA) * items_per_enumerate;
  }

  uint64_t xuid = REX_KERNEL_STATE()->user_profile()->xuid();

  auto e = make_object<XStaticEnumerator<XCONTENT_DATA>>(REX_KERNEL_STATE(), items_per_enumerate);
  auto result = e->Initialize(0xFF, 0xFE, 0x20005, 0x20007, 0);
  if (XFAILED(result)) {
    return result;
  }

  if (!device_info || device_info->device_id == DummyDeviceId::HDD) {
    // Enumerate user-specific content
    auto content_datas = REX_KERNEL_STATE()->content_manager()->ListContent(
        static_cast<uint32_t>(DummyDeviceId::HDD), xuid, XContentType(uint32_t(content_type)));
    for (const auto& content_data : content_datas) {
      auto item = e->AppendItem();
      *item = content_data;
    }

    // Also enumerate common content (xuid=0)
    if (xuid != 0) {
      auto common_datas = REX_KERNEL_STATE()->content_manager()->ListContent(
          static_cast<uint32_t>(DummyDeviceId::HDD), 0, XContentType(uint32_t(content_type)));
      for (const auto& content_data : common_datas) {
        auto item = e->AppendItem();
        *item = content_data;
      }
    }
  }

  if (!device_info || device_info->device_id == DummyDeviceId::ODD) {
    // TODO(gibbed): disc drive content
  }

  REXKRNL_DEBUG("XamContentCreateEnumerator: added {} items to enumerator", e->item_count());

  *handle_out = e->handle();
  return X_ERROR_SUCCESS;
}

enum class kDispositionState : uint32_t { Unknown = 0, Create = 1, Open = 2 };

u32 xeXamContentCreate(u32 user_index, mapped_string root_name, mapped_void content_data_ptr,
                       u32 content_data_size, u32 flags, mapped_u32 disposition_ptr,
                       mapped_u32 license_mask_ptr, u32 cache_size, u64 content_size,
                       mapped_void overlapped_ptr) {
  uint64_t xuid = REX_KERNEL_STATE()->user_profile()->xuid();

  XCONTENT_AGGREGATE_DATA content_data;
  if (content_data_size == sizeof(XCONTENT_DATA)) {
    content_data = *content_data_ptr.as<XCONTENT_DATA*>();
  } else if (content_data_size == sizeof(XCONTENT_AGGREGATE_DATA)) {
    content_data = *content_data_ptr.as<XCONTENT_AGGREGATE_DATA*>();
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }

  if (content_data.content_type == XContentType::kMarketplaceContent) {
    xuid = 0;
  }

  auto content_manager = REX_KERNEL_STATE()->content_manager();

  if (overlapped_ptr && disposition_ptr) {
    *disposition_ptr = 0;
  }

  auto run = [content_manager, xuid, root_name = root_name.value(), flags, content_data,
              disposition_ptr,
              license_mask_ptr](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    X_RESULT result = X_ERROR_INVALID_PARAMETER;
    kDispositionState disposition = kDispositionState::Unknown;
    switch (flags & 0xF) {
      case 1:  // CREATE_NEW
               // Fail if exists.
        if (content_manager->ContentExists(xuid, content_data)) {
          result = X_ERROR_ALREADY_EXISTS;
        } else {
          disposition = kDispositionState::Create;
        }
        break;
      case 2:  // CREATE_ALWAYS
               // Overwrite existing, if any.
        // Close any existing mount under this root name first.
        // Games may reuse the same root without explicitly closing.
        content_manager->CloseContent(root_name);
        if (content_manager->ContentExists(xuid, content_data)) {
          content_manager->DeleteContent(xuid, content_data);
        }
        // Check filesystem state after deletion attempt to decide
        // whether to create fresh or open existing.
        if (content_manager->ContentExists(xuid, content_data)) {
          disposition = kDispositionState::Open;
        } else {
          disposition = kDispositionState::Create;
        }
        break;
      case 3:  // OPEN_EXISTING
               // Open only if exists.
        if (!content_manager->ContentExists(xuid, content_data)) {
          result = X_ERROR_PATH_NOT_FOUND;
        } else {
          disposition = kDispositionState::Open;
        }
        break;
      case 4:  // OPEN_ALWAYS
               // Create if needed.
        if (!content_manager->ContentExists(xuid, content_data)) {
          disposition = kDispositionState::Create;
        } else {
          disposition = kDispositionState::Open;
        }
        break;
      case 5:  // TRUNCATE_EXISTING
               // Fail if doesn't exist, if does exist delete and recreate.
        if (!content_manager->ContentExists(xuid, content_data)) {
          result = X_ERROR_PATH_NOT_FOUND;
        } else {
          content_manager->CloseContent(root_name);
          content_manager->DeleteContent(xuid, content_data);
          if (content_manager->ContentExists(xuid, content_data)) {
            disposition = kDispositionState::Open;
          } else {
            disposition = kDispositionState::Create;
          }
        }
        break;
      default:
        assert_unhandled_case(flags & 0xF);
        break;
    }

    uint32_t content_license = 0;
    if (disposition == kDispositionState::Create) {
      result = content_manager->CreateContent(root_name, xuid, content_data);
      if (XSUCCEEDED(result)) {
        content_manager->WriteContentHeaderFile(xuid, content_data);
      }
    } else if (disposition == kDispositionState::Open) {
      result = content_manager->OpenContent(root_name, xuid, content_data, content_license);
    }

    if (license_mask_ptr && XSUCCEEDED(result)) {
      *license_mask_ptr = content_license;
    }

    if (disposition_ptr) {
      *disposition_ptr = static_cast<uint32_t>(disposition);
    }

    // What the game asks for and what we answer, in one line. It is the only place where both paths
    // are visible: the one that gets to create or open (which also leaves its own trace) and the one
    // cut short because the content "does not exist", which used to be silent.
    REXKRNL_INFO("[save] XamContentCreate root '{}' content '{}' type {:08X} mode {} -> {} (result {:08X})",
                 root_name, content_data.file_name(), static_cast<uint32_t>(content_data.content_type.get()), flags & 0xF,
                 disposition == kDispositionState::Create  ? "create"
                 : disposition == kDispositionState::Open  ? "open"
                                                           : "none",
                 uint32_t(result));

    extended_error = X_HRESULT_FROM_WIN32(result);
    length = static_cast<uint32_t>(disposition);
    return result;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    return run(extended_error, length);
  } else {
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run, overlapped_ptr.guest_address());
    return X_ERROR_IO_PENDING;
  }
}

u32 XamContentCreateEx_entry(u32 user_index, mapped_string root_name, mapped_void content_data_ptr,
                             u32 flags, mapped_u32 disposition_ptr, mapped_u32 license_mask_ptr,
                             u32 cache_size, u64 content_size, mapped_void overlapped_ptr) {
  return xeXamContentCreate(user_index, root_name, content_data_ptr, sizeof(XCONTENT_DATA), flags,
                            disposition_ptr, license_mask_ptr, cache_size, content_size,
                            overlapped_ptr);
}

u32 XamContentCreate_entry(u32 user_index, mapped_string root_name, mapped_void content_data_ptr,
                           u32 flags, mapped_u32 disposition_ptr, mapped_u32 license_mask_ptr,
                           mapped_void overlapped_ptr) {
  return xeXamContentCreate(user_index, root_name, content_data_ptr, sizeof(XCONTENT_DATA), flags,
                            disposition_ptr, license_mask_ptr, 0, 0, overlapped_ptr);
}

u32 XamContentCreateInternal_entry(mapped_string root_name, mapped_void content_data_ptr, u32 flags,
                                   mapped_u32 disposition_ptr, mapped_u32 license_mask_ptr,
                                   u32 cache_size, u64 content_size, mapped_void overlapped_ptr) {
  return xeXamContentCreate(0xFE, root_name, content_data_ptr, sizeof(XCONTENT_AGGREGATE_DATA),
                            flags, disposition_ptr, license_mask_ptr, cache_size, content_size,
                            overlapped_ptr);
}

u32 XamContentOpenFile_entry(u32 user_index, mapped_string root_name, mapped_string path, u32 flags,
                             mapped_u32 disposition_ptr, mapped_u32 license_mask_ptr,
                             mapped_void overlapped_ptr) {
  // TODO(gibbed): arguments assumed based on XamContentCreate.
  return X_ERROR_FILE_NOT_FOUND;
}

u32 XamContentFlush_entry(mapped_string root_name, mapped_void overlapped_ptr) {
  X_RESULT result = X_ERROR_SUCCESS;
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

u32 XamContentClose_entry(mapped_string root_name, mapped_void overlapped_ptr) {
  // Closes a previously opened root from XamContentCreate*.
  auto result = REX_KERNEL_STATE()->content_manager()->CloseContent(root_name.value());
  if (result != X_ERROR_SUCCESS) {
    REXKRNL_WARN("[save] XamContentClose root '{}': was not open (result {:08X})",
                 root_name.value(), uint32_t(result));
  }

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

u32 XamContentGetCreator_entry(u32 user_index, mapped_void content_data_ptr,
                               mapped_u32 is_creator_ptr, mapped_u64 creator_xuid_ptr,
                               mapped_void overlapped_ptr) {
  auto result = X_ERROR_SUCCESS;

  uint64_t xuid = REX_KERNEL_STATE()->user_profile()->xuid();
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  bool content_exists = REX_KERNEL_STATE()->content_manager()->ContentExists(xuid, content_data);

  if (content_exists) {
    if (content_data.content_type == XContentType::kSavedGame) {
      // User always creates saves.
      *is_creator_ptr = 1;
      if (creator_xuid_ptr) {
        *creator_xuid_ptr = xuid;
      }
    } else {
      *is_creator_ptr = 0;
      if (creator_xuid_ptr) {
        *creator_xuid_ptr = 0;
      }
    }
  } else {
    result = X_ERROR_PATH_NOT_FOUND;
  }

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

u32 XamContentGetThumbnail_entry(u32 user_index, mapped_void content_data_ptr,
                                 mapped_void buffer_ptr, mapped_u32 buffer_size_ptr,
                                 mapped_void overlapped_ptr) {
  assert_not_null(buffer_size_ptr);
  uint32_t buffer_size = *buffer_size_ptr;
  uint64_t xuid = REX_KERNEL_STATE()->user_profile()->xuid();
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  // Get thumbnail (if it exists).
  std::vector<uint8_t> buffer;
  auto result =
      REX_KERNEL_STATE()->content_manager()->GetContentThumbnail(xuid, content_data, &buffer);

  *buffer_size_ptr = uint32_t(buffer.size());

  if (XSUCCEEDED(result)) {
    // Write data, if we were given a pointer.
    // This may have just been a size query.
    if (buffer_ptr) {
      if (buffer_size < buffer.size()) {
        // Dest buffer too small.
        result = X_ERROR_INSUFFICIENT_BUFFER;
      } else {
        // Copy data.
        std::memcpy((uint8_t*)buffer_ptr, buffer.data(), buffer.size());
      }
    }
  }

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

u32 XamContentSetThumbnail_entry(u32 user_index, mapped_void content_data_ptr,
                                 mapped_void buffer_ptr, u32 buffer_size,
                                 mapped_void overlapped_ptr) {
  uint64_t xuid = REX_KERNEL_STATE()->user_profile()->xuid();
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  // Buffer is PNG data.
  auto buffer = std::vector<uint8_t>((uint8_t*)buffer_ptr, (uint8_t*)buffer_ptr + buffer_size);
  auto result = REX_KERNEL_STATE()->content_manager()->SetContentThumbnail(xuid, content_data,
                                                                           std::move(buffer));

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

u32 XamContentDelete_entry(u32 user_index, mapped_void content_data_ptr,
                           mapped_void overlapped_ptr) {
  uint64_t xuid = REX_KERNEL_STATE()->user_profile()->xuid();
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_DATA*>();

  auto result = REX_KERNEL_STATE()->content_manager()->DeleteContent(xuid, content_data);

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

u32 XamContentDeleteInternal_entry(mapped_void content_data_ptr, mapped_void overlapped_ptr) {
  // INFO: Analysis of xam.xex shows that "internal" functions are wrappers with
  // 0xFE as user_index
  uint64_t xuid = REX_KERNEL_STATE()->user_profile()->xuid();
  XCONTENT_AGGREGATE_DATA content_data = *content_data_ptr.as<XCONTENT_AGGREGATE_DATA*>();

  auto result = REX_KERNEL_STATE()->content_manager()->DeleteContent(xuid, content_data);

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr.guest_address(), result);
    return X_ERROR_IO_PENDING;
  } else {
    return result;
  }
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamContentGetLicenseMask, rex::kernel::xam::XamContentGetLicenseMask_entry)
REX_EXPORT(__imp__XamContentResolve, rex::kernel::xam::XamContentResolve_entry)
REX_EXPORT(__imp__XamContentCreateEnumerator, rex::kernel::xam::XamContentCreateEnumerator_entry)
REX_EXPORT(__imp__XamContentCreateEx, rex::kernel::xam::XamContentCreateEx_entry)
REX_EXPORT(__imp__XamContentCreate, rex::kernel::xam::XamContentCreate_entry)
REX_EXPORT(__imp__XamContentCreateInternal, rex::kernel::xam::XamContentCreateInternal_entry)
REX_EXPORT(__imp__XamContentOpenFile, rex::kernel::xam::XamContentOpenFile_entry)
REX_EXPORT(__imp__XamContentFlush, rex::kernel::xam::XamContentFlush_entry)
REX_EXPORT(__imp__XamContentClose, rex::kernel::xam::XamContentClose_entry)
REX_EXPORT(__imp__XamContentGetCreator, rex::kernel::xam::XamContentGetCreator_entry)
REX_EXPORT(__imp__XamContentGetThumbnail, rex::kernel::xam::XamContentGetThumbnail_entry)
REX_EXPORT(__imp__XamContentSetThumbnail, rex::kernel::xam::XamContentSetThumbnail_entry)
REX_EXPORT(__imp__XamContentDelete, rex::kernel::xam::XamContentDelete_entry)
REX_EXPORT(__imp__XamContentDeleteInternal, rex::kernel::xam::XamContentDeleteInternal_entry)

REX_EXPORT_STUB(__imp__XamContentClosePackageFile);
REX_EXPORT_STUB(__imp__XamContentCopyInternal);
REX_EXPORT_STUB(__imp__XamContentCreateAndMountPackage);
REX_EXPORT_STUB(__imp__XamContentCreateEnumeratorInternal);
REX_EXPORT_STUB(__imp__XamContentDismountAndClosePackage);
REX_EXPORT_STUB(__imp__XamContentDuplicateFileHandle);
REX_EXPORT_STUB(__imp__XamContentExistsOnDeviceInternal);
REX_EXPORT_STUB(__imp__XamContentFlushPackage);
REX_EXPORT_STUB(__imp__XamContentGetAttributes);
REX_EXPORT_STUB(__imp__XamContentGetAttributesInternal);
REX_EXPORT_STUB(__imp__XamContentGetHeaderInternal);
REX_EXPORT_STUB(__imp__XamContentGetLocalizedString);
REX_EXPORT_STUB(__imp__XamContentGetMetaDataInternal);
REX_EXPORT_STUB(__imp__XamContentGetMountedPackageByRootName);
REX_EXPORT_STUB(__imp__XamContentGetOnlineCreator);
REX_EXPORT_STUB(__imp__XamContentInstall);
REX_EXPORT_STUB(__imp__XamContentInstallInternal);
REX_EXPORT_STUB(__imp__XamContentIsGameInstalledToHDD);
REX_EXPORT_STUB(__imp__XamContentLaunchImage);
REX_EXPORT_STUB(__imp__XamContentLaunchImageFromFileInternal);
REX_EXPORT_STUB(__imp__XamContentLaunchImageInternal);
REX_EXPORT_STUB(__imp__XamContentLaunchImageInternalEx);
REX_EXPORT_STUB(__imp__XamContentLockUnlockPackageHeaders);
REX_EXPORT_STUB(__imp__XamContentMountInstalledGame);
REX_EXPORT_STUB(__imp__XamContentMountPackage);
REX_EXPORT_STUB(__imp__XamContentMoveInternal);
REX_EXPORT_STUB(__imp__XamContentOpenFileInternal);
REX_EXPORT_STUB(__imp__XamContentOpenPackageFile);
REX_EXPORT_STUB(__imp__XamContentQueryLicenseInternal);
REX_EXPORT_STUB(__imp__XamContentRegisterChangeCallback);
REX_EXPORT_STUB(__imp__XamContentResolveInternal);
REX_EXPORT_STUB(__imp__XamContentSetAttributes);
REX_EXPORT_STUB(__imp__XamContentSetMediaMetaDataInternal);
REX_EXPORT_STUB(__imp__XamContentSetThumbnailInternal);
REX_EXPORT_STUB(__imp__XamContentWritePackageHeader);
