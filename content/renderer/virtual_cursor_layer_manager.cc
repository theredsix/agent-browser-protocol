// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/virtual_cursor_layer_manager.h"

#include "cc/layers/layer.h"
#include "content/renderer/virtual_cursor_layer.h"

namespace content {

VirtualCursorLayerManager::VirtualCursorLayerManager() = default;

VirtualCursorLayerManager::~VirtualCursorLayerManager() {
  DestroyCursorLayer();
}

void VirtualCursorLayerManager::Bind(
    mojo::PendingAssociatedReceiver<blink::mojom::VirtualCursor> receiver) {
  receiver_.Bind(std::move(receiver));
}

void VirtualCursorLayerManager::SetRootLayer(cc::Layer* root_layer) {
  if (root_layer_ == root_layer) {
    return;
  }

  // If we had a cursor layer attached to the old root, remove it.
  if (cursor_layer_ && root_layer_) {
    cursor_layer_->RemoveFromParent();
  }

  root_layer_ = root_layer;

  // If enabled and we have a root layer, create/attach the cursor layer.
  if (enabled_ && root_layer_) {
    if (!cursor_layer_) {
      CreateCursorLayer();
    } else {
      root_layer_->AddChild(cursor_layer_);
      EnsureCursorLayerOnTop();
    }

    // Apply cached state.
    cursor_layer_->SetPosition(cached_x_, cached_y_);
    cursor_layer_->SetCursorVisible(cached_visible_);
    cursor_layer_->SetCursorType(cached_cursor_type_);
  }
}

void VirtualCursorLayerManager::SetPosition(float x, float y, bool visible) {
  cached_x_ = x;
  cached_y_ = y;
  cached_visible_ = visible;

  if (cursor_layer_) {
    cursor_layer_->SetPosition(x, y);
    cursor_layer_->SetCursorVisible(visible);
  }
}

void VirtualCursorLayerManager::SetCursorType(ui::mojom::CursorType cursor_type) {
  cached_cursor_type_ = cursor_type;

  if (cursor_layer_) {
    cursor_layer_->SetCursorType(cursor_type);
  }
}

void VirtualCursorLayerManager::SetVisible(bool visible) {
  cached_visible_ = visible;

  if (cursor_layer_) {
    cursor_layer_->SetCursorVisible(visible);
  }
}

void VirtualCursorLayerManager::SetEnabled(bool enabled) {
  if (enabled_ == enabled) {
    return;
  }

  enabled_ = enabled;

  if (enabled) {
    if (root_layer_) {
      CreateCursorLayer();
      // Apply cached state.
      cursor_layer_->SetPosition(cached_x_, cached_y_);
      cursor_layer_->SetCursorVisible(cached_visible_);
      cursor_layer_->SetCursorType(cached_cursor_type_);
    }
  } else {
    DestroyCursorLayer();
  }
}

void VirtualCursorLayerManager::CreateCursorLayer() {
  if (cursor_layer_) {
    return;
  }

  cursor_layer_ = base::MakeRefCounted<VirtualCursorLayer>();

  if (root_layer_) {
    root_layer_->AddChild(cursor_layer_);
    EnsureCursorLayerOnTop();
  }
}

void VirtualCursorLayerManager::DestroyCursorLayer() {
  if (!cursor_layer_) {
    return;
  }

  if (root_layer_) {
    cursor_layer_->RemoveFromParent();
  }

  cursor_layer_ = nullptr;
}

void VirtualCursorLayerManager::EnsureCursorLayerOnTop() {
  if (!cursor_layer_ || !root_layer_) {
    return;
  }

  // Remove and re-add to ensure it's the last (topmost) child.
  cursor_layer_->RemoveFromParent();
  root_layer_->AddChild(cursor_layer_);
}

}  // namespace content
