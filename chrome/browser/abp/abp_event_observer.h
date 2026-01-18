#ifndef CHROME_BROWSER_ABP_ABP_EVENT_OBSERVER_H_
#define CHROME_BROWSER_ABP_ABP_EVENT_OBSERVER_H_

#include <map>
#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/values.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client.h"
#include "content/public/browser/devtools_agent_host_observer.h"

namespace abp {

class AbpHistoryController;

// CDP event client for a single tab
class AbpCdpEventClient : public content::DevToolsAgentHostClient {
 public:
  using EventCallback =
      base::RepeatingCallback<void(const std::string& tab_id,
                                   const std::string& event_type,
                                   const base::Value::Dict& data)>;

  AbpCdpEventClient(scoped_refptr<content::DevToolsAgentHost> host,
                    EventCallback callback);
  ~AbpCdpEventClient() override;

  AbpCdpEventClient(const AbpCdpEventClient&) = delete;
  AbpCdpEventClient& operator=(const AbpCdpEventClient&) = delete;

  const std::string& tab_id() const { return tab_id_; }

  // Enable CDP domains for event capture
  void EnableEventCapture();

  // DevToolsAgentHostClient implementation
  void DispatchProtocolMessage(content::DevToolsAgentHost* host,
                               base::span<const uint8_t> message) override;
  void AgentHostClosed(content::DevToolsAgentHost* host) override;

 private:
  void SendCommand(const std::string& method,
                   const base::Value::Dict& params = {});
  void HandleEvent(const std::string& method, const base::Value::Dict& params);

  // Rate limiting for scroll events
  bool ShouldRateLimitScroll();

  scoped_refptr<content::DevToolsAgentHost> host_;
  std::string tab_id_;
  EventCallback callback_;
  int next_command_id_ = 1;

  // Rate limiting state
  base::TimeTicks last_scroll_time_;

  base::WeakPtrFactory<AbpCdpEventClient> weak_factory_{this};
};

// Observes all tabs and captures CDP events for history recording.
// Must be created and used on the UI thread.
class AbpEventObserver : public content::DevToolsAgentHostObserver {
 public:
  explicit AbpEventObserver(AbpHistoryController* history_controller);
  ~AbpEventObserver() override;

  AbpEventObserver(const AbpEventObserver&) = delete;
  AbpEventObserver& operator=(const AbpEventObserver&) = delete;

  // Start observing all tabs
  void Start();

  // Stop observing
  void Stop();

 private:
  // DevToolsAgentHostObserver implementation
  void DevToolsAgentHostCreated(content::DevToolsAgentHost* host) override;
  void DevToolsAgentHostAttached(content::DevToolsAgentHost* host) override;
  void DevToolsAgentHostNavigated(content::DevToolsAgentHost* host) override;
  void DevToolsAgentHostDetached(content::DevToolsAgentHost* host) override;
  void DevToolsAgentHostCrashed(content::DevToolsAgentHost* host,
                                base::TerminationStatus status) override;
  bool ShouldForceDevToolsAgentHostCreation() override;

  // Attach to a new tab for event capture
  void AttachToHost(content::DevToolsAgentHost* host);

  // Event callback from CDP clients
  void OnEvent(const std::string& tab_id,
               const std::string& event_type,
               const base::Value::Dict& data);

  raw_ptr<AbpHistoryController> history_controller_;  // Not owned
  bool observing_ = false;

  // CDP event clients per tab
  std::map<std::string, std::unique_ptr<AbpCdpEventClient>> event_clients_;

  base::WeakPtrFactory<AbpEventObserver> weak_factory_{this};
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_EVENT_OBSERVER_H_
