#ifndef CHROME_BROWSER_ABP_ABP_EVENT_COLLECTOR_H_
#define CHROME_BROWSER_ABP_ABP_EVENT_COLLECTOR_H_

#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/values.h"

namespace abp {

// Represents a captured browser event during action execution
struct AbpEvent {
  AbpEvent();
  AbpEvent(const std::string& type, int64_t virtual_time_ms, base::Value::Dict data);
  ~AbpEvent();
  AbpEvent(const AbpEvent&) = delete;
  AbpEvent& operator=(const AbpEvent&) = delete;
  AbpEvent(AbpEvent&&);
  AbpEvent& operator=(AbpEvent&&);

  std::string type;           // "navigation", "dialog", "file_chooser", etc.
  int64_t virtual_time_ms = 0;    // Virtual time when event occurred
  base::Value::Dict data;     // Event-specific data
};

class AbpController;

// Collects browser events during action execution.
// Owned by AbpController and lives on the UI thread.
class AbpEventCollector {
 public:
  explicit AbpEventCollector(AbpController* controller);
  ~AbpEventCollector();

  AbpEventCollector(const AbpEventCollector&) = delete;
  AbpEventCollector& operator=(const AbpEventCollector&) = delete;

  // Start capturing events for a tab
  void StartCapturing(const std::string& tab_id);

  // Stop capturing and return captured events
  std::vector<AbpEvent> StopCapturing();

  // Check if currently capturing
  bool IsCapturing() const { return capturing_; }

  // Get the tab ID being captured
  const std::string& GetCapturingTabId() const { return capturing_tab_id_; }

  // Handle CDP events (called from AbpCdpClient event listener)
  void OnCdpEvent(const std::string& tab_id,
                  const std::string& method,
                  const base::Value::Dict& params);

  // Handle Chrome browser events
  void OnTabCreated(const std::string& new_tab_id,
                    const std::string& opener_tab_id);
  void OnTabClosed(const std::string& tab_id);
  void OnDownloadStarted(const std::string& download_id,
                         const std::string& url);
  void OnDownloadCompleted(const std::string& download_id);

  // Generate unique IDs for file choosers
  std::string GenerateFileChooserId();

 private:
  // Get virtual time for current tab, or wall clock if not enabled
  int64_t GetVirtualTimeMs();

  // Add an event to the capture buffer
  void AddEvent(const std::string& type, base::Value::Dict data);

  raw_ptr<AbpController> controller_;
  bool capturing_ = false;
  std::string capturing_tab_id_;
  std::vector<AbpEvent> captured_events_;
  int next_file_chooser_id_ = 1;
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_EVENT_COLLECTOR_H_
