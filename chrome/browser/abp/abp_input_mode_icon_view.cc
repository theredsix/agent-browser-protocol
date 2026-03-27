// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/abp/abp_input_mode_icon_view.h"

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "chrome/app/vector_icons/vector_icons.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/views/accessibility/view_accessibility.h"

AbpInputModeIconView::AbpInputModeIconView(
    IconLabelBubbleView::Delegate* icon_label_bubble_delegate,
    PageActionIconView::Delegate* page_action_icon_delegate)
    : PageActionIconView(/*command_updater=*/nullptr,
                         /*command_id=*/0,
                         icon_label_bubble_delegate,
                         page_action_icon_delegate,
                         "AbpInputMode") {
  SetVisible(true);  // Always visible when ABP is active
  GetViewAccessibility().SetName(u"Toggle input mode");

  // Try to register as observer. The controller may not exist yet during
  // early browser window setup — UpdateImpl() will retry.
  TryRegisterObserver();
}

AbpInputModeIconView::~AbpInputModeIconView() {
  if (observer_registered_) {
    abp::AbpController* controller = abp::AbpController::GetInstance();
    if (controller) {
      controller->RemoveInputModeObserver(this);
    }
  }
}

void AbpInputModeIconView::TryRegisterObserver() {
  if (observer_registered_) {
    return;
  }
  abp::AbpController* controller = abp::AbpController::GetInstance();
  if (!controller) {
    return;
  }
  controller->AddInputModeObserver(this);
  current_mode_ = controller->GetInputMode();
  observer_registered_ = true;
  UpdateIconImage();
}

void AbpInputModeIconView::UpdateImpl() {
  // The controller may not exist during early window setup. Retry here
  // since UpdateImpl() is called on tab changes.
  TryRegisterObserver();
  SetVisible(abp::AbpController::GetInstance() != nullptr);
}

void AbpInputModeIconView::OnExecuting(ExecuteSource source) {
  abp::AbpController* controller = abp::AbpController::GetInstance();
  if (!controller) {
    return;
  }

  // Read the controller's actual state to avoid any sync issues.
  abp::AbpController::InputMode actual_mode = controller->GetInputMode();

  if (actual_mode == abp::AbpController::InputMode::kCdp) {
    controller->ExitCdpMode(
        base::BindOnce([](int, const std::string&, std::string) {}));
    return;
  }

  // Toggle between agent and human modes.
  base::Value::Dict params;
  if (actual_mode == abp::AbpController::InputMode::kAgent) {
    params.Set("input_mode", "human");
  } else {
    params.Set("input_mode", "agent");
  }
  controller->SetInputMode(params, base::DoNothing());
}

views::BubbleDialogDelegate* AbpInputModeIconView::GetBubble() const {
  return nullptr;  // No bubble -- direct toggle.
}

void AbpInputModeIconView::UpdateIconImage() {
  if (!GetWidget()) {
    return;
  }

  if (current_mode_ == abp::AbpController::InputMode::kHuman) {
    // Paint the human icon in yellow to match the border overlay.
    const int icon_size = delegate()->GetPageActionIconSize();
    constexpr SkColor kHumanModeYellow = SkColorSetRGB(0xF5, 0xC5, 0x42);
    const gfx::ImageSkia image =
        gfx::CreateVectorIcon(kAbpHumanIcon, icon_size, kHumanModeYellow);
    if (!image.isNull()) {
      SetImageModel(ui::ImageModel::FromImageSkia(image));
    }
    SchedulePaint();
    return;
  }

  if (current_mode_ == abp::AbpController::InputMode::kCdp) {
    const int icon_size = delegate()->GetPageActionIconSize();
    constexpr SkColor kCdpModePurple = SkColorSetRGB(0x9B, 0x59, 0xB6);
    const gfx::ImageSkia image =
        gfx::CreateVectorIcon(kAbpCdpIcon, icon_size, kCdpModePurple);
    if (!image.isNull()) {
      SetImageModel(ui::ImageModel::FromImageSkia(image));
    }
    SchedulePaint();
    return;
  }

  // Agent mode: use the default icon rendering (theme-appropriate color).
  PageActionIconView::UpdateIconImage();
}

const gfx::VectorIcon& AbpInputModeIconView::GetVectorIcon() const {
  if (current_mode_ == abp::AbpController::InputMode::kHuman) {
    return kAbpHumanIcon;
  }
  if (current_mode_ == abp::AbpController::InputMode::kCdp) {
    return kAbpCdpIcon;
  }
  return kAbpRobotIcon;
}

void AbpInputModeIconView::OnInputModeChanged(
    abp::AbpController::InputMode mode) {
  current_mode_ = mode;
  UpdateIconImage();
  // Reset the ink drop highlight — without a bubble to dismiss it, the
  // ACTIVATED state from NotifyClick persists indefinitely.
  SetHighlighted(false);
  // Update tooltip to reflect current mode.
  if (mode == abp::AbpController::InputMode::kHuman) {
    SetTooltipText(u"Switch to agent mode");
  } else if (mode == abp::AbpController::InputMode::kCdp) {
    SetTooltipText(u"Exit CDP mode");
  } else {
    SetTooltipText(u"Switch to human mode");
  }
}

BEGIN_METADATA(AbpInputModeIconView)
END_METADATA
