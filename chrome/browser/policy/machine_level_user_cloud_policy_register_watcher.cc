// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/policy/machine_level_user_cloud_policy_register_watcher.h"

#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/callback.h"
#include "base/metrics/histogram_macros.h"
#include "base/syslog_logging.h"
#include "chrome/browser/policy/browser_dm_token_storage.h"
#include "chrome/grit/chromium_strings.h"
#include "ui/base/l10n/l10n_util.h"

namespace policy {

using RegisterResult = MachineLevelUserCloudPolicyController::RegisterResult;

const char
    MachineLevelUserCloudPolicyRegisterWatcher::kStartupDialogHistogramName[] =
        "Enterprise.MachineLevelUserCloudPolicyEnrollment.StartupDialog";

MachineLevelUserCloudPolicyRegisterWatcher::
    MachineLevelUserCloudPolicyRegisterWatcher(
        MachineLevelUserCloudPolicyController* controller)
    : controller_(controller) {
  controller_->AddObserver(this);
}
MachineLevelUserCloudPolicyRegisterWatcher::
    ~MachineLevelUserCloudPolicyRegisterWatcher() {
  controller_->RemoveObserver(this);
}

RegisterResult MachineLevelUserCloudPolicyRegisterWatcher::
    WaitUntilCloudPolicyEnrollmentFinished() {
  BrowserDMTokenStorage* token_storage = BrowserDMTokenStorage::Get();

  if (token_storage->RetrieveEnrollmentToken().empty()) {
    return RegisterResult::kNoEnrollmentNeeded;
  }

  // We are already enrolled successfully.
  if (!token_storage->RetrieveDMToken().empty()) {
    return RegisterResult::kEnrollmentSuccess;
  }

  EnterpriseStartupDialog::DialogResultCallback callback = base::BindOnce(
      &MachineLevelUserCloudPolicyRegisterWatcher::OnDialogClosed,
      base::Unretained(this));
  if (dialog_creation_callback_)
    dialog_ = std::move(dialog_creation_callback_).Run(std::move(callback));
  else
    dialog_ = EnterpriseStartupDialog::CreateAndShowDialog(std::move(callback));

  visible_start_time_ = base::Time::Now();
  RecordEnrollmentStartDialog(EnrollmentStartupDialog::kShown);

  if (register_result_) {
    // |register_result_| has been set only if the enrollment has finihsed.
    // And it must be failed if it's finished without a DM token which is
    // checked above. Show the error message directly.
    DCHECK(!register_result_.value());
    DisplayErrorMessage();
  } else {
    // Display the loading dialog and wait for the enrollment process.
    dialog_->DisplayLaunchingInformationWithThrobber(l10n_util::GetStringUTF16(
        IDS_ENTERPRISE_STARTUP_CLOUD_POLICY_ENROLLMENT_TOOLTIP));
  }
  run_loop_.Run();
  if (register_result_.value_or(false))
    return RegisterResult::kEnrollmentSuccess;

  SYSLOG(ERROR) << "Can not start Chrome as machine level user cloud policy "
                   "enrollment has failed. Please double check network "
                   "connection and the status of enrollment token then open "
                   "Chrome again.";
  if (is_restart_needed_)
    return RegisterResult::kRestartDueToFailure;
  return RegisterResult::kQuitDueToFailure;
}

void MachineLevelUserCloudPolicyRegisterWatcher::
    SetDialogCreationCallbackForTesting(DialogCreationCallback callback) {
  dialog_creation_callback_ = std::move(callback);
}

// static
void MachineLevelUserCloudPolicyRegisterWatcher::RecordEnrollmentStartDialog(
    EnrollmentStartupDialog dialog_startup) {
  UMA_HISTOGRAM_ENUMERATION(kStartupDialogHistogramName, dialog_startup);
}

void MachineLevelUserCloudPolicyRegisterWatcher::OnPolicyRegisterFinished(
    bool succeeded) {
  register_result_ = succeeded;

  // If dialog still exists, dismiss the dialog for a success enrollment or
  // show the error message. If dialog has been closed before enrollment
  // finished, Chrome should already be in the shutdown process.
  if (dialog_ && dialog_->IsShowing()) {
    if (register_result_.value()) {
      dialog_.reset();
    } else {
      DisplayErrorMessage();
    }
  }
}

void MachineLevelUserCloudPolicyRegisterWatcher::OnDialogClosed(
    bool is_accepted,
    bool can_show_browser_window) {
  if (can_show_browser_window) {
    // Chrome startup can continue normally.
    RecordEnrollmentStartDialog(EnrollmentStartupDialog::kClosedSuccess);
  } else if (is_accepted) {
    // User chose to restart chrome and try re-enrolling.
    RecordEnrollmentStartDialog(EnrollmentStartupDialog::kClosedRelaunch);
  } else if (register_result_.has_value()) {
    // User closed the dialog after seeing a message that enrollment failed.
    RecordEnrollmentStartDialog(EnrollmentStartupDialog::kClosedFail);
  } else {
    // User closed the dialog after waiting too long with no result.
    RecordEnrollmentStartDialog(EnrollmentStartupDialog::kClosedAbort);
  }

  base::TimeDelta visible_time = base::Time::Now() - visible_start_time_;
  UMA_HISTOGRAM_TIMES(
      "Enterprise.MachineLevelUserCloudPolicyEnrollment.StartupDialogTime",
      visible_time);

  // User confirm the dialog to relaunch Chrome to retry the register.
  is_restart_needed_ = is_accepted;

  // Resume the launch process once the dialog is closed.
  run_loop_.Quit();
}

void MachineLevelUserCloudPolicyRegisterWatcher::DisplayErrorMessage() {
  dialog_->DisplayErrorMessage(
      l10n_util::GetStringUTF16(
          IDS_ENTERPRISE_STARTUP_CLOUD_POLICY_ENROLLMENT_ERROR),
      l10n_util::GetStringUTF16(IDS_ENTERPRISE_STARTUP_RELAUNCH_BUTTON));
}

}  // namespace policy
