#include "chrome/browser/abp/abp_action_context.h"

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "chrome/browser/abp/abp_controller.h"
#include "chrome/browser/abp/abp_event_collector.h"
#include "chrome/browser/abp/abp_history_controller.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"

namespace abp {

// static
void AbpActionContext::Run(AbpController* controller,
                           const std::string& tab_id,
                           const std::string& action_type,
                           const base::Value::Dict& params,
                           ActionCallback action,
                           ResponseCallback response) {
  RunWithOptions(controller, tab_id, action_type, params, Options(),
                 std::move(action), std::move(response));
}

// static
void AbpActionContext::RunWithOptions(AbpController* controller,
                                      const std::string& tab_id,
                                      const std::string& action_type,
                                      const base::Value::Dict& params,
                                      const Options& options,
                                      ActionCallback action,
                                      ResponseCallback response) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  auto ctx = base::MakeRefCounted<AbpActionContext>(
      controller, tab_id, action_type, params, options, std::move(action),
      std::move(response));
  ctx->Start();
}

AbpActionContext::AbpActionContext(AbpController* controller,
                                   const std::string& tab_id,
                                   const std::string& action_type,
                                   const base::Value::Dict& params,
                                   const Options& options,
                                   ActionCallback action,
                                   ResponseCallback response)
    : controller_(controller),
      tab_id_(tab_id),
      action_type_(action_type),
      params_(params.Clone()),
      options_(options),
      action_(std::move(action)),
      response_callback_(std::move(response)) {}

AbpActionContext::~AbpActionContext() = default;

void AbpActionContext::Start() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // Hold a self-reference to prevent destruction during async operations
  prevent_destroy_ = this;

  start_time_ms_ = base::Time::Now().InMillisecondsSinceUnixEpoch();
  start_ticks_ = base::TimeTicks::Now();

  // Capture virtual time at start
  virtual_time_at_start_ = controller_->GetVirtualTimeMs(tab_id_);

  // Validate tab exists
  web_contents_ = controller_->FindWebContents(tab_id_);
  if (!web_contents_) {
    SendErrorResponse(404, "TAB_NOT_FOUND", "Tab not found");
    return;
  }

  // Get CDP client
  client_ = controller_->GetOrCreateCdpClient(web_contents_);
  if (!client_) {
    SendErrorResponse(500, "CDP_ERROR", "Failed to create CDP client");
    return;
  }

  // Start event capture
  StartEventCapture();

  // Start the flow: resume execution first
  ResumeExecutionIfNeeded();
}

void AbpActionContext::StartEventCapture() {
  if (controller_->event_collector()) {
    controller_->event_collector()->StartCapturing(tab_id_);
  }
}

void AbpActionContext::ResumeExecutionIfNeeded() {
  // Check if execution control should be skipped for this action
  if (options_.skip_execution_control) {
    OnExecutionResumed();
    return;
  }

  // Check if execution control is enabled for this tab
  if (!controller_->IsExecutionControlEnabled()) {
    OnExecutionResumed();
    return;
  }

  // Use the controller's ResumeExecution which handles state tracking
  controller_->ResumeExecution(
      tab_id_,
      base::BindOnce(&AbpActionContext::OnExecutionResumed,
                     weak_factory_.GetWeakPtr()));
}

void AbpActionContext::OnExecutionResumed() {
  if (has_error_) {
    return;
  }

  // Capture "before" screenshot
  CaptureBeforeScreenshot();
}

void AbpActionContext::CaptureBeforeScreenshot() {
  controller_->CaptureScreenshotForHistory(
      tab_id_, start_time_ms_, true,
      base::BindOnce(&AbpActionContext::OnBeforeScreenshotCaptured,
                     weak_factory_.GetWeakPtr()));
}

void AbpActionContext::OnBeforeScreenshotCaptured(std::string screenshot_path) {
  screenshot_before_path_ = std::move(screenshot_path);

  if (has_error_) {
    return;
  }

  // Execute the action
  ExecuteAction();
}

void AbpActionContext::ExecuteAction() {
  // Invoke the user-provided action callback
  // The action should call OnActionDispatched() when done
  if (action_) {
    std::move(action_).Run(this);
  } else {
    // No action provided, go directly to wait
    OnActionDispatched();
  }
}

void AbpActionContext::OnActionDispatched() {
  action_end_ticks_ = base::TimeTicks::Now();

  if (has_error_) {
    return;
  }

  // Start wait_until handling
  DoWaitUntil();
}

void AbpActionContext::OnActionError(const std::string& error_code,
                                     const std::string& error_message) {
  has_error_ = true;
  error_code_ = error_code;
  error_message_ = error_message;

  // Still need to pause and record before sending error response
  PauseExecutionIfNeeded();
}

void AbpActionContext::SetResult(base::Value::Dict result) {
  result_ = std::move(result);
}

void AbpActionContext::DoWaitUntil() {
  // If execution control is enabled (and not skipped for this action),
  // we skip the WaitForActionComplete because we'll pause execution
  // immediately (freezing the page state). Virtual time handles all timing.
  if (!options_.skip_execution_control &&
      controller_->IsExecutionControlEnabled()) {
    OnWaitUntilComplete();
    return;
  }

  // Without execution control, use the standard wait mechanism
  controller_->WaitForActionComplete(
      tab_id_,
      base::BindOnce(&AbpActionContext::OnWaitUntilComplete,
                     weak_factory_.GetWeakPtr()));
}

void AbpActionContext::OnWaitUntilComplete() {
  wait_completed_ms_ = base::Time::Now().InMillisecondsSinceUnixEpoch();

  // Stop event capture and get scroll position
  StopEventCaptureAndGetScrollPosition();
}

void AbpActionContext::StopEventCaptureAndGetScrollPosition() {
  // Stop event capture and save events
  if (controller_->event_collector()) {
    captured_events_ = controller_->event_collector()->StopCapturing();
  }

  // Get scroll position
  controller_->GetScrollPosition(
      tab_id_,
      base::BindOnce(&AbpActionContext::OnScrollPositionReceived,
                     weak_factory_.GetWeakPtr()));
}

void AbpActionContext::OnScrollPositionReceived(base::Value::Dict scroll_info) {
  scroll_info_ = std::move(scroll_info);

  // Pause execution (freezes V8 + virtual time)
  PauseExecutionIfNeeded();
}

void AbpActionContext::PauseExecutionIfNeeded() {
  // Check if execution control should be skipped for this action
  if (options_.skip_execution_control) {
    OnExecutionPaused();
    return;
  }

  if (!controller_->IsExecutionControlEnabled()) {
    OnExecutionPaused();
    return;
  }

  controller_->PauseExecution(
      tab_id_,
      base::BindOnce(&AbpActionContext::OnExecutionPaused,
                     weak_factory_.GetWeakPtr()));
}

void AbpActionContext::OnExecutionPaused() {
  // Page is now frozen - capture screenshot
  CaptureAfterScreenshot();
}

void AbpActionContext::CaptureAfterScreenshot() {
  controller_->CaptureScreenshotForHistory(
      tab_id_, start_time_ms_, false,
      base::BindOnce(&AbpActionContext::OnAfterScreenshotCaptured,
                     weak_factory_.GetWeakPtr()));
}

void AbpActionContext::OnAfterScreenshotCaptured(std::string screenshot_path) {
  screenshot_after_path_ = std::move(screenshot_path);

  // Capture virtual time at end
  virtual_time_at_end_ = controller_->GetVirtualTimeMs(tab_id_);

  // Now capture base64 screenshot for response envelope
  CaptureScreenshotBase64();
}

void AbpActionContext::CaptureScreenshotBase64() {
  controller_->CaptureScreenshotBase64(
      tab_id_,
      base::BindOnce(&AbpActionContext::OnScreenshotBase64Captured,
                     weak_factory_.GetWeakPtr()));
}

void AbpActionContext::OnScreenshotBase64Captured(std::string base64,
                                                   int width,
                                                   int height) {
  screenshot_base64_ = std::move(base64);
  screenshot_width_ = width;
  screenshot_height_ = height;

  // Record to history
  RecordHistory(!has_error_, error_code_, error_message_);

  // Build full response envelope and send
  if (has_error_) {
    // For errors, send an error response but still include any partial result
    SendErrorResponse(500, error_code_, error_message_);
  } else {
    BuildResponseEnvelope();
    SendResponse();
  }
}

void AbpActionContext::RecordHistory(bool success,
                                     const std::string& error_code,
                                     const std::string& error_message) {
  if (!controller_->history_controller_) {
    return;
  }

  int64_t end_time_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
  int64_t duration_ms = end_time_ms - start_time_ms_;

  base::Value result_value(result_.Clone());
  controller_->history_controller_->RecordAction(
      tab_id_, action_type_, params_, &result_value, success, error_code,
      error_message, start_time_ms_, duration_ms, screenshot_before_path_,
      screenshot_after_path_);
}

void AbpActionContext::BuildResponseEnvelope() {
  // The response envelope is built in SendResponse()
  // This method is a placeholder for any pre-processing
}

void AbpActionContext::SendResponse() {
  if (!response_callback_) {
    return;
  }

  // Build full response envelope
  base::Value::Dict envelope;

  // 1. Add action result
  envelope.Set("result", std::move(result_));

  // 2. Add screenshot
  if (!screenshot_base64_.empty()) {
    base::Value::Dict screenshot;
    screenshot.Set("data", screenshot_base64_);
    screenshot.Set("width", screenshot_width_);
    screenshot.Set("height", screenshot_height_);
    screenshot.Set("virtual_time_ms", static_cast<double>(virtual_time_at_end_));
    screenshot.Set("format", "webp");
    envelope.Set("screenshot", std::move(screenshot));
  }

  // 3. Add scroll position
  if (!scroll_info_.empty()) {
    envelope.Set("scroll", std::move(scroll_info_));
  }

  // 4. Add captured events
  base::Value::List events_list;
  for (auto& event : captured_events_) {
    base::Value::Dict event_dict;
    event_dict.Set("type", event.type);
    event_dict.Set("virtual_time_ms", static_cast<double>(event.virtual_time_ms));
    event_dict.Set("data", std::move(event.data));
    events_list.Append(std::move(event_dict));
  }
  envelope.Set("events", std::move(events_list));

  // 5. Add timing info
  base::Value::Dict timing;
  timing.Set("action_started_ms", static_cast<double>(start_time_ms_));
  timing.Set("action_completed_ms",
             static_cast<double>(start_time_ms_ +
                 (action_end_ticks_ - start_ticks_).InMilliseconds()));
  timing.Set("wait_completed_ms", static_cast<double>(wait_completed_ms_));
  timing.Set("duration_ms",
             static_cast<int>(wait_completed_ms_ - start_time_ms_));
  envelope.Set("timing", std::move(timing));

  // 6. Add virtual time info if execution control is enabled
  if (controller_->IsExecutionControlEnabled()) {
    auto& states = controller_->execution_states_;
    auto it = states.find(tab_id_);
    if (it != states.end()) {
      base::Value::Dict virtual_time;
      virtual_time.Set("paused", it->second.paused);
      virtual_time.Set("base_ticks_ms", it->second.virtual_time_base_ticks_ms);
      envelope.Set("virtual_time", std::move(virtual_time));
    }
  }

  controller_->SendJson(200, base::Value(std::move(envelope)),
                        std::move(response_callback_));

  // Clear self-reference to allow destruction
  prevent_destroy_ = nullptr;
}

void AbpActionContext::SendErrorResponse(int status,
                                         const std::string& error_code,
                                         const std::string& error_message) {
  if (!response_callback_) {
    return;
  }

  // Record the error in history if we have enough context
  if (!has_error_) {
    has_error_ = true;
    error_code_ = error_code;
    error_message_ = error_message;
    RecordHistory(false, error_code, error_message);
  }

  controller_->SendError(status, error_message, std::move(response_callback_));

  // Clear self-reference to allow destruction
  prevent_destroy_ = nullptr;
}

}  // namespace abp
