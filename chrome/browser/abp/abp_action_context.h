#ifndef CHROME_BROWSER_ABP_ABP_ACTION_CONTEXT_H_
#define CHROME_BROWSER_ABP_ABP_ACTION_CONTEXT_H_

#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/abp/abp_event_collector.h"

namespace content {
class WebContents;
}

namespace abp {

class AbpController;
class AbpCdpClient;

// Callback signature for response (content_type for JSON vs binary responses)
using ResponseCallback = base::OnceCallback<void(int status,
                                                  const std::string& content_type,
                                                  std::string body)>;

// Forward declaration for the action callback
class AbpActionContext;
using ActionCallback = base::OnceCallback<void(AbpActionContext* ctx)>;

// AbpActionContext wraps any state-modifying ABP action with consistent:
// - Execution resume at start (Debugger.resume + virtual time advance)
// - Action execution
// - wait_until handling
// - Execution pause after completion (virtual time pause + Debugger.pause)
// - Screenshot capture (page is frozen)
// - Response formatting (timing + virtual_time info)
//
// Flow:
//   Run()
//     -> ResumeExecutionIfNeeded()
//     -> OnExecutionResumed()
//     -> CaptureBeforeScreenshot()
//     -> OnBeforeScreenshotCaptured()
//     -> ExecuteAction() (calls user-provided ActionCallback)
//     -> [action calls OnActionDispatched()]
//     -> WaitUntil() (handles wait_until from params)
//     -> OnWaitUntilComplete()
//     -> PauseExecutionIfNeeded() (virtual time pause + Debugger.pause)
//     -> OnExecutionPaused()
//     -> CaptureAfterScreenshot() (page is frozen)
//     -> OnAfterScreenshotCaptured()
//     -> RecordHistory()
//     -> SendResponse()
//
class AbpActionContext : public base::RefCounted<AbpActionContext> {
 public:
  // Options for running an action
  struct Options {
    // If true, skip resume/pause execution control.
    // Use for actions like Navigate that need JS to run during the action.
    bool skip_execution_control = false;
  };

  // Factory method - creates context and starts the action flow
  // The action callback will be invoked after execution is resumed.
  // The action should call ctx->OnActionDispatched() when the core
  // action logic is complete (e.g., after mouse release for click).
  static void Run(AbpController* controller,
                  const std::string& tab_id,
                  const std::string& action_type,
                  const base::Value::Dict& params,
                  ActionCallback action,
                  ResponseCallback response);

  // Factory method with options
  static void RunWithOptions(AbpController* controller,
                             const std::string& tab_id,
                             const std::string& action_type,
                             const base::Value::Dict& params,
                             const Options& options,
                             ActionCallback action,
                             ResponseCallback response);

  // Called by action implementation when action dispatch is complete.
  // This triggers the wait_until -> pause -> screenshot -> response flow.
  void OnActionDispatched();

  // Called by action implementation to set result data that will be
  // included in the response.
  void SetResult(base::Value::Dict result);

  // Called by action implementation on error.
  // This immediately sends an error response and stops the flow.
  void OnActionError(const std::string& error_code,
                     const std::string& error_message);

  // Access to CDP client for action implementation
  AbpCdpClient* client() const { return client_; }

  // Access to WebContents for action implementation
  content::WebContents* web_contents() const { return web_contents_; }

  // Access to params for action implementation
  const base::Value::Dict& params() const { return params_; }

  // Access to tab_id for action implementation
  const std::string& tab_id() const { return tab_id_; }

  // Access to controller for action implementation
  // Use sparingly - prefer using context methods when possible
  AbpController* controller() const { return controller_; }

  // Constructor - prefer using Run() or RunWithOptions() factory methods
  AbpActionContext(AbpController* controller,
                   const std::string& tab_id,
                   const std::string& action_type,
                   const base::Value::Dict& params,
                   const Options& options,
                   ActionCallback action,
                   ResponseCallback response);

 private:
  friend class base::RefCounted<AbpActionContext>;
  ~AbpActionContext();

  // Internal flow methods
  void Start();
  void StartEventCapture();
  void ResumeExecutionIfNeeded();
  void OnExecutionResumed();
  void CaptureBeforeScreenshot();
  void OnBeforeScreenshotCaptured(std::string screenshot_path);
  void ExecuteAction();
  void DoWaitUntil();
  void OnWaitUntilComplete();
  void StopEventCaptureAndGetScrollPosition();
  void OnScrollPositionReceived(base::Value::Dict scroll_info);
  void PauseExecutionIfNeeded();
  void OnExecutionPaused();
  void CaptureAfterScreenshot();
  void OnAfterScreenshotCaptured(std::string screenshot_path);
  void CaptureScreenshotBase64();
  void OnScreenshotBase64Captured(std::string base64, int width, int height);
  void RecordHistory(bool success,
                     const std::string& error_code,
                     const std::string& error_message);
  void BuildResponseEnvelope();
  void SendResponse();
  void SendErrorResponse(int status,
                         const std::string& error_code,
                         const std::string& error_message);

  // State
  raw_ptr<AbpController> controller_;
  std::string tab_id_;
  std::string action_type_;
  base::Value::Dict params_;
  Options options_;
  ActionCallback action_;
  ResponseCallback response_callback_;
  raw_ptr<AbpCdpClient> client_ = nullptr;
  raw_ptr<content::WebContents> web_contents_ = nullptr;
  base::Value::Dict result_;

  // Screenshots
  std::string screenshot_before_path_;
  std::string screenshot_after_path_;

  // Timing
  int64_t start_time_ms_ = 0;
  base::TimeTicks start_ticks_;
  base::TimeTicks action_end_ticks_;

  // Virtual time info (captured at start and end)
  double virtual_time_at_start_ = 0;
  double virtual_time_at_end_ = 0;

  // Response envelope data
  std::vector<AbpEvent> captured_events_;
  base::Value::Dict scroll_info_;
  std::string screenshot_base64_;
  int screenshot_width_ = 0;
  int screenshot_height_ = 0;
  int64_t wait_completed_ms_ = 0;

  // Error state
  bool has_error_ = false;
  std::string error_code_;
  std::string error_message_;

  // Self-reference to prevent destruction during async operations.
  // Set in Start(), cleared in SendResponse()/SendErrorResponse().
  scoped_refptr<AbpActionContext> prevent_destroy_;

  base::WeakPtrFactory<AbpActionContext> weak_factory_{this};
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_ACTION_CONTEXT_H_
