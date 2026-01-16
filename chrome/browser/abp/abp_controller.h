#ifndef CHROME_BROWSER_ABP_ABP_CONTROLLER_H_
#define CHROME_BROWSER_ABP_ABP_CONTROLLER_H_

#include <map>
#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client.h"

namespace content {
class WebContents;
}

namespace abp {

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

  // Route incoming HTTP request to appropriate handler
  void HandleRequest(const std::string& method,
                     const std::string& path,
                     const std::string& body,
                     ResponseCallback callback);

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
  void Reload(const std::string& tab_id, ResponseCallback callback);
  void GoBack(const std::string& tab_id, ResponseCallback callback);
  void GoForward(const std::string& tab_id, ResponseCallback callback);

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
  void OnClickPressedResult(const std::string& tab_id,
                            double x,
                            double y,
                            ResponseCallback callback,
                            bool success,
                            const std::string& result);
  void OnClickResult(ResponseCallback callback,
                     bool success,
                     const std::string& result);
  void OnTypeResult(ResponseCallback callback,
                    bool success,
                    const std::string& result);

  // CDP clients per WebContents (keyed by DevToolsAgentHost ID)
  std::map<std::string, std::unique_ptr<AbpCdpClient>> cdp_clients_;

  base::WeakPtrFactory<AbpController> weak_factory_{this};
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_CONTROLLER_H_
