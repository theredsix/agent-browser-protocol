#include "chrome/browser/abp/abp_config.h"

#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "chrome/browser/abp/abp_switches.h"
#include "chrome/common/chrome_paths.h"

namespace abp {

namespace {

// Expands ~ to home directory in path strings
base::FilePath ExpandPath(const std::string& path_str) {
  if (path_str.empty()) {
    return base::FilePath();
  }

  std::string expanded = path_str;
  if (base::StartsWith(expanded, "~/", base::CompareCase::SENSITIVE)) {
    base::FilePath home_dir;
    if (base::PathService::Get(base::DIR_HOME, &home_dir)) {
      expanded = home_dir.value() + expanded.substr(1);
    }
  }
  return base::FilePath(expanded);
}

base::FilePath GetDefaultConfigDir() {
  base::FilePath config_dir;
  if (base::PathService::Get(chrome::DIR_USER_DATA, &config_dir)) {
    return config_dir;
  }
  // Fallback to ~/.config/chromium
  base::FilePath home_dir;
  if (base::PathService::Get(base::DIR_HOME, &home_dir)) {
    return home_dir.Append(".config").Append("chromium");
  }
  return base::FilePath();
}

}  // namespace

// static
AbpConfig AbpConfig::GetDefaults() {
  AbpConfig config;
  base::FilePath config_dir = GetDefaultConfigDir();

  config.history.enabled = true;
  config.history.database_path = config_dir.Append("abp_history.db");
  config.history.screenshots.enabled = true;
  config.history.screenshots.directory = config_dir.Append("abp_screenshots");

  return config;
}

AbpConfig LoadAbpConfigFromFile(const base::FilePath& config_path) {
  AbpConfig config = AbpConfig::GetDefaults();

  if (config_path.empty() || !base::PathExists(config_path)) {
    return config;
  }

  std::string contents;
  if (!base::ReadFileToString(config_path, &contents)) {
    LOG(WARNING) << "ABP: Failed to read config file: " << config_path;
    return config;
  }

  auto parsed = base::JSONReader::ReadAndReturnValueWithError(
      contents, base::JSON_PARSE_RFC);
  if (!parsed.has_value()) {
    LOG(WARNING) << "ABP: Failed to parse config file: " << parsed.error().message;
    return config;
  }

  if (!parsed->is_dict()) {
    LOG(WARNING) << "ABP: Config file must be a JSON object";
    return config;
  }

  const base::Value::Dict& root = parsed->GetDict();

  // Parse history section
  const base::Value::Dict* history = root.FindDict("history");
  if (history) {
    // history.enabled
    if (auto enabled = history->FindBool("enabled")) {
      config.history.enabled = *enabled;
    }

    // history.database_path
    if (const std::string* db_path = history->FindString("database_path")) {
      base::FilePath expanded = ExpandPath(*db_path);
      if (!expanded.empty()) {
        config.history.database_path = expanded;
      }
    }

    // history.screenshots section
    const base::Value::Dict* screenshots = history->FindDict("screenshots");
    if (screenshots) {
      // screenshots.enabled
      if (auto enabled = screenshots->FindBool("enabled")) {
        config.history.screenshots.enabled = *enabled;
      }

      // screenshots.directory
      if (const std::string* dir = screenshots->FindString("directory")) {
        base::FilePath expanded = ExpandPath(*dir);
        if (!expanded.empty()) {
          config.history.screenshots.directory = expanded;
        }
      }
    }
  }

  LOG(INFO) << "ABP: Loaded config from " << config_path;
  return config;
}

AbpConfig LoadAbpConfig() {
  const base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();

  // Check --abp-config flag first
  if (command_line->HasSwitch(switches::kAbpConfig)) {
    base::FilePath config_path =
        command_line->GetSwitchValuePath(switches::kAbpConfig);
    return LoadAbpConfigFromFile(config_path);
  }

  // Try default config location
  base::FilePath config_dir = GetDefaultConfigDir();
  if (!config_dir.empty()) {
    base::FilePath default_config = config_dir.Append("abp_config.json");
    if (base::PathExists(default_config)) {
      return LoadAbpConfigFromFile(default_config);
    }
  }

  // Return defaults
  return AbpConfig::GetDefaults();
}

}  // namespace abp
