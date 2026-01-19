// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_RENDERER_VIRTUAL_CURSOR_LAYER_MANAGER_H_
#define CONTENT_RENDERER_VIRTUAL_CURSOR_LAYER_MANAGER_H_

#include "base/memory/raw_ptr.h"
#include "content/common/content_export.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "third_party/blink/public/mojom/page/virtual_cursor.mojom.h"

namespace cc {
class Layer;
}

namespace content {

class VirtualCursorLayer;

// Manages the VirtualCursorLayer lifecycle and implements the Mojo interface
// for controlling the virtual cursor from the browser process.
//
// This class is responsible for:
// - Creating and destroying the cursor layer
// - Ensuring the cursor layer is the topmost child of the root layer
// - Handling Mojo IPC calls from the browser process
class CONTENT_EXPORT VirtualCursorLayerManager
    : public blink::mojom::VirtualCursor {
 public:
  VirtualCursorLayerManager();
  ~VirtualCursorLayerManager() override;

  VirtualCursorLayerManager(const VirtualCursorLayerManager&) = delete;
  VirtualCursorLayerManager& operator=(const VirtualCursorLayerManager&) = delete;

  // Binds this manager to a Mojo interface receiver.
  void Bind(mojo::PendingAssociatedReceiver<blink::mojom::VirtualCursor> receiver);

  // Sets the root layer that the cursor layer will be attached to.
  // Must be called before the cursor can be displayed.
  void SetRootLayer(cc::Layer* root_layer);

  // Gets the cursor layer (may be null if not enabled).
  VirtualCursorLayer* GetCursorLayer() { return cursor_layer_.get(); }

  // Returns whether the virtual cursor is enabled.
  bool IsEnabled() const { return enabled_; }

  // blink::mojom::VirtualCursor implementation.
  void SetPosition(float x, float y, bool visible) override;
  void SetCursorType(ui::mojom::CursorType cursor_type) override;
  void SetVisible(bool visible) override;
  void SetEnabled(bool enabled) override;

 private:
  // Creates the cursor layer and attaches it to the root layer.
  void CreateCursorLayer();

  // Destroys the cursor layer and detaches it from the root layer.
  void DestroyCursorLayer();

  // Ensures the cursor layer is the topmost child of the root layer.
  void EnsureCursorLayerOnTop();

  // The Mojo receiver for the VirtualCursor interface.
  mojo::AssociatedReceiver<blink::mojom::VirtualCursor> receiver_{this};

  // The root layer that the cursor layer is attached to.
  raw_ptr<cc::Layer> root_layer_ = nullptr;

  // The cursor layer (owned via scoped_refptr in cc::Layer).
  scoped_refptr<VirtualCursorLayer> cursor_layer_;

  // Whether the virtual cursor system is enabled.
  bool enabled_ = false;

  // Cached state for when the root layer is not yet set.
  float cached_x_ = 0;
  float cached_y_ = 0;
  bool cached_visible_ = false;
  ui::mojom::CursorType cached_cursor_type_ = ui::mojom::CursorType::kPointer;
};

}  // namespace content

#endif  // CONTENT_RENDERER_VIRTUAL_CURSOR_LAYER_MANAGER_H_
