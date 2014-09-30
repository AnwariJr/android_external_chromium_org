// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/chromeos_paths.h"

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/sys_info.h"

namespace chromeos {

namespace {

const base::FilePath::CharType kDefaultAppOrderFileName[] =
#if defined(GOOGLE_CHROME_BUILD)
    FILE_PATH_LITERAL("/usr/share/google-chrome/default_app_order.json");
#else
    FILE_PATH_LITERAL("/usr/share/chromium/default_app_order.json");
#endif  // defined(GOOGLE_CHROME_BUILD)

const base::FilePath::CharType kDefaultUserPolicyKeysDir[] =
    FILE_PATH_LITERAL("/var/run/user_policy");

const base::FilePath::CharType kOwnerKeyFileName[] =
    FILE_PATH_LITERAL("/var/lib/whitelist/owner.key");

const base::FilePath::CharType kInstallAttributesFileName[] =
    FILE_PATH_LITERAL("/var/run/lockbox/install_attributes.pb");

const base::FilePath::CharType kUptimeFileName[] =
    FILE_PATH_LITERAL("/proc/uptime");

const base::FilePath::CharType kUpdateRebootNeededUptimeFile[] =
    FILE_PATH_LITERAL("/var/run/chrome/update_reboot_needed_uptime");

const base::FilePath::CharType kDeviceLocalAccountExtensionDir[] =
    FILE_PATH_LITERAL("/var/cache/device_local_account_extensions");

const base::FilePath::CharType kDeviceLocalAccountExternalDataDir[] =
    FILE_PATH_LITERAL("/var/cache/device_local_account_external_policy_data");

const base::FilePath::CharType kDeviceLocalAccountComponentPolicy[] =
    FILE_PATH_LITERAL("/var/cache/device_local_account_component_policy");

bool PathProvider(int key, base::FilePath* result) {
  switch (key) {
    case FILE_DEFAULT_APP_ORDER:
      *result = base::FilePath(kDefaultAppOrderFileName);
      break;
    case DIR_USER_POLICY_KEYS:
      *result = base::FilePath(kDefaultUserPolicyKeysDir);
      break;
    case FILE_OWNER_KEY:
      *result = base::FilePath(kOwnerKeyFileName);
      break;
    case FILE_INSTALL_ATTRIBUTES:
      *result = base::FilePath(kInstallAttributesFileName);
      break;
    case FILE_UPTIME:
      *result = base::FilePath(kUptimeFileName);
      break;
    case FILE_UPDATE_REBOOT_NEEDED_UPTIME:
      *result = base::FilePath(kUpdateRebootNeededUptimeFile);
      break;
    case DIR_DEVICE_LOCAL_ACCOUNT_EXTENSIONS:
      *result = base::FilePath(kDeviceLocalAccountExtensionDir);
      break;
    case DIR_DEVICE_LOCAL_ACCOUNT_EXTERNAL_DATA:
      *result = base::FilePath(kDeviceLocalAccountExternalDataDir);
      break;
    case DIR_DEVICE_LOCAL_ACCOUNT_COMPONENT_POLICY:
      *result = base::FilePath(kDeviceLocalAccountComponentPolicy);
      break;
    default:
      return false;
  }
  return true;
}

}  // namespace

void RegisterPathProvider() {
  PathService::RegisterProvider(PathProvider, PATH_START, PATH_END);
}

void RegisterStubPathOverrides(const base::FilePath& stubs_dir) {
  CHECK(!base::SysInfo::IsRunningOnChromeOS());
  // Override these paths on the desktop, so that enrollment and cloud
  // policy work and can be tested.
  base::FilePath parent = base::MakeAbsoluteFilePath(stubs_dir);
  PathService::Override(
      DIR_USER_POLICY_KEYS,
      parent.AppendASCII("stub_user_policy"));
  const bool is_absolute = true;
  const bool create = false;
  PathService::OverrideAndCreateIfNeeded(
      FILE_OWNER_KEY,
      parent.AppendASCII("stub_owner.key"),
      is_absolute,
      create);
  PathService::OverrideAndCreateIfNeeded(
      FILE_INSTALL_ATTRIBUTES,
      parent.AppendASCII("stub_install_attributes.pb"),
      is_absolute,
      create);
  PathService::Override(
      DIR_DEVICE_LOCAL_ACCOUNT_EXTENSIONS,
      parent.AppendASCII("stub_device_local_account_extensions"));
  PathService::Override(
      DIR_DEVICE_LOCAL_ACCOUNT_EXTERNAL_DATA,
      parent.AppendASCII("stub_device_local_account_external_data"));
  PathService::Override(
      DIR_DEVICE_LOCAL_ACCOUNT_COMPONENT_POLICY,
      parent.AppendASCII("stub_device_local_account_component_policy"));
}

}  // namespace chromeos
