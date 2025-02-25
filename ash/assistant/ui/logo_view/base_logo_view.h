// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_ASSISTANT_UI_LOGO_VIEW_BASE_LOGO_VIEW_H_
#define ASH_ASSISTANT_UI_LOGO_VIEW_BASE_LOGO_VIEW_H_

#include "base/macros.h"
#include "ui/views/view.h"

namespace ash {

class BaseLogoView : public views::View {
 public:
  enum class State {
    kUndefined,
    kListening,
    kMicFab,
    kUserSpeaks,
  };

  BaseLogoView();
  ~BaseLogoView() override;

  // If |animate| is true, animates to the |state|.
  virtual void SetState(State state, bool animate) {}

  // Set the speech level for kUserSpeaks state. |speech_level| is the last
  // observed speech level in dB.
  virtual void SetSpeechLevel(float speech_level) {}

  // Creates LogoView based on the build flag ENABLE_CROS_LIBASSISTANT.
  static BaseLogoView* Create();

 private:
  DISALLOW_COPY_AND_ASSIGN(BaseLogoView);
};

}  // namespace ash

#endif  // ASH_ASSISTANT_UI_LOGO_VIEW_BASE_LOGO_VIEW_H_
