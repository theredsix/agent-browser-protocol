#ifndef CHROME_BROWSER_ABP_ABP_TOOL_BUILDER_H_
#define CHROME_BROWSER_ABP_ABP_TOOL_BUILDER_H_

#include <string>
#include <vector>

#include "base/values.h"

namespace abp {

// Fluent builder for MCP tool definitions.
//
// Reduces verbose manual JSON construction from ~30 lines per tool to ~5 lines:
//
// Before:
//   base::Value::Dict tool;
//   tool.Set("name", "browser_click");
//   tool.Set("description", "Click at coordinates");
//   base::Value::Dict input_schema;
//   input_schema.Set("type", "object");
//   base::Value::Dict props;
//   base::Value::Dict tab_id_prop;
//   tab_id_prop.Set("type", "string");
//   tab_id_prop.Set("description", "Target tab ID");
//   props.Set("tab_id", std::move(tab_id_prop));
//   // ... 20 more lines ...
//
// After:
//   ToolBuilder("browser_click")
//       .Description("Click at coordinates")
//       .RequiredString("tab_id", "Target tab ID")
//       .RequiredNumber("x", "X coordinate")
//       .RequiredNumber("y", "Y coordinate")
//       .OptionalStringEnum("button", "Mouse button", {"left", "right", "middle"})
//       .Build();
//
class ToolBuilder {
 public:
  explicit ToolBuilder(const std::string& name);
  ~ToolBuilder();

  ToolBuilder(const ToolBuilder&) = delete;
  ToolBuilder& operator=(const ToolBuilder&) = delete;
  ToolBuilder(ToolBuilder&&);
  ToolBuilder& operator=(ToolBuilder&&);

  // Set the tool description
  ToolBuilder& Description(const std::string& description);

  // Required properties (added to "required" array)
  ToolBuilder& RequiredString(const std::string& name,
                              const std::string& description);
  ToolBuilder& RequiredNumber(const std::string& name,
                              const std::string& description);
  ToolBuilder& RequiredBoolean(const std::string& name,
                               const std::string& description);

  // Required with enum constraint
  ToolBuilder& RequiredStringEnum(const std::string& name,
                                  const std::string& description,
                                  std::vector<std::string> values);

  // Optional properties
  ToolBuilder& OptionalString(const std::string& name,
                              const std::string& description);
  ToolBuilder& OptionalNumber(const std::string& name,
                              const std::string& description);
  ToolBuilder& OptionalBoolean(const std::string& name,
                               const std::string& description);

  // Optional with enum constraint
  ToolBuilder& OptionalStringEnum(const std::string& name,
                                  const std::string& description,
                                  std::vector<std::string> values);

  // Array properties (items are strings by default)
  ToolBuilder& OptionalStringArray(const std::string& name,
                                   const std::string& description);
  ToolBuilder& RequiredStringArray(const std::string& name,
                                   const std::string& description);

  // Array of strings with enum constraint on items
  ToolBuilder& OptionalStringArrayEnum(const std::string& name,
                                       const std::string& description,
                                       std::vector<std::string> values);

  // Build the final tool definition
  base::Value::Dict Build();

 private:
  void AddProperty(const std::string& name,
                   const std::string& type,
                   const std::string& description,
                   bool required);
  void AddPropertyWithEnum(const std::string& name,
                           const std::string& type,
                           const std::string& description,
                           std::vector<std::string> values,
                           bool required);
  void AddArrayProperty(const std::string& name,
                        const std::string& description,
                        base::Value::Dict items,
                        bool required);

  std::string name_;
  std::string description_;
  base::Value::Dict properties_;
  base::Value::List required_;
};

}  // namespace abp

#endif  // CHROME_BROWSER_ABP_ABP_TOOL_BUILDER_H_
