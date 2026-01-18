#ifndef CHROME_BROWSER_ABP_ABP_CONTROLLER_H_
#define CHROME_BROWSER_ABP_ABP_CONTROLLER_H_

#include <map>
#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client.h"
#include "content/public/browser/render_widget_host_view.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-forward.h"

namespace content {
class WebContents;
}

namespace abp {

class AbpHistoryController;

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

using ResponseCallback = base::OnceCallback<void(int status, std::string body)>;

// CDP client for sending commands and receiving responses
class AbpCdpClient : public content::DevToolsAgentHostClient {
 public:
  using CdpCallback = base::OnceCallback<void(bool success,
                                               const std::string& result)>;

  explicit AbpCdpClient(scoped_refptr<content::DevToolsAgentHost> host);
  ~AbpCdpClient() override;

  AbpCdpClient(const AbpCdpClient&) = delete;
  AbpCdpClient& operator=(const AbpCdpClient&) = delete;

  // Send a CDP command and receive response via callback
  void SendCommand(const std::string& method,
                   const base::Value::Dict& params,
                   CdpCallback callback);

  // DevToolsAgentHostClient implementation
  void DispatchProtocolMessage(content::DevToolsAgentHost* host,
                               base::span<const uint8_t> message) override;
  void AgentHostClosed(content::DevToolsAgentHost* host) override;

 private:
  scoped_refptr<content::DevToolsAgentHost> host_;
  int next_command_id_ = 1;
  std::map<int, CdpCallback> pending_callbacks_;
  base::WeakPtrFactory<AbpCdpClient> weak_factory_{this};
};

// Handles ABP REST API requests on the UI thread.
// Provides direct access to browser windows and tabs.
class AbpController {
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
  };

  // Set the history controller for action recording
  void SetHistoryController(AbpHistoryController* history_controller);

  // Route incoming HTTP request to appropriate handler
  void HandleRequest(const std::string& method,
                     const std::string& path,
                     const std::string& body,
                     ResponseCallback callback);

  // Center the mouse cursor in the active tab's viewport
  // Used for ABP input-only mode initialization
  void CenterMouseInActiveTab();

  // Helpers (public for use in lambdas)
  content::WebContents* FindWebContents(const std::string& tab_id);
  AbpCdpClient* GetOrCreateCdpClient(content::WebContents* wc);
  void SendError(int status,
                 const std::string& error,
                 ResponseCallback callback);
  void SendJson(int status,
                base::Value value,
                ResponseCallback callback);

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
  void OnExecuteScriptResult(ResponseCallback callback,
                             bool success,
                             const std::string& result);
  void OnClickBeforeScreenshot(const std::string& tab_id,
                               double x,
                               double y,
                               std::unique_ptr<ActionContext> context,
                               ResponseCallback callback,
                               std::string screenshot_before_path);
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
  void OnTypeResult(std::unique_ptr<ActionContext> context,
                    ResponseCallback callback,
                    bool success,
                    const std::string& result);
  void OnTypeAfterScreenshot(std::unique_ptr<ActionContext> context,
                             ResponseCallback callback,
                             std::string screenshot_after_path);

  // Take screenshot for history (before or after action)
  // cursor_x/cursor_y: optional cursor position (-1 to use last known position)
  // Uses direct C++ capture with CopyFromSurface
  void CaptureScreenshotForHistory(
      const std::string& tab_id,
      int64_t timestamp,
      bool is_before,
      base::OnceCallback<void(std::string path)> callback,
      double cursor_x = -1,
      double cursor_y = -1);

  // Direct screenshot capture using CopyFromSurface (no CDP/JS injection)
  void CaptureScreenshotDirect(
      content::WebContents* web_contents,
      const base::FilePath& screenshot_path,
      double cursor_x,
      double cursor_y,
      base::OnceCallback<void(std::string path)> callback);

  // Callback when surface copy completes
  void OnSurfaceCopied(
      const base::FilePath& screenshot_path,
      double cursor_x,
      double cursor_y,
      ui::mojom::CursorType cursor_type,
      float device_scale_factor,
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

  // History controller (not owned)
  raw_ptr<AbpHistoryController> history_controller_ = nullptr;

  base::WeakPtrFactory<AbpController> weak_factory_{this};
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_CONTROLLER_H_
