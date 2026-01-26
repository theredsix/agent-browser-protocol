#ifndef CHROME_BROWSER_ABP_ABP_CONTROLLER_H_
#define CHROME_BROWSER_ABP_ABP_CONTROLLER_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "chrome/browser/abp/abp_types.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-forward.h"

namespace content {
class WebContents;
}

namespace abp {

class AbpActionContext;
class AbpDownloadObserver;
class AbpEventCollector;
class AbpHistoryController;

// Key information for CDP Input.dispatchKeyEvent
struct KeyInfo {
  KeyInfo();
  KeyInfo(const std::string& key,
          const std::string& code,
          int windows_virtual_key,
          int native_virtual_key,
          bool is_modifier,
          int modifier_flag);
  ~KeyInfo();
  KeyInfo(const KeyInfo&);
  KeyInfo& operator=(const KeyInfo&);

  std::string key;              // CDP key value (e.g., "Enter", "a")
  std::string code;             // CDP code value (e.g., "Enter", "KeyA")
  int windows_virtual_key = 0;  // windowsVirtualKeyCode
  int native_virtual_key = 0;   // nativeVirtualKeyCode (same as windows on most platforms)
  bool is_modifier = false;     // true for Shift, Control, Alt, Meta
  int modifier_flag = 0;        // Modifier bitmask (1=Alt, 2=Ctrl, 4=Meta, 8=Shift)
};

// Get key info for CDP key event dispatch
// Returns info for the given key name (case-sensitive for letters)
KeyInfo GetKeyInfo(const std::string& key_name);

// Convert modifier names to CDP modifier bitmask
// Modifier names: "Alt", "Control", "Meta", "Shift"
int ModifiersToFlags(const std::vector<std::string>& modifiers);

// Context for recording actions with history
struct ActionContext {
  ActionContext();
  ~ActionContext();
  ActionContext(const ActionContext&) = delete;
  ActionContext& operator=(const ActionContext&) = delete;
  ActionContext(ActionContext&&);
  ActionContext& operator=(ActionContext&&);

  std::string tab_id;
  std::string action_type;
  base::Value::Dict params;
  int64_t start_time = 0;
  std::string screenshot_before_path;
};

// CDP client for sending commands and receiving responses/events
class AbpCdpClient : public content::DevToolsAgentHostClient {
 public:
  using CdpCallback = base::OnceCallback<void(bool success,
                                               const std::string& result)>;
  using EventCallback = base::RepeatingCallback<void(const std::string& method,
                                                      const base::Value::Dict& params)>;

  explicit AbpCdpClient(scoped_refptr<content::DevToolsAgentHost> host);
  ~AbpCdpClient() override;

  AbpCdpClient(const AbpCdpClient&) = delete;
  AbpCdpClient& operator=(const AbpCdpClient&) = delete;

  // Send a CDP command and receive response via callback
  void SendCommand(const std::string& method,
                   const base::Value::Dict& params,
                   CdpCallback callback);

  // Set event listener for CDP events (replaces any existing listener)
  void SetEventListener(EventCallback callback);

  // Clear event listener
  void ClearEventListener();

  // DevToolsAgentHostClient implementation
  void DispatchProtocolMessage(content::DevToolsAgentHost* host,
                               base::span<const uint8_t> message) override;
  void AgentHostClosed(content::DevToolsAgentHost* host) override;

 private:
  scoped_refptr<content::DevToolsAgentHost> host_;
  int next_command_id_ = 1;
  std::map<int, CdpCallback> pending_callbacks_;
  EventCallback event_listener_;
  base::WeakPtrFactory<AbpCdpClient> weak_factory_{this};
};

// Handles ABP REST API requests on the UI thread.
// Provides direct access to browser windows and tabs.
class AbpController {
  // AbpActionContext needs access to execution control, history, and
  // screenshot methods to implement the unified action flow.
  friend class AbpActionContext;

 public:
  AbpController();
  ~AbpController();

  AbpController(const AbpController&) = delete;
  AbpController& operator=(const AbpController&) = delete;

  // Screenshot options struct (public for lambda access)
  struct ScreenshotOptions {
    ScreenshotOptions();
    ~ScreenshotOptions();
    ScreenshotOptions(const ScreenshotOptions&);
    ScreenshotOptions& operator=(const ScreenshotOptions&);

    std::string format = "png";      // png, jpeg, webp
    int quality = 80;                // 1-100 for jpeg/webp
    std::string markup = "none";     // none, interactive, clickable, typeable, inputs
    std::string mouse = "normal";    // normal, none, large
    bool cursor = true;              // Include virtual cursor in screenshot
  };

  // Set the history controller for action recording
  void SetHistoryController(AbpHistoryController* history_controller);

  // Set download observer (called by AbpHttpServer)
  void SetDownloadObserver(AbpDownloadObserver* observer);

  // Route incoming HTTP request to appropriate handler
  void HandleRequest(const std::string& method,
                     const std::string& path,
                     const std::string& body,
                     ResponseCallback callback);

  // Center the mouse cursor in the active tab's viewport
  // Used for ABP input-only mode initialization
  void CenterMouseInActiveTab();

  // Check if browser is ready for ABP operations
  // Returns true if there's a browser window with a tab that has a valid view
  bool IsBrowserReady();

  // Get browser status for /api/v1/browser/status endpoint
  void GetBrowserStatus(ResponseCallback callback);

  // Helpers (public for use in lambdas)
  content::WebContents* FindWebContents(const std::string& tab_id);
  AbpCdpClient* GetOrCreateCdpClient(content::WebContents* wc);
  void SendError(int status,
                 const std::string& error,
                 ResponseCallback callback);
  void SendJson(int status,
                base::Value value,
                ResponseCallback callback);

  // Check if execution control is enabled (via command-line flag)
  bool IsExecutionControlEnabled() const;

  // Resume execution before action (Debugger.resume + virtual time advance)
  void ResumeExecution(const std::string& tab_id, base::OnceClosure then);

  // Pause execution after action (virtual time pause + Debugger.pause)
  void PauseExecution(const std::string& tab_id, base::OnceClosure then);

  // Wait for action_complete conditions before calling callback
  void WaitForActionComplete(const std::string& tab_id,
                             base::OnceClosure on_complete);

  // Take screenshot for history (before or after action)
  // Uses direct C++ capture with CopyFromSurface
  // Note: Cursor is rendered by virtual cursor overlay and captured automatically
  void CaptureScreenshotForHistory(
      const std::string& tab_id,
      int64_t timestamp,
      bool is_before,
      base::OnceCallback<void(std::string path)> callback);

  // Update the virtual cursor state for a tab (used by input actions)
  void UpdateVirtualCursorState(const std::string& tab_id, double x, double y);

  // Get virtual time for a tab, or wall clock if not enabled
  int64_t GetVirtualTimeMs(const std::string& tab_id);

  // Get the event collector (for AbpActionContext)
  AbpEventCollector* event_collector() { return event_collector_.get(); }

  // Called by event collector when a file chooser is opened
  void OnFileChooserOpened(const std::string& chooser_id,
                           const std::string& tab_id,
                           base::Value::Dict info);

  // Get scroll position for a tab via CDP (async)
  void GetScrollPosition(
      const std::string& tab_id,
      base::OnceCallback<void(base::Value::Dict)> callback);

  // Capture screenshot and return base64 data (for response envelope)
  void CaptureScreenshotBase64(
      const std::string& tab_id,
      base::OnceCallback<void(std::string base64, int width, int height)> callback);

 private:
  // Tab operations
  void ListTabs(ResponseCallback callback);
  void GetTab(const std::string& tab_id, ResponseCallback callback);
  void CreateTab(const base::Value::Dict& params, ResponseCallback callback);
  void CloseTab(const std::string& tab_id, ResponseCallback callback);

  // Navigation
  void Navigate(const std::string& tab_id,
                const base::Value::Dict& params,
                ResponseCallback callback);
  void OnNavigateBeforeScreenshot(const std::string& tab_id,
                                  const std::string& url,
                                  std::unique_ptr<ActionContext> context,
                                  ResponseCallback callback,
                                  std::string screenshot_before_path);
  void DispatchNavigateEvent(const std::string& tab_id,
                             const std::string& url,
                             std::unique_ptr<ActionContext> context,
                             ResponseCallback callback);
  void OnNavigateAfterScreenshot(const std::string& url,
                                 std::unique_ptr<ActionContext> context,
                                 ResponseCallback callback,
                                 std::string screenshot_after_path);
  void Reload(const std::string& tab_id, ResponseCallback callback);
  void OnReloadBeforeScreenshot(const std::string& tab_id,
                                std::unique_ptr<ActionContext> context,
                                ResponseCallback callback,
                                std::string screenshot_before_path);
  void OnReloadAfterScreenshot(std::unique_ptr<ActionContext> context,
                               ResponseCallback callback,
                               std::string screenshot_after_path);
  void GoBack(const std::string& tab_id, ResponseCallback callback);
  void OnGoBackBeforeScreenshot(const std::string& tab_id,
                                std::unique_ptr<ActionContext> context,
                                ResponseCallback callback,
                                std::string screenshot_before_path);
  void OnGoBackAfterScreenshot(std::unique_ptr<ActionContext> context,
                               ResponseCallback callback,
                               std::string screenshot_after_path);
  void GoForward(const std::string& tab_id, ResponseCallback callback);
  void OnGoForwardBeforeScreenshot(const std::string& tab_id,
                                   std::unique_ptr<ActionContext> context,
                                   ResponseCallback callback,
                                   std::string screenshot_before_path);
  void OnGoForwardAfterScreenshot(std::unique_ptr<ActionContext> context,
                                  ResponseCallback callback,
                                  std::string screenshot_after_path);

  // Content
  void Screenshot(const std::string& tab_id,
                  const base::Value::Dict& params,
                  ResponseCallback callback);
  void ExecuteScript(const std::string& tab_id,
                     const base::Value::Dict& params,
                     ResponseCallback callback);

  // Input
  void Click(const std::string& tab_id,
             const base::Value::Dict& params,
             ResponseCallback callback);
  void Type(const std::string& tab_id,
            const base::Value::Dict& params,
            ResponseCallback callback);
  void Move(const std::string& tab_id,
            const base::Value::Dict& params,
            ResponseCallback callback);
  void Scroll(const std::string& tab_id,
              const base::Value::Dict& params,
              ResponseCallback callback);
  void KeyPress(const std::string& tab_id,
                const base::Value::Dict& params,
                ResponseCallback callback);
  void KeyDown(const std::string& tab_id,
               const base::Value::Dict& params,
               ResponseCallback callback);
  void KeyUp(const std::string& tab_id,
             const base::Value::Dict& params,
             ResponseCallback callback);

  // Tab control
  void ActivateTab(const std::string& tab_id, ResponseCallback callback);
  void StopLoading(const std::string& tab_id, ResponseCallback callback);

  // Utility actions
  void Wait(const std::string& tab_id,
            const base::Value::Dict& params,
            ResponseCallback callback);

  // File chooser endpoint
  void HandleFileChooser(const std::string& chooser_id,
                         const base::Value::Dict& params,
                         ResponseCallback callback);

  // Binary screenshot (GET endpoint - returns raw WebP)
  void BinaryScreenshot(const std::string& tab_id,
                        const std::string& query,
                        ResponseCallback callback);

  // Browser shutdown
  void ShutdownBrowser(const base::Value::Dict& params,
                       ResponseCallback callback);

  // Download endpoints
  void ListDownloads(const std::string& query, ResponseCallback callback);
  void GetDownloadStatus(const std::string& download_id,
                         ResponseCallback callback);
  void CancelDownload(const std::string& download_id,
                      ResponseCallback callback);

  // CDP callbacks
  void OnScreenshotResult(ResponseCallback callback,
                          const ScreenshotOptions& options,
                          bool success,
                          const std::string& result);
  void OnMarkupInjected(const std::string& tab_id,
                        ResponseCallback callback,
                        const ScreenshotOptions& options,
                        bool success,
                        const std::string& result);
  void OnCursorSetForScreenshot(const std::string& tab_id,
                                base::Value::Dict params,
                                ResponseCallback callback,
                                const ScreenshotOptions& options,
                                content::WebContents* wc,
                                bool success,
                                const std::string& result);
  void CaptureScreenshotWithCursor(const std::string& tab_id,
                                   ResponseCallback callback,
                                   const ScreenshotOptions& options);
  void OnCursorScreenshotCaptured(const std::string& tab_id,
                                  ResponseCallback callback,
                                  const ScreenshotOptions& options,
                                  const content::CopyFromSurfaceResult& result);
  void OnExecuteScriptResult(ResponseCallback callback,
                             bool success,
                             const std::string& result);
  void OnClickBeforeScreenshot(const std::string& tab_id,
                               double x,
                               double y,
                               std::unique_ptr<ActionContext> context,
                               ResponseCallback callback,
                               std::string screenshot_before_path);
  void DispatchClickEvent(const std::string& tab_id,
                          double x,
                          double y,
                          std::unique_ptr<ActionContext> context,
                          ResponseCallback callback);
  void OnClickPressedResult(const std::string& tab_id,
                            double x,
                            double y,
                            std::unique_ptr<ActionContext> context,
                            ResponseCallback callback,
                            bool success,
                            const std::string& result);
  void OnClickResult(std::unique_ptr<ActionContext> context,
                     ResponseCallback callback,
                     bool success,
                     const std::string& result);
  void OnClickAfterScreenshot(std::unique_ptr<ActionContext> context,
                              ResponseCallback callback,
                              std::string screenshot_after_path);
  void OnTypeBeforeScreenshot(const std::string& tab_id,
                              const std::string& text,
                              std::unique_ptr<ActionContext> context,
                              ResponseCallback callback,
                              std::string screenshot_before_path);
  void DispatchTypeEvent(const std::string& tab_id,
                         const std::string& text,
                         std::unique_ptr<ActionContext> context,
                         ResponseCallback callback);
  void OnTypeResult(std::unique_ptr<ActionContext> context,
                    ResponseCallback callback,
                    bool success,
                    const std::string& result);
  void OnTypeAfterScreenshot(std::unique_ptr<ActionContext> context,
                             ResponseCallback callback,
                             std::string screenshot_after_path);

  // Centralized after-action handler: wait for action_complete, take screenshot,
  // record to history, and send response. This is the single entry point for
  // completing any action that modifies page state.
  //
  // Parameters:
  //   tab_id: The tab where the action was performed
  //   context: Action context with start_time, params, screenshot_before_path
  //   result: The result dict to include in the response
  //   callback: Response callback to send the final response
  void CompleteActionWithScreenshot(
      const std::string& tab_id,
      std::unique_ptr<ActionContext> context,
      base::Value::Dict result,
      ResponseCallback callback);

  // Internal callback after wait completes
  void OnWaitCompleteForAction(
      const std::string& tab_id,
      std::unique_ptr<ActionContext> context,
      base::Value::Dict result,
      ResponseCallback callback);

  // Internal callback after screenshot captured
  void OnActionScreenshotCaptured(
      std::unique_ptr<ActionContext> context,
      base::Value::Dict result,
      ResponseCallback callback,
      std::string screenshot_after_path);

  // Direct screenshot capture using CopyFromSurface (no CDP/JS injection)
  // Note: Cursor is rendered by virtual cursor overlay and captured automatically
  void CaptureScreenshotDirect(
      content::WebContents* web_contents,
      const base::FilePath& screenshot_path,
      base::OnceCallback<void(std::string path)> callback);

  // Callback when surface copy completes
  void OnSurfaceCopied(
      const base::FilePath& screenshot_path,
      base::OnceCallback<void(std::string path)> callback,
      const content::CopyFromSurfaceResult& result);

  // Legacy CDP-based capture methods (kept for API screenshot endpoint)
  void OnHistoryMarkupInjected(
      const std::string& tab_id,
      const base::FilePath& screenshot_path,
      base::OnceCallback<void(std::string path)> callback,
      bool success,
      const std::string& result);
  void OnHistoryScreenshotCaptured(
      const std::string& tab_id,
      const base::FilePath& screenshot_path,
      base::OnceCallback<void(std::string path)> callback,
      bool success,
      const std::string& result);

  // Record completed action to history
  void RecordCompletedAction(const ActionContext& context,
                             const base::Value* result,
                             bool success,
                             const std::string& error_code,
                             const std::string& error_message,
                             const std::string& screenshot_after_path);

  // CDP clients per WebContents (keyed by DevToolsAgentHost ID)
  std::map<std::string, std::unique_ptr<AbpCdpClient>> cdp_clients_;

  // Sets the virtual cursor position via Mojo IPC to the renderer.
  // This is the primary path for cursor rendering in the compositor layer.
  void SetVirtualCursorViaMojo(content::WebContents* wc,
                                float x,
                                float y,
                                bool visible);

  // Sets the virtual cursor type via Mojo IPC.
  void SetVirtualCursorTypeViaMojo(content::WebContents* wc,
                                    ui::mojom::CursorType cursor_type);

  // Enables/disables the virtual cursor system for a tab.
  void SetVirtualCursorEnabledViaMojo(content::WebContents* wc, bool enabled);

  // Virtual cursor state per tab (keyed by DevToolsAgentHost ID)
  // When a virtual cursor is active via CDP overlay, we track it here
  // so screenshots don't double-draw the cursor
  struct VirtualCursorState {
    bool active = false;
    double x = 0;
    double y = 0;
    ui::mojom::CursorType cursor_type = ui::mojom::CursorType::kPointer;
  };
  std::map<std::string, VirtualCursorState> virtual_cursor_states_;

  // Execution state for V8 virtual clock + debugger pause
  struct ExecutionState {
    bool debugger_enabled = false;
    bool virtual_time_enabled = false;
    bool paused = false;  // true = JS halted + time frozen
    double virtual_time_base_ticks_ms = 0;
  };
  std::map<std::string, ExecutionState> execution_states_;

  // Held keyboard keys state per tab (for keyboard/down and keyboard/up)
  struct HeldKeyState {
    HeldKeyState();
    ~HeldKeyState();
    HeldKeyState(const HeldKeyState&);
    HeldKeyState& operator=(const HeldKeyState&);

    std::set<std::string> held_keys;  // Set of currently held key names
    int current_modifiers = 0;        // Bitmask of active modifiers (1=Alt, 2=Ctrl, 4=Meta, 8=Shift)
  };
  std::map<std::string, HeldKeyState> held_keys_state_;

  // Enable execution control (Debugger + virtual time) for a tab
  void EnableExecutionControl(
      const std::string& tab_id,
      std::optional<double> initial_virtual_time,
      base::OnceClosure then);

  // Get/Set execution state via REST endpoint
  void GetExecutionState(const std::string& tab_id, ResponseCallback callback);
  void SetExecutionState(const std::string& tab_id,
                         const base::Value::Dict& params,
                         ResponseCallback callback);

  // CDP callbacks for execution control
  void OnDebuggerEnabled(const std::string& tab_id,
                         std::optional<double> initial_virtual_time,
                         base::OnceClosure then,
                         bool success,
                         const std::string& result);
  void OnVirtualTimeEnabled(const std::string& tab_id,
                            base::OnceClosure then,
                            bool success,
                            const std::string& result);
  void OnDebuggerResumed(const std::string& tab_id,
                         base::OnceClosure then,
                         bool success,
                         const std::string& result);
  void OnVirtualTimeResumed(const std::string& tab_id,
                            base::OnceClosure then,
                            bool success,
                            const std::string& result);
  void OnVirtualTimePaused(const std::string& tab_id,
                           base::OnceClosure then,
                           bool success,
                           const std::string& result);
  void OnDebuggerPaused(const std::string& tab_id,
                        base::OnceClosure then,
                        bool success,
                        const std::string& result);

  // Action complete wait state for after-screenshots
  // Waits for: networkidle2 AND 500ms elapsed AND load AND DOMContentLoaded
  struct ActionCompleteWaiter {
    ActionCompleteWaiter();
    ~ActionCompleteWaiter();
    ActionCompleteWaiter(const ActionCompleteWaiter&) = delete;
    ActionCompleteWaiter& operator=(const ActionCompleteWaiter&) = delete;

    std::string tab_id;
    base::TimeTicks action_start_time;
    base::OnceClosure on_complete;

    // Condition flags
    bool load_fired = false;
    bool dom_content_loaded_fired = false;
    bool network_idle = false;
    bool min_time_elapsed = false;

    // Network tracking (networkidle2 = ≤2 connections for 500ms)
    int active_requests = 0;
    base::TimeTicks last_network_activity;

    // Timeout
    base::TimeTicks timeout_time;

    bool IsComplete() const {
      return load_fired && dom_content_loaded_fired &&
             network_idle && min_time_elapsed;
    }
  };
  std::map<std::string, std::unique_ptr<ActionCompleteWaiter>> action_waiters_;

  // Check if wait conditions are met and fire callback if so
  void CheckActionCompleteConditions(const std::string& tab_id);

  // Handle CDP events for action complete tracking
  void OnCdpEventForWait(const std::string& tab_id,
                         const std::string& method,
                         const base::Value::Dict& params);

  // Timer callback for minimum wait time
  void OnMinWaitTimeElapsed(const std::string& tab_id);

  // Timer callback for network idle check
  void OnNetworkIdleCheck(const std::string& tab_id);

  // Timer callback for wait timeout
  void OnWaitTimeout(const std::string& tab_id);

  // Dialog endpoint methods
  void GetDialog(const std::string& tab_id, ResponseCallback callback);
  void AcceptDialog(const std::string& tab_id,
                    const base::Value::Dict& params,
                    ResponseCallback callback);
  void DismissDialog(const std::string& tab_id, ResponseCallback callback);

  // Track dialog events from CDP
  void OnDialogOpened(const std::string& tab_id,
                      const std::string& dialog_type,
                      const std::string& message,
                      const std::string& default_prompt);
  void OnDialogClosed(const std::string& tab_id);

  // History controller (not owned)
  raw_ptr<AbpHistoryController> history_controller_ = nullptr;

  // Download observer (not owned - owned by AbpHttpServer)
  raw_ptr<AbpDownloadObserver> download_observer_ = nullptr;

  // Event collector for capturing events during actions (owned)
  std::unique_ptr<AbpEventCollector> event_collector_;

  // Pending file choosers (keyed by chooser ID)
  std::map<std::string, base::Value::Dict> pending_file_choosers_;

  // Pending JavaScript dialogs (keyed by tab_id - only one dialog per tab)
  struct PendingDialog {
    PendingDialog();
    ~PendingDialog();
    PendingDialog(const PendingDialog&);
    PendingDialog& operator=(const PendingDialog&);

    std::string dialog_type;   // "alert", "confirm", "prompt", "beforeunload"
    std::string message;
    std::string default_prompt;
    int64_t opened_at_ms = 0;
  };
  std::map<std::string, PendingDialog> pending_dialogs_;

  base::WeakPtrFactory<AbpController> weak_factory_{this};
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_CONTROLLER_H_
