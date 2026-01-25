#ifndef CHROME_BROWSER_ABP_ABP_SWITCHES_H_
#define CHROME_BROWSER_ABP_ABP_SWITCHES_H_

namespace abp::switches {

// Enable ABP HTTP server
extern const char kEnableAbp[];

// Port for HTTP server (default: 8222)
extern const char kAbpPort[];

// Path to ABP config file (default: ~/.config/chromium/abp_config.json)
extern const char kAbpConfig[];

// Allow system input even when ABP is enabled (ABP blocks system input by default)
extern const char kAllowSystemInputs[];

// Disable execution control (Debugger.pause + virtual time) - enabled by default
extern const char kAbpDisablePause[];

// Session directory for storing screenshots, database, and logs
// Default: /tmp/abp-<UUID>
extern const char kAbpSessionDir[];

}  // namespace abp::switches

#endif  // CHROME_BROWSER_ABP_ABP_SWITCHES_H_
