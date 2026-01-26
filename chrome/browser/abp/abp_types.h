#ifndef CHROME_BROWSER_ABP_ABP_TYPES_H_
#define CHROME_BROWSER_ABP_ABP_TYPES_H_

#include <string>

#include "base/functional/callback.h"

namespace abp {

// Standard response callback for all ABP components.
// Used by AbpController, AbpMcpHandler, AbpHistoryController, and AbpActionContext.
//
// Parameters:
//   status: HTTP status code (200, 400, 404, 500, etc.)
//   content_type: MIME type ("application/json", "image/webp", etc.)
//   body: Response body content
using ResponseCallback = base::OnceCallback<void(int status,
                                                  const std::string& content_type,
                                                  std::string body)>;

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_TYPES_H_
