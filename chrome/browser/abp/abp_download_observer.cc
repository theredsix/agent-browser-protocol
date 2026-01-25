#include "chrome/browser/abp/abp_download_observer.h"

#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "content/public/browser/browser_context.h"

namespace abp {

// TrackedDownload implementation
AbpDownloadObserver::TrackedDownload::TrackedDownload() = default;
AbpDownloadObserver::TrackedDownload::~TrackedDownload() = default;
AbpDownloadObserver::TrackedDownload::TrackedDownload(const TrackedDownload&) =
    default;
AbpDownloadObserver::TrackedDownload&
AbpDownloadObserver::TrackedDownload::operator=(const TrackedDownload&) =
    default;

AbpDownloadObserver::AbpDownloadObserver(AbpController* controller)
    : controller_(controller) {}

AbpDownloadObserver::~AbpDownloadObserver() {
  Stop();
}

void AbpDownloadObserver::Start() {
  // Get the DownloadManager from the first browser's profile
  for (Browser* browser : *BrowserList::GetInstance()) {
    Profile* profile = browser->profile();
    if (profile) {
      download_manager_ = profile->GetDownloadManager();
      if (download_manager_) {
        download_manager_->AddObserver(this);
        LOG(INFO) << "ABP: Download observer started";
        return;
      }
    }
  }
  LOG(WARNING) << "ABP: No DownloadManager available for download observer";
}

void AbpDownloadObserver::Stop() {
  // Unobserve all download items
  for (const auto& pair : item_to_id_) {
    pair.first->RemoveObserver(this);
  }
  item_to_id_.clear();

  // Unobserve download manager
  if (download_manager_) {
    download_manager_->RemoveObserver(this);
    download_manager_ = nullptr;
  }
}

std::string AbpDownloadObserver::GenerateDownloadId() {
  return base::StringPrintf("dl_%d", next_download_id_++);
}

std::string AbpDownloadObserver::StateToString(
    download::DownloadItem::DownloadState state) {
  switch (state) {
    case download::DownloadItem::IN_PROGRESS:
      return "in_progress";
    case download::DownloadItem::COMPLETE:
      return "completed";
    case download::DownloadItem::CANCELLED:
      return "cancelled";
    case download::DownloadItem::INTERRUPTED:
      return "failed";
    default:
      return "unknown";
  }
}

void AbpDownloadObserver::OnDownloadCreated(content::DownloadManager* manager,
                                            download::DownloadItem* item) {
  std::string id = GenerateDownloadId();
  item_to_id_[item] = id;
  item->AddObserver(this);

  TrackedDownload download;
  download.id = id;
  download.url = item->GetURL().spec();
  download.filename = item->GetTargetFilePath().BaseName().AsUTF8Unsafe();
  download.path = item->GetTargetFilePath().AsUTF8Unsafe();
  download.state = StateToString(item->GetState());
  download.bytes_received = item->GetReceivedBytes();
  download.total_bytes = item->GetTotalBytes();
  download.mime_type = item->GetMimeType();
  download.start_time_ms = item->GetStartTime().InMillisecondsSinceUnixEpoch();

  tracked_downloads_[id] = std::move(download);

  LOG(INFO) << "ABP: Download created id=" << id << " url=" << download.url;
}

void AbpDownloadObserver::OnDownloadUpdated(download::DownloadItem* item) {
  auto it = item_to_id_.find(item);
  if (it == item_to_id_.end()) {
    return;
  }

  UpdateTrackedDownload(item);
}

void AbpDownloadObserver::UpdateTrackedDownload(download::DownloadItem* item) {
  auto it = item_to_id_.find(item);
  if (it == item_to_id_.end()) {
    return;
  }

  const std::string& id = it->second;
  auto dl_it = tracked_downloads_.find(id);
  if (dl_it == tracked_downloads_.end()) {
    return;
  }

  TrackedDownload& download = dl_it->second;
  download.state = StateToString(item->GetState());
  download.bytes_received = item->GetReceivedBytes();
  download.total_bytes = item->GetTotalBytes();
  download.filename = item->GetTargetFilePath().BaseName().AsUTF8Unsafe();
  download.path = item->GetTargetFilePath().AsUTF8Unsafe();

  if (item->GetState() == download::DownloadItem::COMPLETE ||
      item->GetState() == download::DownloadItem::CANCELLED ||
      item->GetState() == download::DownloadItem::INTERRUPTED) {
    download.end_time_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
    LOG(INFO) << "ABP: Download finished id=" << id
              << " state=" << download.state;
  }
}

void AbpDownloadObserver::OnDownloadDestroyed(download::DownloadItem* item) {
  auto it = item_to_id_.find(item);
  if (it != item_to_id_.end()) {
    item_to_id_.erase(it);
  }
}

void AbpDownloadObserver::ManagerGoingDown(content::DownloadManager* manager) {
  if (download_manager_ == manager) {
    download_manager_ = nullptr;
  }
}

std::vector<AbpDownloadObserver::TrackedDownload>
AbpDownloadObserver::GetDownloads(const std::string& state_filter,
                                  int limit) const {
  std::vector<TrackedDownload> result;
  result.reserve(std::min(static_cast<int>(tracked_downloads_.size()), limit));

  for (const auto& pair : tracked_downloads_) {
    if (static_cast<int>(result.size()) >= limit) {
      break;
    }

    if (state_filter.empty() || pair.second.state == state_filter) {
      result.push_back(pair.second);
    }
  }

  return result;
}

std::optional<AbpDownloadObserver::TrackedDownload>
AbpDownloadObserver::GetDownload(const std::string& id) const {
  auto it = tracked_downloads_.find(id);
  if (it != tracked_downloads_.end()) {
    return it->second;
  }
  return std::nullopt;
}

bool AbpDownloadObserver::CancelDownload(const std::string& id) {
  // Find the download item by ID
  for (const auto& pair : item_to_id_) {
    if (pair.second == id) {
      download::DownloadItem* item = pair.first;
      if (item->GetState() == download::DownloadItem::IN_PROGRESS) {
        item->Cancel(true);  // Cancel with user interaction
        return true;
      }
      return false;  // Already finished
    }
  }
  return false;  // Not found
}

}  // namespace abp
