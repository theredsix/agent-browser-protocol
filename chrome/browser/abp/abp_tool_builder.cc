#include "chrome/browser/abp/abp_tool_builder.h"

namespace abp {

ToolBuilder::ToolBuilder(const std::string& name) : name_(name) {}

ToolBuilder::~ToolBuilder() = default;

ToolBuilder::ToolBuilder(ToolBuilder&&) = default;
ToolBuilder& ToolBuilder::operator=(ToolBuilder&&) = default;

ToolBuilder& ToolBuilder::Description(const std::string& description) {
  description_ = description;
  return *this;
}

ToolBuilder& ToolBuilder::RequiredString(const std::string& name,
                                         const std::string& description) {
  AddProperty(name, "string", description, true);
  return *this;
}

ToolBuilder& ToolBuilder::RequiredNumber(const std::string& name,
                                         const std::string& description) {
  AddProperty(name, "number", description, true);
  return *this;
}

ToolBuilder& ToolBuilder::RequiredBoolean(const std::string& name,
                                          const std::string& description) {
  AddProperty(name, "boolean", description, true);
  return *this;
}

ToolBuilder& ToolBuilder::RequiredStringEnum(const std::string& name,
                                             const std::string& description,
                                             std::vector<std::string> values) {
  AddPropertyWithEnum(name, "string", description, std::move(values), true);
  return *this;
}

ToolBuilder& ToolBuilder::OptionalString(const std::string& name,
                                         const std::string& description) {
  AddProperty(name, "string", description, false);
  return *this;
}

ToolBuilder& ToolBuilder::OptionalNumber(const std::string& name,
                                         const std::string& description) {
  AddProperty(name, "number", description, false);
  return *this;
}

ToolBuilder& ToolBuilder::OptionalBoolean(const std::string& name,
                                          const std::string& description) {
  AddProperty(name, "boolean", description, false);
  return *this;
}

ToolBuilder& ToolBuilder::OptionalStringEnum(const std::string& name,
                                             const std::string& description,
                                             std::vector<std::string> values) {
  AddPropertyWithEnum(name, "string", description, std::move(values), false);
  return *this;
}

ToolBuilder& ToolBuilder::OptionalStringArray(const std::string& name,
                                              const std::string& description) {
  base::Value::Dict items;
  items.Set("type", "string");
  AddArrayProperty(name, description, std::move(items), false);
  return *this;
}

ToolBuilder& ToolBuilder::RequiredStringArray(const std::string& name,
                                              const std::string& description) {
  base::Value::Dict items;
  items.Set("type", "string");
  AddArrayProperty(name, description, std::move(items), true);
  return *this;
}

ToolBuilder& ToolBuilder::OptionalStringArrayEnum(
    const std::string& name,
    const std::string& description,
    std::vector<std::string> values) {
  base::Value::Dict items;
  items.Set("type", "string");
  base::Value::List enum_values;
  for (const auto& value : values) {
    enum_values.Append(value);
  }
  items.Set("enum", std::move(enum_values));
  AddArrayProperty(name, description, std::move(items), false);
  return *this;
}

base::Value::Dict ToolBuilder::Build() {
  base::Value::Dict tool;
  tool.Set("name", name_);
  tool.Set("description", description_);

  base::Value::Dict input_schema;
  input_schema.Set("type", "object");
  input_schema.Set("properties", std::move(properties_));

  if (!required_.empty()) {
    input_schema.Set("required", std::move(required_));
  }

  tool.Set("inputSchema", std::move(input_schema));
  return tool;
}

void ToolBuilder::AddProperty(const std::string& name,
                              const std::string& type,
                              const std::string& description,
                              bool required) {
  base::Value::Dict prop;
  prop.Set("type", type);
  prop.Set("description", description);
  properties_.Set(name, std::move(prop));

  if (required) {
    required_.Append(name);
  }
}

void ToolBuilder::AddPropertyWithEnum(const std::string& name,
                                      const std::string& type,
                                      const std::string& description,
                                      std::vector<std::string> values,
                                      bool required) {
  base::Value::Dict prop;
  prop.Set("type", type);
  prop.Set("description", description);

  base::Value::List enum_values;
  for (const auto& value : values) {
    enum_values.Append(value);
  }
  prop.Set("enum", std::move(enum_values));

  properties_.Set(name, std::move(prop));

  if (required) {
    required_.Append(name);
  }
}

void ToolBuilder::AddArrayProperty(const std::string& name,
                                   const std::string& description,
                                   base::Value::Dict items,
                                   bool required) {
  base::Value::Dict prop;
  prop.Set("type", "array");
  prop.Set("description", description);
  prop.Set("items", std::move(items));
  properties_.Set(name, std::move(prop));

  if (required) {
    required_.Append(name);
  }
}

}  // namespace abp
