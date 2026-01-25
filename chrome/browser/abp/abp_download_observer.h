#ifndef CHROME_BROWSER_ABP_ABP_DOWNLOAD_OBSERVER_H_
#define CHROME_BROWSER_ABP_ABP_DOWNLOAD_OBSERVER_H_

#include <map>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/download/public/common/download_item.h"
#include "content/public/browser/download_manager.h"

namespace abp {

class AbpController;

// Tracks downloads for ABP REST API.
// Observes DownloadManager for new downloads and DownloadItems for updates.
class AbpDownloadObserver : public content::DownloadManager::Observer,
                            public download::DownloadItem::Observer {
 public:
  explicit AbpDownloadObserver(AbpController* controller);
  ~AbpDownloadObserver() override;

  AbpDownloadObserver(const AbpDownloadObserver&) = delete;
  AbpDownloadObserver& operator=(const AbpDownloadObserver&) = delete;

  // Initialize - starts observing the DownloadManager
  void Start();

  // Stop observing
  void Stop();

  // Tracked download info
  struct TrackedDownload {
    TrackedDownload();
    ~TrackedDownload();
    TrackedDownload(const TrackedDownload&);
    TrackedDownload& operator=(const TrackedDownload&);

    std::string id;
    std::string url;
    std::string filename;
    std::string path;
    std::string state;  // "in_progress", "completed", "cancelled", "failed"
    int64_t bytes_received = 0;
    int64_t total_bytes = 0;
    std::string mime_type;
    int64_t start_time_ms = 0;
    int64_t end_time_ms = 0;
  };

  // Get all tracked downloads (optionally filtered by state)
  std::vector<TrackedDownload> GetDownloads(const std::string& state_filter = "",
                                            int limit = 100) const;

  // Get a specific download by ID
  std::optional<TrackedDownload> GetDownload(const std::string& id) const;

  // Cancel a download
  bool CancelDownload(const std::string& id);

 private:
  // content::DownloadManager::Observer
  void OnDownloadCreated(content::DownloadManager* manager,
                         download::DownloadItem* item) override;
  void ManagerGoingDown(content::DownloadManager* manager) override;

  // download::DownloadItem::Observer
  void OnDownloadUpdated(download::DownloadItem* item) override;
  void OnDownloadDestroyed(download::DownloadItem* item) override;

  // Generate a unique download ID
  std::string GenerateDownloadId();

  // Convert download state to string
  static std::string StateToString(download::DownloadItem::DownloadState state);

  // Update tracked download from DownloadItem
  void UpdateTrackedDownload(download::DownloadItem* item);

  raw_ptr<AbpController> controller_;
  raw_ptr<content::DownloadManager> download_manager_ = nullptr;
  std::map<std::string, TrackedDownload> tracked_downloads_;
  std::map<download::DownloadItem*, std::string> item_to_id_;
  int next_download_id_ = 1;

  base::WeakPtrFactory<AbpDownloadObserver> weak_factory_{this};
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_DOWNLOAD_OBSERVER_H_
