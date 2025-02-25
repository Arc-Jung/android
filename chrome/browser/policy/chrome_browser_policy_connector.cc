// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/policy/chrome_browser_policy_connector.h"

#include <memory>
#include <string>
#include <utility>

#include "base/callback.h"
#include "base/command_line.h"
#include "base/path_service.h"
#include "base/task_scheduler/post_task.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/policy/configuration_policy_handler_list_factory.h"
#include "chrome/browser/policy/device_management_service_configuration.h"
#include "chrome/common/chrome_paths.h"
#include "components/policy/core/common/async_policy_provider.h"
#include "components/policy/core/common/cloud/cloud_external_data_manager.h"
#include "components/policy/core/common/cloud/cloud_policy_client_registration_helper.h"
#include "components/policy/core/common/cloud/device_management_service.h"
#include "components/policy/core/common/cloud/user_cloud_policy_manager.h"
#include "components/policy/core/common/configuration_policy_provider.h"
#include "components/policy/core/common/policy_map.h"
#include "components/policy/core/common/policy_namespace.h"
#include "components/policy/core/common/policy_service.h"
#include "components/policy/core/common/policy_types.h"
#include "components/policy/policy_constants.h"
#include "net/url_request/url_request_context_getter.h"

#if defined(OS_WIN)
#include "base/win/registry.h"
#include "components/policy/core/common/policy_loader_win.h"
#elif defined(OS_MACOSX)
#include <CoreFoundation/CoreFoundation.h>
#include "base/mac/foundation_util.h"
#include "base/strings/sys_string_conversions.h"
#include "components/policy/core/common/policy_loader_mac.h"
#include "components/policy/core/common/preferences_mac.h"
#elif defined(OS_POSIX) && !defined(OS_ANDROID)
#include "components/policy/core/common/config_dir_policy_loader.h"
#elif defined(OS_ANDROID)
#include "components/policy/core/browser/android/android_combined_policy_provider.h"
#endif

#if !defined(OS_ANDROID) && !defined(OS_CHROMEOS)
#include "chrome/browser/policy/machine_level_user_cloud_policy_controller.h"
#include "components/policy/core/common/cloud/machine_level_user_cloud_policy_manager.h"
#endif

namespace policy {

ChromeBrowserPolicyConnector::ChromeBrowserPolicyConnector()
    : BrowserPolicyConnector(base::Bind(&BuildHandlerList)) {
#if !defined(OS_ANDROID) && !defined(OS_CHROMEOS)
  machine_level_user_cloud_policy_controller_ =
      std::make_unique<MachineLevelUserCloudPolicyController>();
#endif
}

ChromeBrowserPolicyConnector::~ChromeBrowserPolicyConnector() {}

void ChromeBrowserPolicyConnector::OnResourceBundleCreated() {
  BrowserPolicyConnectorBase::OnResourceBundleCreated();
}

void ChromeBrowserPolicyConnector::Init(
    PrefService* local_state,
    scoped_refptr<net::URLRequestContextGetter> request_context) {
  std::unique_ptr<DeviceManagementService::Configuration> configuration(
      new DeviceManagementServiceConfiguration(
          BrowserPolicyConnector::GetDeviceManagementUrl()));
  std::unique_ptr<DeviceManagementService> device_management_service(
      new DeviceManagementService(std::move(configuration)));
  device_management_service->ScheduleInitialization(
      kServiceInitializationStartupDelay);

  InitInternal(local_state, std::move(device_management_service));

#if !defined(OS_ANDROID) && !defined(OS_CHROMEOS)
  machine_level_user_cloud_policy_controller_->Init(local_state,
                                                    request_context);
#endif
}

bool ChromeBrowserPolicyConnector::IsEnterpriseManaged() const {
  NOTREACHED() << "This method is only defined for Chrome OS";
  return false;
}

void ChromeBrowserPolicyConnector::Shutdown() {
#if !defined(OS_ANDROID) && !defined(OS_CHROMEOS)
  // Reset the controller before calling base class so that
  // shutdown occurs in correct sequence.
  machine_level_user_cloud_policy_controller_.reset();
#endif

  BrowserPolicyConnector::Shutdown();
}

ConfigurationPolicyProvider*
ChromeBrowserPolicyConnector::GetPlatformProvider() {
  ConfigurationPolicyProvider* provider =
      BrowserPolicyConnectorBase::GetPolicyProviderForTesting();
  return provider ? provider : platform_provider_;
}

std::vector<std::unique_ptr<policy::ConfigurationPolicyProvider>>
ChromeBrowserPolicyConnector::CreatePolicyProviders() {
  auto providers = BrowserPolicyConnector::CreatePolicyProviders();
  std::unique_ptr<ConfigurationPolicyProvider> platform_provider =
      CreatePlatformProvider();
  if (platform_provider) {
    platform_provider_ = platform_provider.get();
    // PlatformProvider should be before all other providers (highest priority).
    providers.insert(providers.begin(), std::move(platform_provider));
  }

#if !defined(OS_ANDROID) && !defined(OS_CHROMEOS)
  std::unique_ptr<MachineLevelUserCloudPolicyManager>
      machine_level_user_cloud_policy_manager =
          MachineLevelUserCloudPolicyController::CreatePolicyManager();
  if (machine_level_user_cloud_policy_manager) {
    machine_level_user_cloud_policy_manager_ =
        machine_level_user_cloud_policy_manager.get();
    providers.push_back(std::move(machine_level_user_cloud_policy_manager));
  }
#endif

  return providers;
}

std::unique_ptr<ConfigurationPolicyProvider>
ChromeBrowserPolicyConnector::CreatePlatformProvider() {
#if defined(OS_WIN)
  std::unique_ptr<AsyncPolicyLoader> loader(PolicyLoaderWin::Create(
      base::CreateSequencedTaskRunnerWithTraits(
          {base::MayBlock(), base::TaskPriority::BACKGROUND}),
      kRegistryChromePolicyKey));
  return std::make_unique<AsyncPolicyProvider>(GetSchemaRegistry(),
                                               std::move(loader));
#elif defined(OS_MACOSX)
#if defined(GOOGLE_CHROME_BUILD)
  // Explicitly watch the "com.google.Chrome" bundle ID, no matter what this
  // app's bundle ID actually is. All channels of Chrome should obey the same
  // policies.
  CFStringRef bundle_id = CFSTR("com.google.Chrome");
#else
  base::ScopedCFTypeRef<CFStringRef> bundle_id(
      base::SysUTF8ToCFStringRef(base::mac::BaseBundleID()));
#endif
  auto loader = std::make_unique<PolicyLoaderMac>(
      base::CreateSequencedTaskRunnerWithTraits(
          {base::MayBlock(), base::TaskPriority::BACKGROUND}),
      policy::PolicyLoaderMac::GetManagedPolicyPath(bundle_id),
      new MacPreferences(), bundle_id);
  return std::make_unique<AsyncPolicyProvider>(GetSchemaRegistry(),
                                               std::move(loader));
#elif defined(OS_POSIX) && !defined(OS_ANDROID)
  base::FilePath config_dir_path;
  if (base::PathService::Get(chrome::DIR_POLICY_FILES, &config_dir_path)) {
    std::unique_ptr<AsyncPolicyLoader> loader(new ConfigDirPolicyLoader(
        base::CreateSequencedTaskRunnerWithTraits(
            {base::MayBlock(), base::TaskPriority::BACKGROUND}),
        config_dir_path, POLICY_SCOPE_MACHINE));
    return std::make_unique<AsyncPolicyProvider>(GetSchemaRegistry(),
                                                 std::move(loader));
  } else {
    return nullptr;
  }
#elif defined(OS_ANDROID)
  return std::make_unique<policy::android::AndroidCombinedPolicyProvider>(
      GetSchemaRegistry());
#else
  return nullptr;
#endif
}

}  // namespace policy
