// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_RENDERER_VIRTUAL_CURSOR_LAYER_H_
#define CONTENT_RENDERER_VIRTUAL_CURSOR_LAYER_H_

#include "cc/layers/content_layer_client.h"
#include "cc/layers/picture_layer.h"
#include "content/common/content_export.h"
#include "ui/base/cursor/mojom/cursor_type.mojom.h"
#include "ui/gfx/geometry/point_f.h"

namespace content {

// A compositor layer that renders a virtual cursor for AI agent control.
// This layer uses transform-based positioning for efficient cursor movement
// without requiring repaints. The cursor bitmap is cached per cursor type.
//
// This layer is managed by VirtualCursorLayerManager and is typically
// added as the topmost child of the root layer.
class CONTENT_EXPORT VirtualCursorLayer : public cc::PictureLayer,
                                          public cc::ContentLayerClient {
 public:
  VirtualCursorLayer();

  VirtualCursorLayer(const VirtualCursorLayer&) = delete;
  VirtualCursorLayer& operator=(const VirtualCursorLayer&) = delete;

  // Sets the cursor position in CSS pixels. Uses transform for positioning.
  void SetPosition(float x, float y);

  // Sets the cursor type/shape. This may cause a repaint if the type changes.
  void SetCursorType(ui::mojom::CursorType cursor_type);

  // Gets the current cursor type.
  ui::mojom::CursorType GetCursorType() const { return cursor_type_; }

  // Shows or hides the cursor layer.
  void SetCursorVisible(bool visible);

  // Returns whether the cursor is visible.
  bool IsCursorVisible() const { return visible_; }

  // Gets the current cursor position.
  gfx::PointF GetCursorPosition() const { return position_; }

  // cc::ContentLayerClient implementation.
  scoped_refptr<cc::DisplayItemList> PaintContentsToDisplayList() override;
  bool FillsBoundsCompletely() const override;

 protected:
  ~VirtualCursorLayer() override;

 private:
  // The cursor type determines which shape is drawn.
  ui::mojom::CursorType cursor_type_ = ui::mojom::CursorType::kPointer;

  // The position in CSS pixels (used for transform).
  gfx::PointF position_;

  // Whether the cursor is visible.
  bool visible_ = true;

  // The scale factor for high-DPI rendering.
  float scale_ = 1.0f;

  // Size of the cursor bitmap (in DIPs).
  static constexpr int kCursorSize = 32;
};

}  // namespace content

#endif  // CONTENT_RENDERER_VIRTUAL_CURSOR_LAYER_H_
