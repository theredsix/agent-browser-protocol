// Copyright 2026 Han Wang. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_HTML_FORMS_ABP_DATE_TIME_CHOOSER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_HTML_FORMS_ABP_DATE_TIME_CHOOSER_H_

#include "third_party/blink/public/mojom/choosers/date_time_popup.mojom-blink.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/html/forms/date_time_chooser.h"
#include "third_party/blink/renderer/platform/mojo/heap_mojo_receiver.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"

namespace blink {

class DateTimeChooserClient;
class Element;
class LocalFrame;

// ABP fork: A DateTimeChooser that routes date/time picker requests to the
// browser process (and from there to the embedder interceptor) via
// LocalFrameHost.ShowDateTimePopup, instead of opening the in-renderer page
// popup. It implements the DateTimePopupClient mojo interface so the browser
// can report back the chosen value (or cancellation).
//
// Modeled on ExternalDateTimeChooser.
class CORE_EXPORT AbpDateTimeChooser final
    : public DateTimeChooser,
      public mojom::blink::DateTimePopupClient {
 public:
  // |frame| must not be null.
  AbpDateTimeChooser(LocalFrame* frame,
                     DateTimeChooserClient* client,
                     const DateTimeChooserParameters& parameters);
  ~AbpDateTimeChooser() override;

  void Trace(Visitor*) const override;

  // DateTimeChooser overrides:
  void EndChooser() override;
  AXObject* RootAXObject(Element* popup_owner) override;
  bool IsPickerVisible() const override;

  // mojom::blink::DateTimePopupClient overrides:
  void DidChooseValue(const String& value) override;
  void DidCancel() override;

 private:
  HeapMojoReceiver<mojom::blink::DateTimePopupClient, AbpDateTimeChooser>
      receiver_;

  Member<DateTimeChooserClient> client_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_HTML_FORMS_ABP_DATE_TIME_CHOOSER_H_
