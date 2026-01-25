#include "chrome/browser/abp/abp_event_collector.h"

#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "chrome/browser/abp/abp_controller.h"

namespace abp {

// AbpEvent implementation
AbpEvent::AbpEvent() = default;
AbpEvent::AbpEvent(const std::string& t, int64_t vt, base::Value::Dict d)
    : type(t), virtual_time_ms(vt), data(std::move(d)) {}
AbpEvent::~AbpEvent() = default;
AbpEvent::AbpEvent(AbpEvent&&) = default;
AbpEvent& AbpEvent::operator=(AbpEvent&&) = default;

// AbpEventCollector implementation
AbpEventCollector::AbpEventCollector(AbpController* controller)
    : controller_(controller) {}

AbpEventCollector::~AbpEventCollector() = default;

void AbpEventCollector::StartCapturing(const std::string& tab_id) {
  capturing_ = true;
  capturing_tab_id_ = tab_id;
  captured_events_.clear();
  LOG(INFO) << "ABP: Started event capturing for tab " << tab_id;
}

std::vector<AbpEvent> AbpEventCollector::StopCapturing() {
  capturing_ = false;
  std::vector<AbpEvent> events = std::move(captured_events_);
  captured_events_.clear();
  capturing_tab_id_.clear();
  LOG(INFO) << "ABP: Stopped event capturing, captured " << events.size()
            << " events";
  return events;
}

int64_t AbpEventCollector::GetVirtualTimeMs() {
  if (!controller_ || capturing_tab_id_.empty()) {
    return base::Time::Now().InMillisecondsSinceUnixEpoch();
  }
  return controller_->GetVirtualTimeMs(capturing_tab_id_);
}

void AbpEventCollector::AddEvent(const std::string& type,
                                  base::Value::Dict data) {
  if (!capturing_) {
    return;
  }

  int64_t virtual_time = GetVirtualTimeMs();
  captured_events_.emplace_back(type, virtual_time, std::move(data));
  LOG(INFO) << "ABP: Captured event type=" << type
            << " virtual_time=" << virtual_time;
}

std::string AbpEventCollector::GenerateFileChooserId() {
  return base::StringPrintf("fc_%d", next_file_chooser_id_++);
}

void AbpEventCollector::OnCdpEvent(const std::string& tab_id,
                                    const std::string& method,
                                    const base::Value::Dict& params) {
  // Only capture events for the tab we're monitoring
  if (!capturing_ || tab_id != capturing_tab_id_) {
    return;
  }

  // Handle navigation events
  if (method == "Page.frameNavigated") {
    const base::Value::Dict* frame = params.FindDict("frame");
    if (frame) {
      base::Value::Dict data;
      data.Set("tab_id", tab_id);
      if (const std::string* url = frame->FindString("url")) {
        data.Set("url", *url);
      }
      if (const std::string* frame_id = frame->FindString("id")) {
        data.Set("frame_id", *frame_id);
      }
      // Check if this is the main frame
      const std::string* parent_id = frame->FindString("parentId");
      data.Set("is_main_frame", parent_id == nullptr);
      AddEvent("navigation", std::move(data));
    }
  }
  // Handle JavaScript dialog events
  else if (method == "Page.javascriptDialogOpening") {
    base::Value::Dict data;
    data.Set("tab_id", tab_id);
    if (const std::string* type = params.FindString("type")) {
      data.Set("dialog_type", *type);
    }
    if (const std::string* message = params.FindString("message")) {
      data.Set("message", *message);
    }
    if (const std::string* default_prompt = params.FindString("defaultPrompt")) {
      data.Set("default_prompt", *default_prompt);
    }
    data.Set("pending", true);
    AddEvent("dialog", std::move(data));
  }
  // Handle file chooser events
  else if (method == "Page.fileChooserOpened") {
    std::string chooser_id = GenerateFileChooserId();

    base::Value::Dict data;
    data.Set("id", chooser_id);
    data.Set("tab_id", tab_id);

    if (const std::string* mode = params.FindString("mode")) {
      // CDP mode is "selectSingle", "selectMultiple", or "save"
      if (*mode == "selectSingle") {
        data.Set("chooser_type", "open");
        data.Set("multiple", false);
      } else if (*mode == "selectMultiple") {
        data.Set("chooser_type", "open_multiple");
        data.Set("multiple", true);
      } else if (*mode == "save") {
        data.Set("chooser_type", "save");
        data.Set("multiple", false);
      } else {
        data.Set("chooser_type", *mode);
        data.Set("multiple", false);
      }
    }

    // Handle accepted MIME types if present
    const base::Value::List* accepts = params.FindList("accepts");
    if (accepts) {
      base::Value::List accepts_copy;
      for (const auto& item : *accepts) {
        accepts_copy.Append(item.Clone());
      }
      data.Set("accepts", std::move(accepts_copy));
    }

    data.Set("pending", true);

    // Notify controller about pending file chooser (for Phase 3 endpoint)
    if (controller_) {
      controller_->OnFileChooserOpened(chooser_id, tab_id, data.Clone());
    }

    AddEvent("file_chooser", std::move(data));
  }
  // Handle dialog closed events
  else if (method == "Page.javascriptDialogClosed") {
    base::Value::Dict data;
    data.Set("tab_id", tab_id);
    if (auto result = params.FindBool("result")) {
      data.Set("accepted", *result);
    }
    if (const std::string* user_input = params.FindString("userInput")) {
      data.Set("user_input", *user_input);
    }
    AddEvent("dialog_closed", std::move(data));
  }
}

void AbpEventCollector::OnTabCreated(const std::string& new_tab_id,
                                      const std::string& opener_tab_id) {
  if (!capturing_) {
    return;
  }

  // Only capture if the opener is the tab we're monitoring
  if (opener_tab_id != capturing_tab_id_) {
    return;
  }

  base::Value::Dict data;
  data.Set("new_tab_id", new_tab_id);
  data.Set("opener_tab_id", opener_tab_id);
  AddEvent("popup", std::move(data));
}

void AbpEventCollector::OnTabClosed(const std::string& tab_id) {
  if (!capturing_) {
    return;
  }

  // Capture any tab close, but mark if it's the monitored tab
  base::Value::Dict data;
  data.Set("tab_id", tab_id);
  data.Set("is_current_tab", tab_id == capturing_tab_id_);
  AddEvent("tab_closed", std::move(data));
}

void AbpEventCollector::OnDownloadStarted(const std::string& download_id,
                                           const std::string& url) {
  if (!capturing_) {
    return;
  }

  base::Value::Dict data;
  data.Set("download_id", download_id);
  data.Set("url", url);
  AddEvent("download_started", std::move(data));
}

void AbpEventCollector::OnDownloadCompleted(const std::string& download_id) {
  if (!capturing_) {
    return;
  }

  base::Value::Dict data;
  data.Set("download_id", download_id);
  AddEvent("download_completed", std::move(data));
}

}  // namespace abp
