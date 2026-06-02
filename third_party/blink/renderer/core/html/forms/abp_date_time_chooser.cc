// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/html/forms/abp_date_time_chooser.h"

#include <utility>

#include "third_party/blink/public/mojom/choosers/date_time_popup.mojom-blink.h"
#include "third_party/blink/public/mojom/frame/frame.mojom-blink.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/renderer/core/accessibility/ax_object_cache.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/html/forms/date_time_chooser_client.h"
#include "third_party/blink/renderer/core/html/forms/input_type.h"

namespace blink {

AbpDateTimeChooser::AbpDateTimeChooser(
    LocalFrame* frame,
    DateTimeChooserClient* client,
    const DateTimeChooserParameters& parameters)
    : receiver_(this, frame->DomWindow()->GetExecutionContext()),
      client_(client) {
  DCHECK(frame);
  DCHECK(client);

  auto popup_params = mojom::blink::DateTimePopupParams::New();
  popup_params->input_type = InputType::TypeToString(parameters.type);
  // ISO strings are carried from HTMLInputElement::SetupDateTimeChooserParameters
  // (they are already serialized for temporal inputs), so we avoid having to
  // invert the doubles in DateTimeChooserParameters here.
  // The mojo `string` fields are non-nullable. Absent min/max attributes (and
  // an unset value) yield a null WTF::String, so coerce to empty.
  popup_params->value =
      parameters.value_string.IsNull() ? g_empty_string : parameters.value_string;
  popup_params->min =
      parameters.min_string.IsNull() ? g_empty_string : parameters.min_string;
  popup_params->max =
      parameters.max_string.IsNull() ? g_empty_string : parameters.max_string;
  popup_params->step = parameters.step;
  for (const auto& suggestion : parameters.suggestions) {
    popup_params->suggestions.push_back(suggestion->Clone());
  }
  popup_params->bounds = parameters.anchor_rect_in_screen;

  // Per the spec, opening a picker is a user interaction.
  // https://html.spec.whatwg.org/multipage/input.html#common-input-element-events
  auto task_runner = frame->GetTaskRunner(TaskType::kUserInteraction);
  frame->GetLocalFrameHostRemote().ShowDateTimePopup(
      std::move(popup_params),
      receiver_.BindNewPipeAndPassRemote(std::move(task_runner)));
}

AbpDateTimeChooser::~AbpDateTimeChooser() = default;

void AbpDateTimeChooser::Trace(Visitor* visitor) const {
  visitor->Trace(receiver_);
  visitor->Trace(client_);
  DateTimeChooser::Trace(visitor);
}

void AbpDateTimeChooser::DidChooseValue(const String& value) {
  // The picker is resolved; drop the mojo receiver so this object can be
  // collected and stops holding the pipe (mirrors ExternalDateTimeChooser).
  receiver_.reset();
  // Cache the owner element first, because DidChooseValue might run
  // JavaScript code and destroy |client_|.
  Element* element = client_ ? &client_->OwnerElement() : nullptr;
  if (client_) {
    client_->DidChooseValue(value);
  }

  // Post an accessibility event on the owner element to indicate the
  // value changed.
  if (element) {
    if (AXObjectCache* cache = element->GetDocument().ExistingAXObjectCache()) {
      cache->HandleValueChanged(element);
    }
  }

  // DidChooseValue might run JavaScript code, and EndChooser() might be
  // called, which clears |client_|.
  if (client_) {
    client_->DidEndChooser();
  }
  client_ = nullptr;
}

void AbpDateTimeChooser::DidCancel() {
  receiver_.reset();
  if (client_) {
    client_->DidEndChooser();
  }
  client_ = nullptr;
}

void AbpDateTimeChooser::EndChooser() {
  if (receiver_.is_bound()) {
    receiver_.reset();
  }
  DateTimeChooserClient* client = client_;
  client_ = nullptr;
  if (client) {
    client->DidEndChooser();
  }
}

AXObject* AbpDateTimeChooser::RootAXObject(Element* popup_owner) {
  return nullptr;
}

bool AbpDateTimeChooser::IsPickerVisible() const {
  return receiver_.is_bound();
}

}  // namespace blink
