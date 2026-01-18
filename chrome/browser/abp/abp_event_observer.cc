#include "chrome/browser/abp/abp_event_observer.h"

#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "chrome/browser/abp/abp_history_controller.h"
#include "content/public/browser/browser_thread.h"

namespace abp {

namespace {

// Rate limiting: minimum interval between scroll events (ms)
constexpr int kScrollRateLimitMs = 100;

}  // namespace

// AbpCdpEventClient implementation

AbpCdpEventClient::AbpCdpEventClient(
    scoped_refptr<content::DevToolsAgentHost> host,
    EventCallback callback)
    : host_(std::move(host)),
      tab_id_(host_->GetId()),
      callback_(std::move(callback)) {
  host_->AttachClient(this);
}

AbpCdpEventClient::~AbpCdpEventClient() {
  if (host_) {
    host_->DetachClient(this);
  }
}

void AbpCdpEventClient::EnableEventCapture() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // Enable Page domain for navigation and dialog events
  SendCommand("Page.enable");

  // Enable Target domain for popup/tab events
  // Note: This is typically enabled at browser level, but doesn't hurt to call

  // Enable Browser domain for download events
  // Note: Browser domain is tricky - may need different approach
}

void AbpCdpEventClient::SendCommand(const std::string& method,
                                    const base::Value::Dict& params) {
  if (!host_) {
    return;
  }

  base::Value::Dict message;
  message.Set("id", next_command_id_++);
  message.Set("method", method);
  message.Set("params", params.Clone());

  std::string json;
  base::JSONWriter::Write(message, &json);
  host_->DispatchProtocolMessage(this, base::as_byte_span(json));
}

void AbpCdpEventClient::DispatchProtocolMessage(
    content::DevToolsAgentHost* host,
    base::span<const uint8_t> message) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  std::string json(message.begin(), message.end());
  auto parsed = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    return;
  }

  const base::Value::Dict& dict = parsed->GetDict();

  // Check if this is an event (has "method" but no "id")
  const std::string* method = dict.FindString("method");
  if (!method || dict.FindInt("id").has_value()) {
    // This is a response to a command, not an event
    return;
  }

  // Get params
  const base::Value::Dict* params = dict.FindDict("params");
  base::Value::Dict empty_params;
  HandleEvent(*method, params ? *params : empty_params);
}

void AbpCdpEventClient::AgentHostClosed(content::DevToolsAgentHost* host) {
  host_ = nullptr;
}

void AbpCdpEventClient::HandleEvent(const std::string& method,
                                    const base::Value::Dict& params) {
  // Map CDP events to ABP event types
  std::string event_type;
  base::Value::Dict data;

  if (method == "Page.frameNavigated") {
    event_type = "navigation";
    const base::Value::Dict* frame = params.FindDict("frame");
    if (frame) {
      if (const std::string* url = frame->FindString("url")) {
        data.Set("url", *url);
      }
      if (const std::string* name = frame->FindString("name")) {
        data.Set("frameName", *name);
      }
    }
    if (const std::string* type = params.FindString("type")) {
      data.Set("transitionType", *type);
    }
  } else if (method == "Page.javascriptDialogOpening") {
    event_type = "dialog";
    if (const std::string* type = params.FindString("type")) {
      data.Set("dialogType", *type);
    }
    if (const std::string* message = params.FindString("message")) {
      data.Set("message", *message);
    }
    if (const std::string* url = params.FindString("url")) {
      data.Set("url", *url);
    }
    if (auto has_browser_handler = params.FindBool("hasBrowserHandler")) {
      data.Set("hasBrowserHandler", *has_browser_handler);
    }
  } else if (method == "Page.javascriptDialogClosed") {
    event_type = "dialog_closed";
    if (auto result = params.FindBool("result")) {
      data.Set("accepted", *result);
    }
    if (const std::string* user_input = params.FindString("userInput")) {
      data.Set("userInput", *user_input);
    }
  } else if (method == "Page.fileChooserOpened") {
    event_type = "file_chooser";
    if (const std::string* mode = params.FindString("mode")) {
      data.Set("mode", *mode);
    }
    if (auto multiple = params.FindBool("multipleFilesAllowed")) {
      data.Set("multipleFilesAllowed", *multiple);
    }
  } else if (method == "Page.downloadWillBegin") {
    event_type = "download_started";
    if (const std::string* url = params.FindString("url")) {
      data.Set("url", *url);
    }
    if (const std::string* suggested_filename =
            params.FindString("suggestedFilename")) {
      data.Set("suggestedFilename", *suggested_filename);
    }
    if (const std::string* guid = params.FindString("guid")) {
      data.Set("downloadId", *guid);
    }
  } else if (method == "Page.downloadProgress") {
    // Only record completion events
    const std::string* state = params.FindString("state");
    if (state && *state == "completed") {
      event_type = "download_completed";
      if (const std::string* guid = params.FindString("guid")) {
        data.Set("downloadId", *guid);
      }
      if (auto total_bytes = params.FindDouble("totalBytes")) {
        data.Set("totalBytes", *total_bytes);
      }
    } else if (state && *state == "canceled") {
      event_type = "download_cancelled";
      if (const std::string* guid = params.FindString("guid")) {
        data.Set("downloadId", *guid);
      }
    }
  } else if (method == "Target.targetCreated") {
    const base::Value::Dict* target_info = params.FindDict("targetInfo");
    if (target_info) {
      const std::string* type = target_info->FindString("type");
      if (type && *type == "page") {
        event_type = "popup";
        if (const std::string* target_id = target_info->FindString("targetId")) {
          data.Set("targetId", *target_id);
        }
        if (const std::string* url = target_info->FindString("url")) {
          data.Set("url", *url);
        }
        if (const std::string* opener_id =
                target_info->FindString("openerId")) {
          data.Set("openerId", *opener_id);
        }
      }
    }
  } else if (method == "Target.targetDestroyed") {
    // Tab closed - we get the target ID
    event_type = "tab_closed";
    if (const std::string* target_id = params.FindString("targetId")) {
      data.Set("closedTabId", *target_id);
    }
  } else if (method == "Page.domContentEventFired" ||
             method == "Page.loadEventFired") {
    // These can be useful but are very noisy - skip for now
    return;
  }

  // Skip if we didn't map the event
  if (event_type.empty()) {
    return;
  }

  // Apply rate limiting for scroll events
  if (event_type == "scroll" && !ShouldRateLimitScroll()) {
    return;
  }

  // Invoke callback
  callback_.Run(tab_id_, event_type, data);
}

bool AbpCdpEventClient::ShouldRateLimitScroll() {
  base::TimeTicks now = base::TimeTicks::Now();
  if ((now - last_scroll_time_).InMilliseconds() < kScrollRateLimitMs) {
    return false;  // Should skip this event
  }
  last_scroll_time_ = now;
  return true;  // OK to process
}

// AbpEventObserver implementation

AbpEventObserver::AbpEventObserver(AbpHistoryController* history_controller)
    : history_controller_(history_controller) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
}

AbpEventObserver::~AbpEventObserver() {
  Stop();
}

void AbpEventObserver::Start() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (observing_) {
    return;
  }

  content::DevToolsAgentHost::AddObserver(this);
  observing_ = true;

  // Attach to existing tabs
  for (const auto& host : content::DevToolsAgentHost::GetOrCreateAll()) {
    if (host->GetType() == content::DevToolsAgentHost::kTypePage) {
      AttachToHost(host.get());
    }
  }

  LOG(INFO) << "ABP: Event observer started";
}

void AbpEventObserver::Stop() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (!observing_) {
    return;
  }

  content::DevToolsAgentHost::RemoveObserver(this);
  observing_ = false;

  // Clear all event clients
  event_clients_.clear();

  LOG(INFO) << "ABP: Event observer stopped";
}

void AbpEventObserver::DevToolsAgentHostCreated(
    content::DevToolsAgentHost* host) {
  if (host->GetType() == content::DevToolsAgentHost::kTypePage) {
    AttachToHost(host);
  }
}

void AbpEventObserver::DevToolsAgentHostAttached(
    content::DevToolsAgentHost* host) {
  // Another client attached - we might already be attached
}

void AbpEventObserver::DevToolsAgentHostNavigated(
    content::DevToolsAgentHost* host) {
  // Navigation handled via CDP Page.frameNavigated event
}

void AbpEventObserver::DevToolsAgentHostDetached(
    content::DevToolsAgentHost* host) {
  // Remove our event client for this host
  event_clients_.erase(host->GetId());
}

void AbpEventObserver::DevToolsAgentHostCrashed(
    content::DevToolsAgentHost* host,
    base::TerminationStatus status) {
  // Record crash as an event
  base::Value::Dict data;
  data.Set("status", static_cast<int>(status));

  if (history_controller_) {
    history_controller_->RecordEvent(host->GetId(), "tab_crashed", data);
  }

  // Remove our event client
  event_clients_.erase(host->GetId());
}

bool AbpEventObserver::ShouldForceDevToolsAgentHostCreation() {
  // Return true to ensure we get notified about all page targets
  return true;
}

void AbpEventObserver::AttachToHost(content::DevToolsAgentHost* host) {
  const std::string& id = host->GetId();

  // Check if already attached
  if (event_clients_.count(id)) {
    return;
  }

  // Create event client
  auto client = std::make_unique<AbpCdpEventClient>(
      host, base::BindRepeating(&AbpEventObserver::OnEvent,
                                weak_factory_.GetWeakPtr()));

  client->EnableEventCapture();

  event_clients_[id] = std::move(client);
}

void AbpEventObserver::OnEvent(const std::string& tab_id,
                               const std::string& event_type,
                               const base::Value::Dict& data) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (history_controller_) {
    history_controller_->RecordEvent(tab_id, event_type, data);
  }
}

}  // namespace abp
