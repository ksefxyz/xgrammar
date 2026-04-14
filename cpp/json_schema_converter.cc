/*!
 *  Copyright (c) 2024 by Contributors
 * \file xgrammar/json_schema_converter.cc
 * \brief Implementation of JSONSchemaConverter and related utilities.
 */
#include "json_schema_converter.h"

#include <picojson.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "json_schema_converter_ext.h"
#include "regex_converter.h"
#include "support/logging.h"
#include "support/utils.h"

namespace xgrammar {

// ==================== Spec ToString implementations ====================

std::string IntegerSpec::ToString() const {
  return "IntegerSpec{minimum=" + (minimum.has_value() ? std::to_string(*minimum) : "null") +
         ", maximum=" + (maximum.has_value() ? std::to_string(*maximum) : "null") +
         ", exclusive_minimum=" +
         (exclusive_minimum.has_value() ? std::to_string(*exclusive_minimum) : "null") +
         ", exclusive_maximum=" +
         (exclusive_maximum.has_value() ? std::to_string(*exclusive_maximum) : "null") + "}";
}

std::string NumberSpec::ToString() const {
  return "NumberSpec{minimum=" + (minimum.has_value() ? std::to_string(*minimum) : "null") +
         ", maximum=" + (maximum.has_value() ? std::to_string(*maximum) : "null") +
         ", exclusive_minimum=" +
         (exclusive_minimum.has_value() ? std::to_string(*exclusive_minimum) : "null") +
         ", exclusive_maximum=" +
         (exclusive_maximum.has_value() ? std::to_string(*exclusive_maximum) : "null") + "}";
}

std::string StringSpec::ToString() const {
  return "StringSpec{pattern=" + (pattern.has_value() ? "\"" + *pattern + "\"" : "null") +
         ", format=" + (format.has_value() ? "\"" + *format + "\"" : "null") +
         ", min_length=" + std::to_string(min_length) +
         ", max_length=" + std::to_string(max_length) + "}";
}

std::string BooleanSpec::ToString() const { return "BooleanSpec{}"; }

std::string NullSpec::ToString() const { return "NullSpec{}"; }

std::string AnySpec::ToString() const { return "AnySpec{}"; }

std::string ArraySpec::ToString() const {
  return "ArraySpec{prefix_items.size()=" + std::to_string(prefix_items.size()) +
         ", allow_additional_items=" + (allow_additional_items ? "true" : "false") +
         ", additional_items=" + (additional_items ? "SchemaSpec" : "null") +
         ", contains_item=" + (contains_item ? "SchemaSpec" : "null") +
         ", min_contains=" + std::to_string(min_contains) +
         ", min_items=" + std::to_string(min_items) + ", max_items=" + std::to_string(max_items) +
         "}";
}

std::string ObjectSpec::ToString() const {
  std::string s =
      "ObjectSpec{properties.size()=" + std::to_string(properties.size()) + ", properties=[";
  for (size_t i = 0; i < properties.size(); ++i) {
    if (i != 0) s += ", ";
    s += properties[i].name;
  }
  s += "], pattern_properties.size()=" + std::to_string(pattern_properties.size()) + ", required=[";
  bool first = true;
  for (const auto& r : required) {
    if (!first) s += ", ";
    s += r;
    first = false;
  }
  s += "], dependent_required.size()=" + std::to_string(dependent_required.size()) +
       ", forbidden_groups.size()=" + std::to_string(forbidden_groups.size());
  s +=
      std::string(", allow_additional_properties=") +
      (allow_additional_properties ? "true" : "false") +
      ", additional_properties_schema=" + (additional_properties_schema ? "SchemaSpec" : "null") +
      ", allow_unevaluated_properties=" + (allow_unevaluated_properties ? "true" : "false") +
      ", unevaluated_properties_schema=" + (unevaluated_properties_schema ? "SchemaSpec" : "null") +
      ", property_names=" + (property_names ? "SchemaSpec" : "null") +
      ", min_properties=" + std::to_string(min_properties) +
      ", max_properties=" + std::to_string(max_properties) + "}";
  return s;
}

std::string ConstSpec::ToString() const { return "ConstSpec{json_value=\"" + json_value + "\"}"; }

std::string EnumSpec::ToString() const {
  std::string s =
      "EnumSpec{json_values.size()=" + std::to_string(json_values.size()) + ", json_values=[";
  for (size_t i = 0; i < json_values.size(); ++i) {
    if (i != 0) s += ", ";
    s += "\"" + json_values[i] + "\"";
  }
  s += "]}";
  return s;
}

std::string RefSpec::ToString() const { return "RefSpec{uri=\"" + uri + "\"}"; }

std::string AnyOfSpec::ToString() const {
  return "AnyOfSpec{options.size()=" + std::to_string(options.size()) + "}";
}

std::string OneOfSpec::ToString() const {
  return "OneOfSpec{options.size()=" + std::to_string(options.size()) + "}";
}

std::string AllOfSpec::ToString() const {
  return "AllOfSpec{schemas.size()=" + std::to_string(schemas.size()) + "}";
}

std::string TypeArraySpec::ToString() const {
  return "TypeArraySpec{type_schemas.size()=" + std::to_string(type_schemas.size()) + "}";
}

std::string SchemaSpec::ToString() const {
  std::string spec_str;
  std::visit([&spec_str](const auto& s) { spec_str = s.ToString(); }, spec);
  return "SchemaSpec{spec=" + spec_str + ", cache_key=\"" + cache_key + "\", rule_name_hint=\"" +
         rule_name_hint + "\"}";
}

// ==================== SchemaParser (Internal) ====================

namespace {

enum class SchemaErrorType : int {
  kInvalidSchema = 0,
  kUnsatisfiableSchema = 1,
};

using SchemaError = TypedError<SchemaErrorType>;

/*!
 * \brief Parser for JSON Schema, converts JSON Schema to SchemaSpec intermediate representation.
 */
class SchemaParser {
 public:
  struct Config {
    bool strict_mode = false;
    JSONFormat json_format;
  };

  explicit SchemaParser(const picojson::value& root_schema, const Config& config)
      : config_(config), root_schema_(root_schema) {}

  Result<SchemaSpecPtr, SchemaError> Parse(
      const picojson::value& schema,
      const std::string& rule_name_hint = "root",
      std::optional<std::string> default_type = std::nullopt
  );

  const picojson::value& GetRootSchema() const { return root_schema_; }
  bool IsStrictMode() const { return config_.strict_mode; }

  Result<SchemaSpecPtr, SchemaError> ResolveRef(
      const std::string& uri, const std::string& rule_name_hint
  );
  Result<SchemaSpecPtr, SchemaError> NormalizeExclusiveDisjunctions(const SchemaSpecPtr& spec);

 private:
  Result<IntegerSpec, SchemaError> ParseInteger(const picojson::object& schema);
  Result<NumberSpec, SchemaError> ParseNumber(const picojson::object& schema);
  Result<StringSpec, SchemaError> ParseString(const picojson::object& schema);
  Result<BooleanSpec, SchemaError> ParseBoolean(const picojson::object& schema);
  Result<NullSpec, SchemaError> ParseNull(const picojson::object& schema);
  Result<ArraySpec, SchemaError> ParseArray(const picojson::object& schema);
  Result<ObjectSpec, SchemaError> ParseObject(const picojson::object& schema);
  Result<ConstSpec, SchemaError> ParseConst(const picojson::object& schema);
  Result<EnumSpec, SchemaError> ParseEnum(const picojson::object& schema);
  Result<RefSpec, SchemaError> ParseRef(const picojson::object& schema);
  Result<std::vector<SchemaSpecPtr>, SchemaError> ParseCompositeOptions(
      const picojson::object& schema,
      const std::string& keyword,
      const std::string& rule_name_prefix
  );
  Result<AnyOfSpec, SchemaError> ParseAnyOf(const picojson::object& schema);
  Result<SchemaSpecPtr, SchemaError> ParseConditional(
      const picojson::object& schema, const std::string& rule_name_hint
  );
  bool IsPartialObjectFragment(const picojson::object& schema) const;
  picojson::value NormalizePartialObjectFragment(const picojson::value& fragment_value) const;
  Result<SchemaSpecPtr, SchemaError> ParseOneOf(
      const picojson::object& schema, const std::string& rule_name_hint
  );
  Result<std::optional<SchemaSpecPtr>, SchemaError> TryParseExclusiveObjectOneOf(
      const picojson::object& schema, const std::string& rule_name_hint
  );
  Result<AllOfSpec, SchemaError> ParseAllOf(const picojson::object& schema);
  Result<TypeArraySpec, SchemaError> ParseTypeArray(
      const picojson::object& schema, const std::string& rule_name_hint
  );
  Result<SchemaSpecPtr, SchemaError> MergeAllOfSchemas(
      const std::vector<SchemaSpecPtr>& schemas, const std::string& rule_name_hint
  );
  Result<SchemaSpecPtr, SchemaError> MergeSchemaSpecs(
      const SchemaSpecPtr& lhs, const SchemaSpecPtr& rhs, const std::string& rule_name_hint
  );
  Result<ObjectSpec, SchemaError> MergeObjectSpecs(
      const ObjectSpec& lhs, const ObjectSpec& rhs, const std::string& rule_name_hint
  );
  Result<ArraySpec, SchemaError> MergeArraySpecs(
      const ArraySpec& lhs, const ArraySpec& rhs, const std::string& rule_name_hint
  );
  Result<bool, SchemaError> AreSchemasDisjoint(
      const SchemaSpecPtr& lhs, const SchemaSpecPtr& rhs, const std::string& rule_name_hint
  );
  Result<SchemaSpecPtr, SchemaError> NormalizeObjectPresenceConstraints(
      const ObjectSpec& object, const std::string& rule_name_hint
  );
  Result<SchemaSpecPtr, SchemaError> NormalizeExclusiveDisjunctionsImpl(
      const SchemaSpecPtr& spec,
      const std::string& rule_name_hint,
      std::unordered_set<const SchemaSpec*>* active_specs,
      std::unordered_set<const SchemaSpec*>* normalized_specs
  );

  std::string ComputeCacheKey(const picojson::value& schema);

  static void WarnUnsupportedKeywords(
      const picojson::object& schema, const std::vector<std::string>& keywords, bool verbose = false
  );

  Config config_;
  picojson::value root_schema_;
  std::unordered_map<std::string, SchemaSpecPtr> ref_cache_;
  std::unordered_map<std::string, SchemaSpecPtr> schema_cache_;
};

bool SchemaParser::IsPartialObjectFragment(const picojson::object& schema) const {
  if (schema.count("type")) {
    if (!schema.at("type").is<std::string>() || schema.at("type").get<std::string>() != "object") {
      return false;
    }
  }
  return schema.count("properties") || schema.count("required") ||
         schema.count("dependentRequired") || schema.count("not") ||
         schema.count("minProperties") || schema.count("maxProperties");
}

picojson::value SchemaParser::NormalizePartialObjectFragment(const picojson::value& fragment_value) const {
  if (!fragment_value.is<picojson::object>()) {
    return fragment_value;
  }

  picojson::object fragment_obj = fragment_value.get<picojson::object>();
  if (fragment_obj.count("properties") && fragment_obj.at("properties").is<picojson::object>()) {
    picojson::object normalized_properties;
    const auto& properties_obj = fragment_obj.at("properties").get<picojson::object>();
    for (const auto& property_name : properties_obj.ordered_keys()) {
      normalized_properties[property_name] =
          NormalizePartialObjectFragment(properties_obj.at(property_name));
    }
    fragment_obj["properties"] = picojson::value(normalized_properties);
  }

  if (IsPartialObjectFragment(fragment_obj)) {
    if (!fragment_obj.count("type")) {
      fragment_obj["type"] = picojson::value("object");
    }
    if (!fragment_obj.count("additionalProperties")) {
      fragment_obj["additionalProperties"] = picojson::value(true);
    }
    if (!fragment_obj.count("unevaluatedProperties")) {
      fragment_obj["unevaluatedProperties"] = picojson::value(true);
    }
  }

  return picojson::value(fragment_obj);
}

std::string SchemaParser::ComputeCacheKey(const picojson::value& schema) {
  static const std::unordered_set<std::string> kSkippedKeys = {
      "title",
      "default",
      "description",
      "examples",
      "deprecated",
      "readOnly",
      "writeOnly",
      "$comment",
      "$schema",
  };

  if (schema.is<picojson::object>()) {
    std::string result = "{";
    std::vector<std::pair<std::string, picojson::value>> sorted_kv;
    for (const auto& kv : schema.get<picojson::object>()) {
      if (kSkippedKeys.count(kv.first) == 0) {
        sorted_kv.push_back(kv);
      }
    }
    std::sort(sorted_kv.begin(), sorted_kv.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.first < rhs.first;
    });
    int64_t idx = 0;
    for (const auto& [key, value] : sorted_kv) {
      if (idx != 0) {
        result += ",";
      }
      ++idx;
      result += "\"" + key + "\":" + ComputeCacheKey(value);
    }
    return result + "}";
  } else if (schema.is<picojson::array>()) {
    std::string result = "[";
    int64_t idx = 0;
    for (const auto& item : schema.get<picojson::array>()) {
      if (idx != 0) {
        result += ",";
      }
      ++idx;
      result += ComputeCacheKey(item);
    }
    return result + "]";
  }
  return schema.serialize(false);
}

void SchemaParser::WarnUnsupportedKeywords(
    const picojson::object& schema, const std::vector<std::string>& keywords, bool verbose
) {
  if (!verbose) {
    return;
  }
  for (const auto& keyword : keywords) {
    if (schema.find(keyword) != schema.end()) {
      XGRAMMAR_LOG(WARNING) << "Keyword " << keyword << " is not supported";
    }
  }
}

Result<SchemaSpecPtr, SchemaError> SchemaParser::ParseConditional(
    const picojson::object& schema, const std::string& rule_name_hint
) {
  picojson::object base_schema = schema;
  base_schema.erase("if");
  base_schema.erase("then");
  base_schema.erase("else");

  auto combine_constraints =
      [](const std::vector<picojson::value>& schemas) -> picojson::value {
        if (schemas.empty()) {
          return picojson::value(picojson::object{});
        }
        if (schemas.size() == 1) {
          return schemas.front();
        }
        picojson::array all_of;
        for (const auto& schema_value : schemas) {
          all_of.push_back(schema_value);
        }
        picojson::object combined;
        combined["allOf"] = picojson::value(all_of);
        return picojson::value(combined);
      };

  auto parse_branch_with_base =
      [&](const picojson::value& branch_schema) -> Result<SchemaSpecPtr, SchemaError> {
    std::vector<picojson::value> parts;
    if (!base_schema.empty()) {
      parts.push_back(picojson::value(base_schema));
    }
    if (branch_schema.is<picojson::object>()) {
      parts.push_back(NormalizePartialObjectFragment(branch_schema));
    } else {
      parts.push_back(branch_schema);
    }
    return Parse(combine_constraints(parts), rule_name_hint);
  };

  if (!schema.count("if")) {
    return Parse(picojson::value(base_schema), rule_name_hint);
  }

  const auto& if_value = schema.at("if");
  if (if_value.is<bool>()) {
    if (if_value.get<bool>()) {
      if (schema.count("then")) {
        return parse_branch_with_base(schema.at("then"));
      }
      return Parse(picojson::value(base_schema), rule_name_hint);
    }
    if (schema.count("else")) {
      return parse_branch_with_base(schema.at("else"));
    }
    return Parse(picojson::value(base_schema), rule_name_hint);
  }

  if (!if_value.is<picojson::object>()) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kInvalidSchema, "if must be an object or boolean"
    );
  }
  if (schema.count("$ref")) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kInvalidSchema, "if/then/else with $ref siblings is not supported"
    );
  }

  const auto& if_obj = if_value.get<picojson::object>();
  static const std::unordered_set<std::string> kIgnoredKeys = {
      "title",
      "default",
      "description",
      "examples",
      "deprecated",
      "readOnly",
      "writeOnly",
      "$comment",
  };

  if (if_obj.count("type")) {
    if (!if_obj.at("type").is<std::string>() || if_obj.at("type").get<std::string>() != "object") {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "if type must be object"
      );
    }
  }

  for (const auto& key : if_obj.ordered_keys()) {
    if (key == "type" || key == "required" || key == "properties" || kIgnoredKeys.count(key) != 0) {
      continue;
    }
    return ResultErr<SchemaError>(
        SchemaErrorType::kInvalidSchema,
        "if only supports object required and properties with const/enum predicates"
    );
  }

  std::unordered_set<std::string> required_names;
  if (if_obj.count("required")) {
    if (!if_obj.at("required").is<picojson::array>()) {
      return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "if required must be an array");
    }
    for (const auto& item : if_obj.at("required").get<picojson::array>()) {
      if (!item.is<std::string>()) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema, "if required must contain only strings"
        );
      }
      required_names.insert(item.get<std::string>());
    }
  }

  struct PropertyPredicate {
    std::string name;
    std::vector<std::string> positive_values;
    std::vector<std::string> negative_values;
  };

  struct ArrayContainsPredicate {
    std::string name;
    picojson::value positive_fragment;
    picojson::value negative_present_fragment;
  };

  std::vector<PropertyPredicate> property_predicates;
  std::vector<ArrayContainsPredicate> array_contains_predicates;
  std::vector<std::string> required_only_names;
  std::unordered_set<std::string> property_predicate_names;

  auto extract_finite_domain =
      [&](const picojson::value& value) -> Result<std::vector<std::string>, SchemaError> {
    if (!value.is<picojson::object>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "if property predicate must be an object"
      );
    }
    const auto& obj = value.get<picojson::object>();
    for (const auto& key : obj.ordered_keys()) {
      if (key == "const" || key == "enum" || key == "type" || kIgnoredKeys.count(key) != 0) {
        continue;
      }
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema,
          "if property predicates only support const or enum"
      );
    }
    std::vector<std::string> values;
    if (obj.count("const")) {
      values.push_back(obj.at("const").serialize());
    } else if (obj.count("enum")) {
      if (!obj.at("enum").is<picojson::array>()) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema, "if property enum must be an array"
        );
      }
      for (const auto& item : obj.at("enum").get<picojson::array>()) {
        auto serialized = item.serialize();
        if (std::find(values.begin(), values.end(), serialized) == values.end()) {
          values.push_back(serialized);
        }
      }
    } else {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema,
          "if property predicates must contain const or enum"
      );
    }
    return ResultOk(std::move(values));
  };

  auto make_required_array = [](const std::vector<std::string>& names) {
    picojson::array required;
    for (const auto& name : names) {
      required.push_back(picojson::value(name));
    }
    return required;
  };

  auto make_absence_reason = [&](const std::string& property_name) {
    picojson::object inner;
    inner["required"] = picojson::value(make_required_array({property_name}));
    picojson::object outer;
    outer["not"] = picojson::value(inner);
    return picojson::value(outer);
  };

  auto make_value_reason =
      [&](const std::string& property_name, const std::vector<std::string>& values) {
        picojson::object property_schema;
        if (values.size() == 1) {
          picojson::value parsed_value;
          std::string err = picojson::parse(parsed_value, values[0]);
          XGRAMMAR_CHECK(err.empty()) << "Failed to parse serialized enum value";
          property_schema["const"] = parsed_value;
        } else {
          picojson::array enum_values;
          for (const auto& value : values) {
            picojson::value parsed_value;
            std::string err = picojson::parse(parsed_value, value);
            XGRAMMAR_CHECK(err.empty()) << "Failed to parse serialized enum value";
            enum_values.push_back(parsed_value);
          }
          property_schema["enum"] = picojson::value(enum_values);
        }
        picojson::object properties;
        properties[property_name] = picojson::value(property_schema);
        picojson::object reason;
        reason["properties"] = picojson::value(properties);
        reason["required"] = picojson::value(make_required_array({property_name}));
        return picojson::value(reason);
      };

  auto make_required_property_fragment =
      [&](const std::string& property_name, const picojson::value& property_schema) {
        picojson::object properties;
        properties[property_name] = property_schema;
        picojson::object fragment;
        fragment["properties"] = picojson::value(properties);
        fragment["required"] = picojson::value(make_required_array({property_name}));
        return picojson::value(fragment);
      };

  auto parse_array_contains_predicate =
      [&](const std::string& property_name,
          const picojson::value& value,
          const picojson::object* available_base_properties) -> Result<ArrayContainsPredicate, SchemaError> {
        if (!value.is<picojson::object>()) {
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema, "if array predicate must be an object"
          );
        }
        const auto& array_obj = value.get<picojson::object>();
        for (const auto& key : array_obj.ordered_keys()) {
          if (key == "type" || key == "contains" || key == "minContains" ||
              kIgnoredKeys.count(key) != 0) {
            continue;
          }
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema,
              "if array predicates only support contains with minContains == 1"
          );
        }
        if (array_obj.count("type")) {
          if (!array_obj.at("type").is<std::string>() ||
              array_obj.at("type").get<std::string>() != "array") {
            return ResultErr<SchemaError>(
                SchemaErrorType::kInvalidSchema, "if array predicate type must be array"
            );
          }
        }
        if (!array_obj.count("contains")) {
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema,
              "if array predicates only support contains with minContains == 1"
          );
        }
        if (array_obj.count("minContains")) {
          if (!array_obj.at("minContains").is<int64_t>() ||
              array_obj.at("minContains").get<int64_t>() != 1) {
            return ResultErr<SchemaError>(
                SchemaErrorType::kInvalidSchema,
                "if array predicates only support minContains == 1"
            );
          }
        }
        if (!available_base_properties || !available_base_properties->count(property_name) ||
            !available_base_properties->at(property_name).is<picojson::object>()) {
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema,
              "if array predicates require a direct sibling array property definition: " +
                  property_name
          );
        }
        const auto& base_array_obj =
            available_base_properties->at(property_name).get<picojson::object>();
        if (!base_array_obj.count("items") || !base_array_obj.at("items").is<picojson::object>()) {
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema,
              "if array predicates require a direct sibling array property definition: " +
                  property_name
          );
        }
        const auto& base_item_obj = base_array_obj.at("items").get<picojson::object>();

        const auto& contains_value = array_obj.at("contains");
        if (!contains_value.is<picojson::object>()) {
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema,
              "if array predicates only support object contains predicates"
          );
        }
        const auto& contains_obj = contains_value.get<picojson::object>();
        for (const auto& key : contains_obj.ordered_keys()) {
          if (key == "type" || key == "required" || key == "properties" ||
              kIgnoredKeys.count(key) != 0) {
            continue;
          }
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema,
              "if array contains predicates only support object required and properties with const/enum predicates"
          );
        }
        if (contains_obj.count("type")) {
          if (!contains_obj.at("type").is<std::string>() ||
              contains_obj.at("type").get<std::string>() != "object") {
            return ResultErr<SchemaError>(
                SchemaErrorType::kInvalidSchema, "if array contains predicate type must be object"
            );
          }
        }

        std::unordered_set<std::string> item_required_names;
        if (contains_obj.count("required")) {
          if (!contains_obj.at("required").is<picojson::array>()) {
            return ResultErr<SchemaError>(
                SchemaErrorType::kInvalidSchema,
                "if array contains required must be an array"
            );
          }
          for (const auto& item : contains_obj.at("required").get<picojson::array>()) {
            if (!item.is<std::string>()) {
              return ResultErr<SchemaError>(
                  SchemaErrorType::kInvalidSchema,
                  "if array contains required must contain only strings"
              );
            }
            item_required_names.insert(item.get<std::string>());
          }
        }

        std::vector<std::string> item_required_only_names;
        std::vector<PropertyPredicate> item_property_predicates;
        std::unordered_set<std::string> item_predicate_names;
        const picojson::object* base_item_properties = nullptr;
        if (base_item_obj.count("properties") && base_item_obj.at("properties").is<picojson::object>()) {
          base_item_properties = &base_item_obj.at("properties").get<picojson::object>();
        }

        if (contains_obj.count("properties")) {
          if (!contains_obj.at("properties").is<picojson::object>()) {
            return ResultErr<SchemaError>(
                SchemaErrorType::kInvalidSchema,
                "if array contains properties must be an object"
            );
          }
          const auto& item_properties = contains_obj.at("properties").get<picojson::object>();
          for (const auto& item_property_name : item_properties.ordered_keys()) {
            if (!item_required_names.count(item_property_name)) {
              return ResultErr<SchemaError>(
                  SchemaErrorType::kInvalidSchema,
                  "if array contains property predicates must also be listed in required"
              );
            }
            auto positive_result = extract_finite_domain(item_properties.at(item_property_name));
            if (positive_result.IsErr()) return ResultErr(std::move(positive_result).UnwrapErr());
            auto positive_values = std::move(positive_result).Unwrap();

            if (!base_item_properties || !base_item_properties->count(item_property_name)) {
              return ResultErr<SchemaError>(
                  SchemaErrorType::kInvalidSchema,
                  "if array predicates require a direct sibling item property definition with const or enum: " +
                      item_property_name
              );
            }
            auto domain_result = extract_finite_domain(base_item_properties->at(item_property_name));
            if (domain_result.IsErr()) {
              return ResultErr<SchemaError>(
                  SchemaErrorType::kInvalidSchema,
                  "if array predicates require a direct sibling item property definition with const or enum: " +
                      item_property_name
              );
            }
            auto domain_values = std::move(domain_result).Unwrap();
            std::vector<std::string> negative_values;
            for (const auto& domain_value : domain_values) {
              if (std::find(positive_values.begin(), positive_values.end(), domain_value) ==
                  positive_values.end()) {
                negative_values.push_back(domain_value);
              }
            }

            item_property_predicates.push_back(
                {item_property_name, std::move(positive_values), std::move(negative_values)}
            );
            item_predicate_names.insert(item_property_name);
          }
        }

        for (const auto& required_name : item_required_names) {
          if (!item_predicate_names.count(required_name)) {
            item_required_only_names.push_back(required_name);
          }
        }

        std::vector<picojson::value> negative_item_reasons;
        for (const auto& required_name : item_required_only_names) {
          negative_item_reasons.push_back(make_absence_reason(required_name));
        }
        for (const auto& predicate : item_property_predicates) {
          negative_item_reasons.push_back(make_absence_reason(predicate.name));
          if (!predicate.negative_values.empty()) {
            negative_item_reasons.push_back(make_value_reason(predicate.name, predicate.negative_values));
          }
        }

        picojson::value negative_item_schema;
        if (negative_item_reasons.empty()) {
          negative_item_schema = picojson::value(false);
        } else if (negative_item_reasons.size() == 1) {
          negative_item_schema = negative_item_reasons.front();
        } else {
          picojson::object any_of_obj;
          any_of_obj["anyOf"] = picojson::value(picojson::array(
              negative_item_reasons.begin(), negative_item_reasons.end()));
          negative_item_schema = picojson::value(any_of_obj);
        }

        picojson::object positive_array_schema;
        positive_array_schema["type"] = picojson::value("array");
        positive_array_schema["contains"] = contains_value;

        picojson::object negative_array_schema;
        negative_array_schema["type"] = picojson::value("array");
        negative_array_schema["items"] = negative_item_schema;

        return ResultOk(ArrayContainsPredicate{
            property_name,
            make_required_property_fragment(property_name, picojson::value(positive_array_schema)),
            make_required_property_fragment(property_name, picojson::value(negative_array_schema)),
        });
      };

  std::function<Result<std::vector<picojson::value>, SchemaError>(
      const picojson::object&, const picojson::object*, const std::string&)>
      parse_object_condition =
          [&](const picojson::object& condition_obj,
              const picojson::object* available_base_schema,
              const std::string& context) -> Result<std::vector<picojson::value>, SchemaError> {
        const picojson::object* current_base_properties = nullptr;
        std::unordered_set<std::string> base_required_names;
        if (available_base_schema != nullptr) {
          if (available_base_schema->count("properties") &&
              available_base_schema->at("properties").is<picojson::object>()) {
            current_base_properties =
                &available_base_schema->at("properties").get<picojson::object>();
          }
          if (available_base_schema->count("required") &&
              available_base_schema->at("required").is<picojson::array>()) {
            for (const auto& item : available_base_schema->at("required").get<picojson::array>()) {
              if (item.is<std::string>()) {
                base_required_names.insert(item.get<std::string>());
              }
            }
          }
        }

        if (condition_obj.count("type")) {
          if (!condition_obj.at("type").is<std::string>() ||
              condition_obj.at("type").get<std::string>() != "object") {
            return ResultErr<SchemaError>(
                SchemaErrorType::kInvalidSchema, context + " type must be object"
            );
          }
        }

        for (const auto& key : condition_obj.ordered_keys()) {
          if (key == "type" || key == "required" || key == "properties" ||
              kIgnoredKeys.count(key) != 0) {
            continue;
          }
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema,
              "if only supports object required and properties with const/enum predicates"
          );
        }

        std::unordered_set<std::string> local_required_names;
        if (condition_obj.count("required")) {
          if (!condition_obj.at("required").is<picojson::array>()) {
            return ResultErr<SchemaError>(
                SchemaErrorType::kInvalidSchema, context + " required must be an array"
            );
          }
          for (const auto& item : condition_obj.at("required").get<picojson::array>()) {
            if (!item.is<std::string>()) {
              return ResultErr<SchemaError>(
                  SchemaErrorType::kInvalidSchema,
                  context + " required must contain only strings"
              );
            }
            local_required_names.insert(item.get<std::string>());
          }
        }

        std::vector<picojson::value> local_negative_reasons;
        std::unordered_set<std::string> local_predicate_names;

        if (condition_obj.count("properties")) {
          if (!condition_obj.at("properties").is<picojson::object>()) {
            return ResultErr<SchemaError>(
                SchemaErrorType::kInvalidSchema, context + " properties must be an object"
            );
          }
          const auto& properties_obj = condition_obj.at("properties").get<picojson::object>();
          for (const auto& property_name : properties_obj.ordered_keys()) {
            if (!local_required_names.count(property_name)) {
              return ResultErr<SchemaError>(
                  SchemaErrorType::kInvalidSchema,
                  "if property predicates must also be listed in required"
              );
            }
            if (!current_base_properties || !current_base_properties->count(property_name)) {
              return ResultErr<SchemaError>(
                  SchemaErrorType::kInvalidSchema,
                  "if property predicates require a direct sibling property definition with const or enum: " +
                      property_name
              );
            }

            const auto& property_value = properties_obj.at(property_name);
            const auto& base_property_value = current_base_properties->at(property_name);

            if (property_value.is<picojson::object>() &&
                property_value.get<picojson::object>().count("contains")) {
              auto array_predicate_result = parse_array_contains_predicate(
                  property_name, property_value, current_base_properties
              );
              if (array_predicate_result.IsErr()) {
                return ResultErr(std::move(array_predicate_result).UnwrapErr());
              }
              auto array_predicate = std::move(array_predicate_result).Unwrap();
              if (!base_required_names.count(property_name)) {
                local_negative_reasons.push_back(make_absence_reason(property_name));
              }
              local_negative_reasons.push_back(array_predicate.negative_present_fragment);
              local_predicate_names.insert(property_name);
              continue;
            }

            if (property_value.is<picojson::object>()) {
              const auto& property_obj = property_value.get<picojson::object>();
              bool is_nested_object_predicate =
                  property_obj.count("required") || property_obj.count("properties");
              if (is_nested_object_predicate &&
                  !property_obj.count("const") && !property_obj.count("enum") &&
                  !property_obj.count("contains")) {
                if (!base_property_value.is<picojson::object>()) {
                  return ResultErr<SchemaError>(
                      SchemaErrorType::kInvalidSchema,
                      "if property predicates require a direct sibling property definition with const or enum: " +
                          property_name
                  );
                }
                const auto& base_property_obj = base_property_value.get<picojson::object>();
                auto nested_result = parse_object_condition(
                    property_obj, &base_property_obj, context + " property " + property_name
                );
                if (nested_result.IsErr()) {
                  return ResultErr(std::move(nested_result).UnwrapErr());
                }
                if (!base_required_names.count(property_name)) {
                  local_negative_reasons.push_back(make_absence_reason(property_name));
                }
                for (const auto& nested_reason : std::move(nested_result).Unwrap()) {
                  local_negative_reasons.push_back(
                      make_required_property_fragment(property_name, nested_reason)
                  );
                }
                local_predicate_names.insert(property_name);
                continue;
              }
            }

            auto positive_result = extract_finite_domain(property_value);
            if (positive_result.IsErr()) return ResultErr(std::move(positive_result).UnwrapErr());
            auto positive_values = std::move(positive_result).Unwrap();

            auto domain_result = extract_finite_domain(base_property_value);
            if (domain_result.IsErr()) {
              return ResultErr<SchemaError>(
                  SchemaErrorType::kInvalidSchema,
                  "if property predicates require a direct sibling property definition with const or enum: " +
                      property_name
              );
            }
            auto domain_values = std::move(domain_result).Unwrap();
            std::vector<std::string> negative_values;
            for (const auto& domain_value : domain_values) {
              if (std::find(positive_values.begin(), positive_values.end(), domain_value) ==
                  positive_values.end()) {
                negative_values.push_back(domain_value);
              }
            }

            if (!base_required_names.count(property_name)) {
              local_negative_reasons.push_back(make_absence_reason(property_name));
            }
            if (!negative_values.empty()) {
              local_negative_reasons.push_back(make_value_reason(property_name, negative_values));
            }
            local_predicate_names.insert(property_name);
          }
        }

        for (const auto& required_name : local_required_names) {
          if (!local_predicate_names.count(required_name) &&
              !base_required_names.count(required_name)) {
            local_negative_reasons.push_back(make_absence_reason(required_name));
          }
        }

        return ResultOk(std::move(local_negative_reasons));
      };

  auto negative_reasons_result = parse_object_condition(if_obj, &base_schema, "if");
  if (negative_reasons_result.IsErr()) {
    return ResultErr(std::move(negative_reasons_result).UnwrapErr());
  }
  std::vector<picojson::value> negative_reasons = std::move(negative_reasons_result).Unwrap();
  bool condition_implied_by_base = negative_reasons.empty();

  auto is_object_like_schema = [](const picojson::object& obj) {
    return obj.count("type") || obj.count("properties") || obj.count("required") ||
           obj.count("dependentRequired") || obj.count("not") || obj.count("patternProperties") ||
           obj.count("propertyNames") || obj.count("additionalProperties") ||
           obj.count("unevaluatedProperties") || obj.count("minProperties") ||
           obj.count("maxProperties");
  };
  if (!base_schema.empty() && !is_object_like_schema(base_schema)) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kInvalidSchema,
        "if/then/else is only supported on object schemas in this implementation"
    );
  }

  std::function<Result<bool, SchemaError>(
      const picojson::value&, std::vector<std::vector<std::string>>*)>
      parse_not_groups =
          [&](const picojson::value& value,
              std::vector<std::vector<std::string>>* groups) -> Result<bool, SchemaError> {
        if (!value.is<picojson::object>()) {
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema, "conditional object not must be an object"
          );
        }
        const auto& obj = value.get<picojson::object>();
        if (obj.count("required")) {
          for (const auto& key : obj.ordered_keys()) {
            if (key == "required" || key == "type" || kIgnoredKeys.count(key) != 0) {
              continue;
            }
            return ResultErr<SchemaError>(
                SchemaErrorType::kInvalidSchema,
                "conditional not only supports required or anyOf of required groups"
            );
          }
          if (!obj.at("required").is<picojson::array>()) {
            return ResultErr<SchemaError>(
                SchemaErrorType::kInvalidSchema, "conditional not required must be an array"
            );
          }
          std::vector<std::string> group;
          for (const auto& item : obj.at("required").get<picojson::array>()) {
            if (!item.is<std::string>()) {
              return ResultErr<SchemaError>(
                  SchemaErrorType::kInvalidSchema,
                  "conditional not required must contain only strings"
              );
            }
            const auto& name = item.get<std::string>();
            if (std::find(group.begin(), group.end(), name) == group.end()) {
              group.push_back(name);
            }
          }
          groups->push_back(std::move(group));
          return ResultOk(true);
        }
        if (!obj.count("anyOf") || !obj.at("anyOf").is<picojson::array>()) {
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema,
              "conditional not only supports required or anyOf of required groups"
          );
        }
        for (const auto& key : obj.ordered_keys()) {
          if (key == "anyOf" || key == "type" || kIgnoredKeys.count(key) != 0) {
            continue;
          }
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema,
              "conditional not only supports required or anyOf of required groups"
          );
        }
        for (const auto& option : obj.at("anyOf").get<picojson::array>()) {
          auto nested_result = parse_not_groups(option, groups);
          if (nested_result.IsErr()) return nested_result;
        }
        return ResultOk(true);
      };

  auto apply_object_fragment =
      [&](picojson::object* branch,
          const picojson::value& fragment_value,
          const std::string& context) -> Result<bool, SchemaError> {
        if (!fragment_value.is<picojson::object>()) {
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema, context + " must be an object"
          );
        }
        const auto& fragment = fragment_value.get<picojson::object>();
        if (fragment.count("type")) {
          if (!fragment.at("type").is<std::string>() ||
              fragment.at("type").get<std::string>() != "object") {
            return ResultErr<SchemaError>(
                SchemaErrorType::kInvalidSchema, context + " type must be object"
            );
          }
        }
        for (const auto& key : fragment.ordered_keys()) {
          if (key == "type" || key == "properties" || key == "required" ||
              key == "dependentRequired" || key == "not" || key == "minProperties" ||
              key == "maxProperties" || kIgnoredKeys.count(key) != 0) {
            continue;
          }
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema,
              context + " only supports object required, properties, dependentRequired, not, "
                        "minProperties and maxProperties"
          );
        }

        if (fragment.count("properties")) {
          if (!fragment.at("properties").is<picojson::object>()) {
            return ResultErr<SchemaError>(
                SchemaErrorType::kInvalidSchema, context + " properties must be an object"
            );
          }
          picojson::object branch_properties;
          if (branch->count("properties")) {
            branch_properties = branch->at("properties").get<picojson::object>();
          }
          const auto& fragment_properties = fragment.at("properties").get<picojson::object>();
          for (const auto& property_name : fragment_properties.ordered_keys()) {
            picojson::value normalized_fragment_property =
                NormalizePartialObjectFragment(fragment_properties.at(property_name));
            if (branch_properties.count(property_name)) {
              picojson::array all_of = {
                  branch_properties.at(property_name), normalized_fragment_property};
              picojson::object merged_property;
              merged_property["allOf"] = picojson::value(all_of);
              branch_properties[property_name] = picojson::value(merged_property);
            } else {
              branch_properties[property_name] = normalized_fragment_property;
            }
          }
          (*branch)["properties"] = picojson::value(branch_properties);
        }

        if (fragment.count("required")) {
          if (!fragment.at("required").is<picojson::array>()) {
            return ResultErr<SchemaError>(
                SchemaErrorType::kInvalidSchema, context + " required must be an array"
            );
          }
          std::vector<std::string> names;
          if (branch->count("required")) {
            for (const auto& item : branch->at("required").get<picojson::array>()) {
              names.push_back(item.get<std::string>());
            }
          }
          for (const auto& item : fragment.at("required").get<picojson::array>()) {
            if (!item.is<std::string>()) {
              return ResultErr<SchemaError>(
                  SchemaErrorType::kInvalidSchema,
                  context + " required must contain only strings"
              );
            }
            const auto& name = item.get<std::string>();
            if (std::find(names.begin(), names.end(), name) == names.end()) {
              names.push_back(name);
            }
          }
          (*branch)["required"] = picojson::value(make_required_array(names));
        }

        if (fragment.count("dependentRequired")) {
          if (!fragment.at("dependentRequired").is<picojson::object>()) {
            return ResultErr<SchemaError>(
                SchemaErrorType::kInvalidSchema,
                context + " dependentRequired must be an object"
            );
          }
          picojson::object merged_dependencies;
          if (branch->count("dependentRequired")) {
            merged_dependencies = branch->at("dependentRequired").get<picojson::object>();
          }
          const auto& fragment_dependencies =
              fragment.at("dependentRequired").get<picojson::object>();
          for (const auto& trigger : fragment_dependencies.ordered_keys()) {
            if (!fragment_dependencies.at(trigger).is<picojson::array>()) {
              return ResultErr<SchemaError>(
                  SchemaErrorType::kInvalidSchema,
                  context + " dependentRequired values must be arrays"
              );
            }
            std::vector<std::string> deps;
            if (merged_dependencies.count(trigger)) {
              for (const auto& item : merged_dependencies.at(trigger).get<picojson::array>()) {
                deps.push_back(item.get<std::string>());
              }
            }
            for (const auto& item : fragment_dependencies.at(trigger).get<picojson::array>()) {
              if (!item.is<std::string>()) {
                return ResultErr<SchemaError>(
                    SchemaErrorType::kInvalidSchema,
                    context + " dependentRequired arrays must contain only strings"
                );
              }
              const auto& dep = item.get<std::string>();
              if (std::find(deps.begin(), deps.end(), dep) == deps.end()) {
                deps.push_back(dep);
              }
            }
            merged_dependencies[trigger] = picojson::value(make_required_array(deps));
          }
          (*branch)["dependentRequired"] = picojson::value(merged_dependencies);
        }

        if (fragment.count("minProperties")) {
          int64_t value = fragment.at("minProperties").get<int64_t>();
          if (branch->count("minProperties")) {
            value = std::max(value, branch->at("minProperties").get<int64_t>());
          }
          (*branch)["minProperties"] = picojson::value(value);
        }
        if (fragment.count("maxProperties")) {
          int64_t value = fragment.at("maxProperties").get<int64_t>();
          if (branch->count("maxProperties")) {
            value = std::min(value, branch->at("maxProperties").get<int64_t>());
          }
          (*branch)["maxProperties"] = picojson::value(value);
        }

        if (fragment.count("not")) {
          std::vector<std::vector<std::string>> groups;
          if (branch->count("not")) {
            auto existing_result = parse_not_groups(branch->at("not"), &groups);
            if (existing_result.IsErr()) return existing_result;
          }
          auto fragment_result = parse_not_groups(fragment.at("not"), &groups);
          if (fragment_result.IsErr()) return fragment_result;
          if (groups.size() == 1) {
            picojson::object not_obj;
            not_obj["required"] = picojson::value(make_required_array(groups[0]));
            (*branch)["not"] = picojson::value(not_obj);
          } else if (!groups.empty()) {
            picojson::array any_of;
            for (const auto& group : groups) {
              picojson::object required_obj;
              required_obj["required"] = picojson::value(make_required_array(group));
              any_of.push_back(picojson::value(required_obj));
            }
            picojson::object not_obj;
            not_obj["anyOf"] = picojson::value(any_of);
            (*branch)["not"] = picojson::value(not_obj);
          }
        }

        return ResultOk(true);
      };

  std::function<Result<std::vector<picojson::value>, SchemaError>(
      const picojson::value&, const std::string&)>
      expand_object_fragment_alternatives =
          [&](const picojson::value& fragment_value,
              const std::string& context) -> Result<std::vector<picojson::value>, SchemaError> {
        if (!fragment_value.is<picojson::object>()) {
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema, context + " must be an object"
          );
        }
        const auto& fragment = fragment_value.get<picojson::object>();
        if (!fragment.count("anyOf")) {
          return ResultOk(std::vector<picojson::value>{fragment_value});
        }
        if (!fragment.at("anyOf").is<picojson::array>()) {
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema, context + " anyOf must be an array"
          );
        }

        picojson::object shared_fragment = fragment;
        shared_fragment.erase("anyOf");

        std::vector<picojson::value> expanded_options;
        const auto& any_of = fragment.at("anyOf").get<picojson::array>();
        for (size_t idx = 0; idx < any_of.size(); ++idx) {
          auto nested_result = expand_object_fragment_alternatives(
              any_of[idx], context + " anyOf[" + std::to_string(idx) + "]"
          );
          if (nested_result.IsErr()) return ResultErr(std::move(nested_result).UnwrapErr());
          for (const auto& nested_fragment : std::move(nested_result).Unwrap()) {
            picojson::object merged_fragment;
            auto shared_apply_result = apply_object_fragment(
                &merged_fragment, picojson::value(shared_fragment), context
            );
            if (shared_apply_result.IsErr()) {
              return ResultErr(std::move(shared_apply_result).UnwrapErr());
            }
            auto option_apply_result = apply_object_fragment(
                &merged_fragment, nested_fragment, context + " anyOf"
            );
            if (option_apply_result.IsErr()) {
              return ResultErr(std::move(option_apply_result).UnwrapErr());
            }
            expanded_options.push_back(picojson::value(merged_fragment));
          }
        }

        return ResultOk(std::move(expanded_options));
      };

  auto make_branch = [&](const std::vector<picojson::value>& fragments,
                         const std::string& branch_hint) -> Result<picojson::value, SchemaError> {
    picojson::object branch = base_schema;
    if (!branch.count("type")) {
      branch["type"] = picojson::value("object");
    }
    for (const auto& fragment : fragments) {
      auto apply_result = apply_object_fragment(&branch, fragment, branch_hint);
      if (apply_result.IsErr()) return ResultErr(std::move(apply_result).UnwrapErr());
    }
    return ResultOk(picojson::value(branch));
  };

  std::vector<SchemaSpecPtr> branch_options;
  size_t branch_index = 0;
  auto append_branch_option =
      [&](const picojson::value& branch_schema) -> Result<bool, SchemaError> {
    auto parse_result =
        Parse(branch_schema, rule_name_hint + "_branch_" + std::to_string(branch_index++));
    if (parse_result.IsErr()) {
      auto err = std::move(parse_result).UnwrapErr();
      if (err.Type() == SchemaErrorType::kUnsatisfiableSchema) {
        return ResultOk(false);
      }
      return ResultErr<SchemaError>(std::move(err));
    }
    branch_options.push_back(std::move(parse_result).Unwrap());
    return ResultOk(true);
  };
  {
    std::vector<picojson::value> positive_alternatives = {picojson::value(picojson::object{})};
    if (schema.count("then")) {
      auto expand_result = expand_object_fragment_alternatives(schema.at("then"), "then");
      if (expand_result.IsErr()) return ResultErr(std::move(expand_result).UnwrapErr());
      positive_alternatives = std::move(expand_result).Unwrap();
    }
    for (const auto& positive_fragment : positive_alternatives) {
      std::vector<picojson::value> positive_parts;
      if (!condition_implied_by_base) {
        positive_parts.push_back(if_value);
      }
      if (positive_fragment.is<picojson::object>() && !positive_fragment.get<picojson::object>().empty()) {
        positive_parts.push_back(positive_fragment);
      }
      auto branch_result = make_branch(positive_parts, "then");
      if (branch_result.IsErr()) return ResultErr(std::move(branch_result).UnwrapErr());
      auto append_result = append_branch_option(std::move(branch_result).Unwrap());
      if (append_result.IsErr()) return ResultErr(std::move(append_result).UnwrapErr());
    }
  }
  std::vector<picojson::value> else_alternatives = {picojson::value(picojson::object{})};
  if (schema.count("else")) {
    auto expand_result = expand_object_fragment_alternatives(schema.at("else"), "else");
    if (expand_result.IsErr()) return ResultErr(std::move(expand_result).UnwrapErr());
    else_alternatives = std::move(expand_result).Unwrap();
  }
  for (const auto& negative_reason : negative_reasons) {
    for (const auto& else_fragment : else_alternatives) {
      std::vector<picojson::value> negative_parts = {negative_reason};
      if (else_fragment.is<picojson::object>() && !else_fragment.get<picojson::object>().empty()) {
        negative_parts.push_back(else_fragment);
      }
      auto branch_result = make_branch(negative_parts, "else");
      if (branch_result.IsErr()) return ResultErr(std::move(branch_result).UnwrapErr());
      auto append_result = append_branch_option(std::move(branch_result).Unwrap());
      if (append_result.IsErr()) return ResultErr(std::move(append_result).UnwrapErr());
    }
  }

  if (branch_options.empty()) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema,
        "if/then/else branches have no satisfiable intersection with sibling constraints"
    );
  }
  if (branch_options.size() == 1) {
    return ResultOk(std::move(branch_options.front()));
  }
  AnyOfSpec conditional_spec;
  conditional_spec.options = std::move(branch_options);
  return ResultOk(SchemaSpec::Make(std::move(conditional_spec), "", rule_name_hint));
}

Result<SchemaSpecPtr, SchemaError> SchemaParser::MergeAllOfSchemas(
    const std::vector<SchemaSpecPtr>& schemas, const std::string& rule_name_hint
) {
  if (schemas.empty()) {
    return ResultOk(SchemaSpec::Make(AnySpec{}, "", rule_name_hint));
  }
  auto merged = schemas[0];
  for (size_t i = 1; i < schemas.size(); ++i) {
    auto merge_result =
        MergeSchemaSpecs(merged, schemas[i], rule_name_hint + "_merge_" + std::to_string(i));
    if (merge_result.IsErr()) return ResultErr(std::move(merge_result).UnwrapErr());
    merged = std::move(merge_result).Unwrap();
  }
  return ResultOk(merged);
}

Result<ObjectSpec, SchemaError> SchemaParser::MergeObjectSpecs(
    const ObjectSpec& lhs, const ObjectSpec& rhs, const std::string& rule_name_hint
) {
  if (!lhs.pattern_properties.empty() || !rhs.pattern_properties.empty() || lhs.property_names ||
      rhs.property_names) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kInvalidSchema,
        "allOf merging objects with patternProperties or propertyNames is not supported"
    );
  }

  auto make_any_schema = []() { return SchemaSpec::Make(AnySpec{}, "", "any"); };
  auto get_fallback_schema = [&](const ObjectSpec& spec) -> std::optional<SchemaSpecPtr> {
    if (spec.allow_additional_properties) {
      return spec.additional_properties_schema ? spec.additional_properties_schema : make_any_schema();
    }
    if (spec.allow_unevaluated_properties) {
      return spec.unevaluated_properties_schema ? spec.unevaluated_properties_schema : make_any_schema();
    }
    return std::nullopt;
  };
  auto get_property_schema =
      [&](const ObjectSpec& spec, const std::string& name) -> std::optional<SchemaSpecPtr> {
    for (const auto& property : spec.properties) {
      if (property.name == name) {
        return property.schema;
      }
    }
    return get_fallback_schema(spec);
  };
  auto append_unique_name = [](std::vector<std::string>* names, const std::string& name) {
    if (std::find(names->begin(), names->end(), name) == names->end()) {
      names->push_back(name);
    }
  };

  ObjectSpec merged;
  std::vector<std::string> property_names;
  property_names.reserve(lhs.properties.size() + rhs.properties.size());
  for (const auto& property : lhs.properties) {
    append_unique_name(&property_names, property.name);
  }
  for (const auto& property : rhs.properties) {
    append_unique_name(&property_names, property.name);
  }

  for (const auto& name : property_names) {
    bool is_required = lhs.required.count(name) || rhs.required.count(name);
    auto lhs_schema = get_property_schema(lhs, name);
    auto rhs_schema = get_property_schema(rhs, name);
    if (!lhs_schema || !rhs_schema) {
      if (is_required) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kUnsatisfiableSchema,
            "allOf required object property \"" + name + "\" is forbidden by another schema"
        );
      }
      continue;
    }

    auto merge_result = MergeSchemaSpecs(*lhs_schema, *rhs_schema, rule_name_hint + "_" + name);
    if (merge_result.IsErr()) {
      auto err = std::move(merge_result).UnwrapErr();
      if (err.Type() == SchemaErrorType::kUnsatisfiableSchema && !is_required) {
        continue;
      }
      return ResultErr<SchemaError>(std::move(err));
    }

    merged.properties.push_back({name, std::move(merge_result).Unwrap()});
    if (is_required) {
      merged.required.insert(name);
    }
  }

  for (const auto& [trigger, deps] : lhs.dependent_required) {
    auto& merged_deps = merged.dependent_required[trigger];
    for (const auto& dep : deps) {
      append_unique_name(&merged_deps, dep);
    }
  }
  for (const auto& [trigger, deps] : rhs.dependent_required) {
    auto& merged_deps = merged.dependent_required[trigger];
    for (const auto& dep : deps) {
      append_unique_name(&merged_deps, dep);
    }
  }

  for (const auto& group : lhs.forbidden_groups) {
    merged.forbidden_groups.push_back(group);
  }
  for (const auto& group : rhs.forbidden_groups) {
    merged.forbidden_groups.push_back(group);
  }

  auto lhs_fallback = get_fallback_schema(lhs);
  auto rhs_fallback = get_fallback_schema(rhs);
  std::optional<SchemaSpecPtr> merged_fallback;
  if (lhs_fallback && rhs_fallback) {
    auto merge_result =
        MergeSchemaSpecs(*lhs_fallback, *rhs_fallback, rule_name_hint + "_additional");
    if (merge_result.IsErr()) {
      auto err = std::move(merge_result).UnwrapErr();
      if (err.Type() != SchemaErrorType::kUnsatisfiableSchema) {
        return ResultErr<SchemaError>(std::move(err));
      }
    } else {
      merged_fallback = std::move(merge_result).Unwrap();
    }
  }

  merged.min_properties = std::max(lhs.min_properties, rhs.min_properties);
  if (lhs.max_properties == -1) {
    merged.max_properties = rhs.max_properties;
  } else if (rhs.max_properties == -1) {
    merged.max_properties = lhs.max_properties;
  } else {
    merged.max_properties = std::min(lhs.max_properties, rhs.max_properties);
  }
  if (merged.max_properties != -1 && merged.min_properties > merged.max_properties) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema, "allOf object property count constraints conflict"
    );
  }
  if (merged.max_properties != -1 &&
      static_cast<int>(merged.required.size()) > merged.max_properties) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema,
        "maxProperties is less than the number of required properties after allOf merge"
    );
  }
  if (!merged_fallback && merged.min_properties > static_cast<int>(merged.properties.size())) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema,
        "minProperties is greater than the number of satisfiable properties after allOf merge"
    );
  }

  merged.allow_additional_properties = merged_fallback.has_value();
  merged.additional_properties_schema = nullptr;
  merged.allow_unevaluated_properties = merged_fallback.has_value();
  merged.unevaluated_properties_schema = nullptr;
  if (merged_fallback && !std::holds_alternative<AnySpec>((*merged_fallback)->spec)) {
    merged.additional_properties_schema = *merged_fallback;
  }

  return ResultOk(std::move(merged));
}

Result<ArraySpec, SchemaError> SchemaParser::MergeArraySpecs(
    const ArraySpec& lhs, const ArraySpec& rhs, const std::string& rule_name_hint
) {
  auto make_any_schema = []() { return SchemaSpec::Make(AnySpec{}, "", "any"); };
  auto get_item_schema = [&](const ArraySpec& spec, size_t index) -> std::optional<SchemaSpecPtr> {
    if (index < spec.prefix_items.size()) {
      return spec.prefix_items[index];
    }
    if (spec.allow_additional_items) {
      return spec.additional_items ? spec.additional_items : make_any_schema();
    }
    return std::nullopt;
  };
  auto get_effective_min_items = [](const ArraySpec& spec) {
    return std::max(spec.min_items, static_cast<int64_t>(spec.prefix_items.size()));
  };
  auto get_effective_max_items = [](const ArraySpec& spec) {
    return spec.allow_additional_items ? spec.max_items : static_cast<int64_t>(spec.prefix_items.size());
  };

  ArraySpec merged;
  size_t merged_prefix_size = std::max(lhs.prefix_items.size(), rhs.prefix_items.size());
  for (size_t i = 0; i < merged_prefix_size; ++i) {
    auto lhs_item = get_item_schema(lhs, i);
    auto rhs_item = get_item_schema(rhs, i);
    if (!lhs_item || !rhs_item) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema, "allOf array prefixItems conflict"
      );
    }

    auto merge_result =
        MergeSchemaSpecs(*lhs_item, *rhs_item, rule_name_hint + "_item_" + std::to_string(i));
    if (merge_result.IsErr()) return ResultErr(std::move(merge_result).UnwrapErr());
    merged.prefix_items.push_back(std::move(merge_result).Unwrap());
  }

  int64_t lhs_effective_min_items = get_effective_min_items(lhs);
  int64_t rhs_effective_min_items = get_effective_min_items(rhs);
  int64_t merged_effective_min_items = std::max(lhs_effective_min_items, rhs_effective_min_items);

  int64_t lhs_effective_max_items = get_effective_max_items(lhs);
  int64_t rhs_effective_max_items = get_effective_max_items(rhs);
  if (lhs_effective_max_items == -1) {
    merged.max_items = rhs_effective_max_items;
  } else if (rhs_effective_max_items == -1) {
    merged.max_items = lhs_effective_max_items;
  } else {
    merged.max_items = std::min(lhs_effective_max_items, rhs_effective_max_items);
  }

  merged.allow_additional_items = lhs.allow_additional_items && rhs.allow_additional_items;
  merged.additional_items = nullptr;
  if (merged.allow_additional_items) {
    auto lhs_additional = get_item_schema(lhs, merged_prefix_size);
    auto rhs_additional = get_item_schema(rhs, merged_prefix_size);
    XGRAMMAR_DCHECK(lhs_additional.has_value() && rhs_additional.has_value());

    auto merge_result =
        MergeSchemaSpecs(*lhs_additional, *rhs_additional, rule_name_hint + "_additional");
    if (merge_result.IsErr()) {
      auto err = std::move(merge_result).UnwrapErr();
      if (err.Type() != SchemaErrorType::kUnsatisfiableSchema) {
        return ResultErr<SchemaError>(std::move(err));
      }
      merged.allow_additional_items = false;
    } else {
      merged.additional_items = std::move(merge_result).Unwrap();
    }
  }

  if (!merged.allow_additional_items) {
    int64_t exact_item_count = static_cast<int64_t>(merged.prefix_items.size());
    if (merged_effective_min_items > exact_item_count ||
        (merged.max_items != -1 && exact_item_count > merged.max_items)) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema, "allOf array item count constraints conflict"
      );
    }
    merged.min_items = exact_item_count;
    merged.max_items = exact_item_count;
    return ResultOk(std::move(merged));
  }

  merged.min_items = std::max(
      merged_effective_min_items, static_cast<int64_t>(merged.prefix_items.size())
  );
  if (merged.max_items != -1 && merged.min_items > merged.max_items) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema, "allOf array item count constraints conflict"
    );
  }

  merged.min_contains = std::max(lhs.min_contains, rhs.min_contains);
  if (merged.min_contains > 1) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kInvalidSchema, "contains only supports minContains == 1"
    );
  }

  if (lhs.contains_item || rhs.contains_item) {
    if (!merged.prefix_items.empty() || !merged.allow_additional_items) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema,
          "contains is only supported for homogeneous arrays without prefixItems"
      );
    }
    SchemaSpecPtr contains_spec = lhs.contains_item ? lhs.contains_item : rhs.contains_item;
    if (lhs.contains_item && rhs.contains_item) {
      auto contains_merge =
          MergeSchemaSpecs(lhs.contains_item, rhs.contains_item, rule_name_hint + "_contains");
      if (contains_merge.IsErr()) return ResultErr(std::move(contains_merge).UnwrapErr());
      contains_spec = std::move(contains_merge).Unwrap();
    }
    if (merged.additional_items) {
      auto contains_merge = MergeSchemaSpecs(
          merged.additional_items, contains_spec, rule_name_hint + "_contains_additional"
      );
      if (contains_merge.IsErr()) return ResultErr(std::move(contains_merge).UnwrapErr());
      merged.contains_item = std::move(contains_merge).Unwrap();
    } else {
      merged.contains_item = std::move(contains_spec);
    }
  }

  if (merged.min_contains > 0 && merged.max_items != -1 &&
      std::max(merged.min_items, merged.min_contains) > merged.max_items) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema, "contains conflicts with array item count constraints"
    );
  }

  return ResultOk(std::move(merged));
}

Result<SchemaSpecPtr, SchemaError> SchemaParser::MergeSchemaSpecs(
    const SchemaSpecPtr& lhs, const SchemaSpecPtr& rhs, const std::string& rule_name_hint
) {
  if (std::holds_alternative<AnySpec>(lhs->spec)) return ResultOk(rhs);
  if (std::holds_alternative<AnySpec>(rhs->spec)) return ResultOk(lhs);

  auto resolve_if_needed = [&](const SchemaSpecPtr& spec) -> Result<SchemaSpecPtr, SchemaError> {
    if (std::holds_alternative<RefSpec>(spec->spec)) {
      return ResolveRef(std::get<RefSpec>(spec->spec).uri, rule_name_hint + "_ref");
    }
    if (std::holds_alternative<AllOfSpec>(spec->spec)) {
      return MergeAllOfSchemas(std::get<AllOfSpec>(spec->spec).schemas, rule_name_hint + "_allof");
    }
    return ResultOk(spec);
  };

  auto lhs_resolved = resolve_if_needed(lhs);
  if (lhs_resolved.IsErr()) return ResultErr(std::move(lhs_resolved).UnwrapErr());
  auto rhs_resolved = resolve_if_needed(rhs);
  if (rhs_resolved.IsErr()) return ResultErr(std::move(rhs_resolved).UnwrapErr());

  auto lhs_spec = std::move(lhs_resolved).Unwrap();
  auto rhs_spec = std::move(rhs_resolved).Unwrap();

  auto merge_disjunction = [&](const std::vector<SchemaSpecPtr>& options,
                               const SchemaSpecPtr& other) -> Result<SchemaSpecPtr, SchemaError> {
    AnyOfSpec merged_anyof;
    for (size_t i = 0; i < options.size(); ++i) {
      auto merge_result =
          MergeSchemaSpecs(options[i], other, rule_name_hint + "_option_" + std::to_string(i));
      if (merge_result.IsOk()) {
        merged_anyof.options.push_back(std::move(merge_result).Unwrap());
      }
    }
    if (merged_anyof.options.empty()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema, "allOf options have no satisfiable intersection"
      );
    }
    if (merged_anyof.options.size() == 1) {
      return ResultOk(merged_anyof.options[0]);
    }
    return ResultOk(SchemaSpec::Make(std::move(merged_anyof), "", rule_name_hint));
  };

  auto merge_exclusive_disjunction =
      [&](const std::vector<SchemaSpecPtr>& options,
          const SchemaSpecPtr& other) -> Result<SchemaSpecPtr, SchemaError> {
    OneOfSpec merged_oneof;
    for (size_t i = 0; i < options.size(); ++i) {
      auto merge_result =
          MergeSchemaSpecs(options[i], other, rule_name_hint + "_option_" + std::to_string(i));
      if (merge_result.IsOk()) {
        merged_oneof.options.push_back(std::move(merge_result).Unwrap());
      } else {
        auto err = std::move(merge_result).UnwrapErr();
        if (err.Type() != SchemaErrorType::kUnsatisfiableSchema) {
          return ResultErr<SchemaError>(std::move(err));
        }
      }
    }
    if (merged_oneof.options.empty()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema, "oneOf options have no satisfiable intersection"
      );
    }
    if (merged_oneof.options.size() == 1) {
      return ResultOk(merged_oneof.options[0]);
    }
    return ResultOk(SchemaSpec::Make(std::move(merged_oneof), "", rule_name_hint));
  };

  auto merge_two_exclusive_disjunctions =
      [&](const std::vector<SchemaSpecPtr>& lhs_options,
          const std::vector<SchemaSpecPtr>& rhs_options) -> Result<SchemaSpecPtr, SchemaError> {
    OneOfSpec merged_oneof;
    for (size_t i = 0; i < lhs_options.size(); ++i) {
      for (size_t j = 0; j < rhs_options.size(); ++j) {
        auto merge_result = MergeSchemaSpecs(
            lhs_options[i],
            rhs_options[j],
            rule_name_hint + "_lhs_" + std::to_string(i) + "_rhs_" + std::to_string(j)
        );
        if (merge_result.IsOk()) {
          merged_oneof.options.push_back(std::move(merge_result).Unwrap());
        } else {
          auto err = std::move(merge_result).UnwrapErr();
          if (err.Type() != SchemaErrorType::kUnsatisfiableSchema) {
            return ResultErr<SchemaError>(std::move(err));
          }
        }
      }
    }
    if (merged_oneof.options.empty()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema, "allOf oneOf options have no satisfiable intersection"
      );
    }
    if (merged_oneof.options.size() == 1) {
      return ResultOk(merged_oneof.options[0]);
    }
    return ResultOk(SchemaSpec::Make(std::move(merged_oneof), "", rule_name_hint));
  };

  auto merge_integer_with_number =
      [&](const IntegerSpec& integer_spec,
          const NumberSpec& number_spec) -> Result<SchemaSpecPtr, SchemaError> {
    int64_t lower = integer_spec.minimum.value_or(std::numeric_limits<int64_t>::min());
    int64_t upper = integer_spec.maximum.value_or(std::numeric_limits<int64_t>::max());
    if (integer_spec.exclusive_minimum.has_value()) {
      lower = std::max(lower, integer_spec.exclusive_minimum.value() + 1);
    }
    if (integer_spec.exclusive_maximum.has_value()) {
      upper = std::min(upper, integer_spec.exclusive_maximum.value() - 1);
    }

    constexpr double kMinInt64AsDouble =
        static_cast<double>(std::numeric_limits<int64_t>::min());
    constexpr double kMaxInt64AsDouble =
        static_cast<double>(std::numeric_limits<int64_t>::max());

    auto apply_number_lower_bound = [&](double bound, bool exclusive) {
      double candidate = exclusive ? std::floor(bound) + 1.0 : std::ceil(bound);
      if (candidate > kMaxInt64AsDouble) {
        lower = 1;
        upper = 0;
        return;
      }
      if (candidate > kMinInt64AsDouble) {
        lower = std::max(lower, static_cast<int64_t>(candidate));
      }
    };
    auto apply_number_upper_bound = [&](double bound, bool exclusive) {
      double candidate = exclusive ? std::ceil(bound) - 1.0 : std::floor(bound);
      if (candidate < kMinInt64AsDouble) {
        lower = 1;
        upper = 0;
        return;
      }
      if (candidate < kMaxInt64AsDouble) {
        upper = std::min(upper, static_cast<int64_t>(candidate));
      }
    };

    if (number_spec.minimum.has_value()) {
      apply_number_lower_bound(number_spec.minimum.value(), false);
    }
    if (number_spec.exclusive_minimum.has_value()) {
      apply_number_lower_bound(number_spec.exclusive_minimum.value(), true);
    }
    if (number_spec.maximum.has_value()) {
      apply_number_upper_bound(number_spec.maximum.value(), false);
    }
    if (number_spec.exclusive_maximum.has_value()) {
      apply_number_upper_bound(number_spec.exclusive_maximum.value(), true);
    }

    if (lower > upper) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema, "allOf integer/number constraints conflict"
      );
    }

    IntegerSpec merged;
    if (lower != std::numeric_limits<int64_t>::min()) {
      merged.minimum = lower;
    }
    if (upper != std::numeric_limits<int64_t>::max()) {
      merged.maximum = upper;
    }
    return ResultOk(SchemaSpec::Make(std::move(merged), "", rule_name_hint));
  };

  if (std::holds_alternative<AnyOfSpec>(lhs_spec->spec)) {
    return merge_disjunction(std::get<AnyOfSpec>(lhs_spec->spec).options, rhs_spec);
  }
  if (std::holds_alternative<AnyOfSpec>(rhs_spec->spec)) {
    return merge_disjunction(std::get<AnyOfSpec>(rhs_spec->spec).options, lhs_spec);
  }
  if (std::holds_alternative<OneOfSpec>(lhs_spec->spec) &&
      std::holds_alternative<OneOfSpec>(rhs_spec->spec)) {
    return merge_two_exclusive_disjunctions(
        std::get<OneOfSpec>(lhs_spec->spec).options, std::get<OneOfSpec>(rhs_spec->spec).options
    );
  }
  if (std::holds_alternative<OneOfSpec>(lhs_spec->spec)) {
    return merge_exclusive_disjunction(std::get<OneOfSpec>(lhs_spec->spec).options, rhs_spec);
  }
  if (std::holds_alternative<OneOfSpec>(rhs_spec->spec)) {
    return merge_exclusive_disjunction(std::get<OneOfSpec>(rhs_spec->spec).options, lhs_spec);
  }
  if (std::holds_alternative<TypeArraySpec>(lhs_spec->spec)) {
    return merge_disjunction(std::get<TypeArraySpec>(lhs_spec->spec).type_schemas, rhs_spec);
  }
  if (std::holds_alternative<TypeArraySpec>(rhs_spec->spec)) {
    return merge_disjunction(std::get<TypeArraySpec>(rhs_spec->spec).type_schemas, lhs_spec);
  }

  if (std::holds_alternative<IntegerSpec>(lhs_spec->spec) &&
      std::holds_alternative<IntegerSpec>(rhs_spec->spec)) {
    const auto& a = std::get<IntegerSpec>(lhs_spec->spec);
    const auto& b = std::get<IntegerSpec>(rhs_spec->spec);
    IntegerSpec merged;

    auto apply_lower = [](std::optional<int64_t> min_value,
                          std::optional<int64_t> exclusive_value,
                          std::optional<int64_t>* out_minimum,
                          std::optional<int64_t>* out_exclusive_minimum) {
      if (min_value.has_value()) {
        if (!out_minimum->has_value() && !out_exclusive_minimum->has_value()) {
          *out_minimum = min_value;
        } else if (out_exclusive_minimum->has_value()) {
          if (min_value.value() > out_exclusive_minimum->value()) {
            *out_minimum = min_value;
            *out_exclusive_minimum = std::nullopt;
          }
        } else if (min_value.value() > out_minimum->value()) {
          *out_minimum = min_value;
        }
      }
      if (exclusive_value.has_value()) {
        if (!out_minimum->has_value() && !out_exclusive_minimum->has_value()) {
          *out_exclusive_minimum = exclusive_value;
        } else if (out_exclusive_minimum->has_value()) {
          if (exclusive_value.value() >= out_exclusive_minimum->value()) {
            *out_exclusive_minimum = exclusive_value;
            *out_minimum = std::nullopt;
          }
        } else if (exclusive_value.value() > out_minimum->value()) {
          *out_exclusive_minimum = exclusive_value;
          *out_minimum = std::nullopt;
        } else if (exclusive_value.value() == out_minimum->value()) {
          *out_exclusive_minimum = exclusive_value;
          *out_minimum = std::nullopt;
        }
      }
    };
    auto apply_upper = [](std::optional<int64_t> max_value,
                          std::optional<int64_t> exclusive_value,
                          std::optional<int64_t>* out_maximum,
                          std::optional<int64_t>* out_exclusive_maximum) {
      if (max_value.has_value()) {
        if (!out_maximum->has_value() && !out_exclusive_maximum->has_value()) {
          *out_maximum = max_value;
        } else if (out_exclusive_maximum->has_value()) {
          if (max_value.value() < out_exclusive_maximum->value()) {
            *out_maximum = max_value;
            *out_exclusive_maximum = std::nullopt;
          }
        } else if (max_value.value() < out_maximum->value()) {
          *out_maximum = max_value;
        }
      }
      if (exclusive_value.has_value()) {
        if (!out_maximum->has_value() && !out_exclusive_maximum->has_value()) {
          *out_exclusive_maximum = exclusive_value;
        } else if (out_exclusive_maximum->has_value()) {
          if (exclusive_value.value() <= out_exclusive_maximum->value()) {
            *out_exclusive_maximum = exclusive_value;
            *out_maximum = std::nullopt;
          }
        } else if (exclusive_value.value() < out_maximum->value()) {
          *out_exclusive_maximum = exclusive_value;
          *out_maximum = std::nullopt;
        } else if (exclusive_value.value() == out_maximum->value()) {
          *out_exclusive_maximum = exclusive_value;
          *out_maximum = std::nullopt;
        }
      }
    };

    apply_lower(a.minimum, a.exclusive_minimum, &merged.minimum, &merged.exclusive_minimum);
    apply_lower(b.minimum, b.exclusive_minimum, &merged.minimum, &merged.exclusive_minimum);
    apply_upper(a.maximum, a.exclusive_maximum, &merged.maximum, &merged.exclusive_maximum);
    apply_upper(b.maximum, b.exclusive_maximum, &merged.maximum, &merged.exclusive_maximum);

    int64_t effective_min =
        merged.exclusive_minimum.value_or(merged.minimum.value_or(std::numeric_limits<int64_t>::min()));
    int64_t effective_max =
        merged.exclusive_maximum.value_or(merged.maximum.value_or(std::numeric_limits<int64_t>::max()));
    if (effective_min > effective_max ||
        (effective_min == effective_max &&
         (merged.exclusive_minimum.has_value() || merged.exclusive_maximum.has_value()))) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema, "allOf integer constraints conflict"
      );
    }
    return ResultOk(SchemaSpec::Make(std::move(merged), "", rule_name_hint));
  }

  if (std::holds_alternative<IntegerSpec>(lhs_spec->spec) &&
      std::holds_alternative<NumberSpec>(rhs_spec->spec)) {
    return merge_integer_with_number(
        std::get<IntegerSpec>(lhs_spec->spec), std::get<NumberSpec>(rhs_spec->spec)
    );
  }
  if (std::holds_alternative<NumberSpec>(lhs_spec->spec) &&
      std::holds_alternative<IntegerSpec>(rhs_spec->spec)) {
    return merge_integer_with_number(
        std::get<IntegerSpec>(rhs_spec->spec), std::get<NumberSpec>(lhs_spec->spec)
    );
  }

  if (std::holds_alternative<NumberSpec>(lhs_spec->spec) &&
      std::holds_alternative<NumberSpec>(rhs_spec->spec)) {
    const auto& a = std::get<NumberSpec>(lhs_spec->spec);
    const auto& b = std::get<NumberSpec>(rhs_spec->spec);
    NumberSpec merged;

    auto apply_lower = [](std::optional<double> min_value,
                          std::optional<double> exclusive_value,
                          std::optional<double>* out_minimum,
                          std::optional<double>* out_exclusive_minimum) {
      if (min_value.has_value()) {
        if (!out_minimum->has_value() && !out_exclusive_minimum->has_value()) {
          *out_minimum = min_value;
        } else if (out_exclusive_minimum->has_value()) {
          if (min_value.value() > out_exclusive_minimum->value()) {
            *out_minimum = min_value;
            *out_exclusive_minimum = std::nullopt;
          }
        } else if (min_value.value() > out_minimum->value()) {
          *out_minimum = min_value;
        }
      }
      if (exclusive_value.has_value()) {
        if (!out_minimum->has_value() && !out_exclusive_minimum->has_value()) {
          *out_exclusive_minimum = exclusive_value;
        } else if (out_exclusive_minimum->has_value()) {
          if (exclusive_value.value() >= out_exclusive_minimum->value()) {
            *out_exclusive_minimum = exclusive_value;
            *out_minimum = std::nullopt;
          }
        } else if (exclusive_value.value() > out_minimum->value()) {
          *out_exclusive_minimum = exclusive_value;
          *out_minimum = std::nullopt;
        } else if (exclusive_value.value() == out_minimum->value()) {
          *out_exclusive_minimum = exclusive_value;
          *out_minimum = std::nullopt;
        }
      }
    };
    auto apply_upper = [](std::optional<double> max_value,
                          std::optional<double> exclusive_value,
                          std::optional<double>* out_maximum,
                          std::optional<double>* out_exclusive_maximum) {
      if (max_value.has_value()) {
        if (!out_maximum->has_value() && !out_exclusive_maximum->has_value()) {
          *out_maximum = max_value;
        } else if (out_exclusive_maximum->has_value()) {
          if (max_value.value() < out_exclusive_maximum->value()) {
            *out_maximum = max_value;
            *out_exclusive_maximum = std::nullopt;
          }
        } else if (max_value.value() < out_maximum->value()) {
          *out_maximum = max_value;
        }
      }
      if (exclusive_value.has_value()) {
        if (!out_maximum->has_value() && !out_exclusive_maximum->has_value()) {
          *out_exclusive_maximum = exclusive_value;
        } else if (out_exclusive_maximum->has_value()) {
          if (exclusive_value.value() <= out_exclusive_maximum->value()) {
            *out_exclusive_maximum = exclusive_value;
            *out_maximum = std::nullopt;
          }
        } else if (exclusive_value.value() < out_maximum->value()) {
          *out_exclusive_maximum = exclusive_value;
          *out_maximum = std::nullopt;
        } else if (exclusive_value.value() == out_maximum->value()) {
          *out_exclusive_maximum = exclusive_value;
          *out_maximum = std::nullopt;
        }
      }
    };

    apply_lower(a.minimum, a.exclusive_minimum, &merged.minimum, &merged.exclusive_minimum);
    apply_lower(b.minimum, b.exclusive_minimum, &merged.minimum, &merged.exclusive_minimum);
    apply_upper(a.maximum, a.exclusive_maximum, &merged.maximum, &merged.exclusive_maximum);
    apply_upper(b.maximum, b.exclusive_maximum, &merged.maximum, &merged.exclusive_maximum);

    double effective_min = merged.exclusive_minimum.value_or(
        merged.minimum.value_or(-std::numeric_limits<double>::infinity())
    );
    double effective_max = merged.exclusive_maximum.value_or(
        merged.maximum.value_or(std::numeric_limits<double>::infinity())
    );
    if (effective_min > effective_max ||
        (effective_min == effective_max &&
         (merged.exclusive_minimum.has_value() || merged.exclusive_maximum.has_value()))) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema, "allOf number constraints conflict"
      );
    }
    return ResultOk(SchemaSpec::Make(std::move(merged), "", rule_name_hint));
  }

  if (std::holds_alternative<StringSpec>(lhs_spec->spec) &&
      std::holds_alternative<StringSpec>(rhs_spec->spec)) {
    const auto& a = std::get<StringSpec>(lhs_spec->spec);
    const auto& b = std::get<StringSpec>(rhs_spec->spec);
    StringSpec merged = a;
    merged.min_length = std::max(a.min_length, b.min_length);
    if (a.max_length == -1) {
      merged.max_length = b.max_length;
    } else if (b.max_length == -1) {
      merged.max_length = a.max_length;
    } else {
      merged.max_length = std::min(a.max_length, b.max_length);
    }
    if (merged.max_length != -1 && merged.min_length > merged.max_length) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema, "allOf string length constraints conflict"
      );
    }
    if (a.pattern && b.pattern && a.pattern.value() != b.pattern.value()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "allOf merging different string patterns is not supported"
      );
    }
    if (a.format && b.format && a.format.value() != b.format.value()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "allOf merging different string formats is not supported"
      );
    }
    if (!merged.pattern) merged.pattern = b.pattern;
    if (!merged.format) merged.format = b.format;
    return ResultOk(SchemaSpec::Make(std::move(merged), "", rule_name_hint));
  }

  if (std::holds_alternative<BooleanSpec>(lhs_spec->spec) &&
      std::holds_alternative<BooleanSpec>(rhs_spec->spec)) {
    return ResultOk(lhs_spec);
  }
  if (std::holds_alternative<NullSpec>(lhs_spec->spec) &&
      std::holds_alternative<NullSpec>(rhs_spec->spec)) {
    return ResultOk(lhs_spec);
  }

  if (std::holds_alternative<ObjectSpec>(lhs_spec->spec) &&
      std::holds_alternative<ObjectSpec>(rhs_spec->spec)) {
    auto merge_result = MergeObjectSpecs(
        std::get<ObjectSpec>(lhs_spec->spec), std::get<ObjectSpec>(rhs_spec->spec), rule_name_hint
    );
    if (merge_result.IsErr()) return ResultErr(std::move(merge_result).UnwrapErr());
    return ResultOk(SchemaSpec::Make(std::move(merge_result).Unwrap(), "", rule_name_hint));
  }

  if (std::holds_alternative<ArraySpec>(lhs_spec->spec) &&
      std::holds_alternative<ArraySpec>(rhs_spec->spec)) {
    auto merge_result = MergeArraySpecs(
        std::get<ArraySpec>(lhs_spec->spec), std::get<ArraySpec>(rhs_spec->spec), rule_name_hint
    );
    if (merge_result.IsErr()) return ResultErr(std::move(merge_result).UnwrapErr());
    return ResultOk(SchemaSpec::Make(std::move(merge_result).Unwrap(), "", rule_name_hint));
  }

  if (std::holds_alternative<ConstSpec>(lhs_spec->spec) &&
      std::holds_alternative<ConstSpec>(rhs_spec->spec)) {
    if (std::get<ConstSpec>(lhs_spec->spec).json_value ==
        std::get<ConstSpec>(rhs_spec->spec).json_value) {
      return ResultOk(lhs_spec);
    }
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema, "allOf const constraints conflict"
    );
  }

  if (std::holds_alternative<EnumSpec>(lhs_spec->spec) &&
      std::holds_alternative<EnumSpec>(rhs_spec->spec)) {
    EnumSpec merged;
    for (const auto& value : std::get<EnumSpec>(lhs_spec->spec).json_values) {
      if (std::find(
              std::get<EnumSpec>(rhs_spec->spec).json_values.begin(),
              std::get<EnumSpec>(rhs_spec->spec).json_values.end(),
              value
          ) != std::get<EnumSpec>(rhs_spec->spec).json_values.end()) {
        merged.json_values.push_back(value);
      }
    }
    if (merged.json_values.empty()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema, "allOf enum constraints conflict"
      );
    }
    if (merged.json_values.size() == 1) {
      return ResultOk(SchemaSpec::Make(ConstSpec{merged.json_values[0]}, "", rule_name_hint));
    }
    return ResultOk(SchemaSpec::Make(std::move(merged), "", rule_name_hint));
  }

  if (std::holds_alternative<ConstSpec>(lhs_spec->spec) &&
      std::holds_alternative<EnumSpec>(rhs_spec->spec)) {
    const auto& const_value = std::get<ConstSpec>(lhs_spec->spec).json_value;
    const auto& enum_values = std::get<EnumSpec>(rhs_spec->spec).json_values;
    if (std::find(enum_values.begin(), enum_values.end(), const_value) != enum_values.end()) {
      return ResultOk(lhs_spec);
    }
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema, "allOf const/enum constraints conflict"
    );
  }

  if (std::holds_alternative<EnumSpec>(lhs_spec->spec) &&
      std::holds_alternative<ConstSpec>(rhs_spec->spec)) {
    return MergeSchemaSpecs(rhs_spec, lhs_spec, rule_name_hint);
  }

  auto get_atomic_type_id = [](const SchemaSpecPtr& spec) -> int {
    if (std::holds_alternative<IntegerSpec>(spec->spec)) return 0;
    if (std::holds_alternative<NumberSpec>(spec->spec)) return 1;
    if (std::holds_alternative<StringSpec>(spec->spec)) return 2;
    if (std::holds_alternative<BooleanSpec>(spec->spec)) return 3;
    if (std::holds_alternative<NullSpec>(spec->spec)) return 4;
    if (std::holds_alternative<ArraySpec>(spec->spec)) return 5;
    if (std::holds_alternative<ObjectSpec>(spec->spec)) return 6;
    return -1;
  };
  int lhs_atomic_type_id = get_atomic_type_id(lhs_spec);
  int rhs_atomic_type_id = get_atomic_type_id(rhs_spec);
  if (lhs_atomic_type_id != -1 && rhs_atomic_type_id != -1 &&
      lhs_atomic_type_id != rhs_atomic_type_id) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema, "allOf types have empty intersection"
    );
  }

  if (lhs_spec->spec.index() == rhs_spec->spec.index()) {
    return ResultOk(lhs_spec);
  }

  return ResultErr<SchemaError>(
      SchemaErrorType::kInvalidSchema, "allOf merge for the provided schema combination is not supported"
  );
}

Result<SchemaSpecPtr, SchemaError> SchemaParser::Parse(
    const picojson::value& schema,
    const std::string& rule_name_hint,
    std::optional<std::string> default_type
) {
  std::string cache_key = ComputeCacheKey(schema);
  if (schema_cache_.count(cache_key)) {
    return ResultOk(schema_cache_[cache_key]);
  }

  if (schema.is<bool>()) {
    if (!schema.get<bool>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema, "Schema 'false' cannot accept any value"
      );
    }
    auto spec = SchemaSpec::Make(AnySpec{}, cache_key, rule_name_hint);
    schema_cache_[cache_key] = spec;
    return ResultOk(spec);
  }

  if (!schema.is<picojson::object>()) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kInvalidSchema,
        "Schema should be an object or bool, but got " + schema.serialize(false)
    );
  }

  const auto& schema_obj = schema.get<picojson::object>();
  WarnUnsupportedKeywords(schema_obj, {"dependentSchemas"});

  SchemaSpecPtr result;

  if (schema_obj.count("if") || schema_obj.count("then") || schema_obj.count("else")) {
    auto conditional_result = ParseConditional(schema_obj, rule_name_hint);
    if (conditional_result.IsErr()) return ResultErr(std::move(conditional_result).UnwrapErr());
    result = std::move(conditional_result).Unwrap();
    result->cache_key = cache_key;
    result->rule_name_hint = rule_name_hint;
  } else if (schema_obj.count("$ref")) {
    auto ref_result = ParseRef(schema_obj);
    if (ref_result.IsErr()) return ResultErr(std::move(ref_result).UnwrapErr());
    auto ref_spec = std::move(ref_result).Unwrap();
    result = SchemaSpec::Make(std::move(ref_spec), cache_key, rule_name_hint);
  } else if (schema_obj.count("const")) {
    auto const_result = ParseConst(schema_obj);
    if (const_result.IsErr()) return ResultErr(std::move(const_result).UnwrapErr());
    result = SchemaSpec::Make(std::move(const_result).Unwrap(), cache_key, rule_name_hint);
  } else if (schema_obj.count("enum")) {
    auto enum_result = ParseEnum(schema_obj);
    if (enum_result.IsErr()) return ResultErr(std::move(enum_result).UnwrapErr());
    result = SchemaSpec::Make(std::move(enum_result).Unwrap(), cache_key, rule_name_hint);
  } else if (schema_obj.count("anyOf")) {
    auto anyof_result = ParseAnyOf(schema_obj);
    if (anyof_result.IsErr()) return ResultErr(std::move(anyof_result).UnwrapErr());
    result = SchemaSpec::Make(std::move(anyof_result).Unwrap(), cache_key, rule_name_hint);
  } else if (schema_obj.count("oneOf")) {
    auto oneof_result = ParseOneOf(schema_obj, rule_name_hint);
    if (oneof_result.IsErr()) return ResultErr(std::move(oneof_result).UnwrapErr());
    result = std::move(oneof_result).Unwrap();
    result->cache_key = cache_key;
    result->rule_name_hint = rule_name_hint;
  } else if (schema_obj.count("allOf")) {
    auto allof_result = ParseAllOf(schema_obj);
    if (allof_result.IsErr()) return ResultErr(std::move(allof_result).UnwrapErr());
    result = SchemaSpec::Make(std::move(allof_result).Unwrap(), cache_key, rule_name_hint);
  } else if (schema_obj.count("type") || default_type.has_value()) {
    if (schema_obj.count("type") && schema_obj.at("type").is<picojson::array>()) {
      auto type_array_result = ParseTypeArray(schema_obj, rule_name_hint);
      if (type_array_result.IsErr()) return ResultErr(std::move(type_array_result).UnwrapErr());
      result = SchemaSpec::Make(std::move(type_array_result).Unwrap(), cache_key, rule_name_hint);
    } else {
      if (schema_obj.count("type") && !schema_obj.at("type").is<std::string>()) {
        return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "Type should be a string");
      }
      const std::string& type = schema_obj.count("type") ? schema_obj.at("type").get<std::string>()
                                                         : default_type.value();
      if (type == "integer") {
        auto int_result = ParseInteger(schema_obj);
        if (int_result.IsErr()) return ResultErr(std::move(int_result).UnwrapErr());
        result = SchemaSpec::Make(std::move(int_result).Unwrap(), cache_key, rule_name_hint);
      } else if (type == "number") {
        auto num_result = ParseNumber(schema_obj);
        if (num_result.IsErr()) return ResultErr(std::move(num_result).UnwrapErr());
        result = SchemaSpec::Make(std::move(num_result).Unwrap(), cache_key, rule_name_hint);
      } else if (type == "string") {
        auto str_result = ParseString(schema_obj);
        if (str_result.IsErr()) return ResultErr(std::move(str_result).UnwrapErr());
        result = SchemaSpec::Make(std::move(str_result).Unwrap(), cache_key, rule_name_hint);
      } else if (type == "boolean") {
        auto bool_result = ParseBoolean(schema_obj);
        if (bool_result.IsErr()) return ResultErr(std::move(bool_result).UnwrapErr());
        result = SchemaSpec::Make(std::move(bool_result).Unwrap(), cache_key, rule_name_hint);
      } else if (type == "null") {
        auto null_result = ParseNull(schema_obj);
        if (null_result.IsErr()) return ResultErr(std::move(null_result).UnwrapErr());
        result = SchemaSpec::Make(std::move(null_result).Unwrap(), cache_key, rule_name_hint);
      } else if (type == "array") {
        auto array_result = ParseArray(schema_obj);
        if (array_result.IsErr()) return ResultErr(std::move(array_result).UnwrapErr());
        result = SchemaSpec::Make(std::move(array_result).Unwrap(), cache_key, rule_name_hint);
      } else if (type == "object") {
        auto obj_result = ParseObject(schema_obj);
        if (obj_result.IsErr()) return ResultErr(std::move(obj_result).UnwrapErr());
        result = SchemaSpec::Make(std::move(obj_result).Unwrap(), cache_key, rule_name_hint);
      } else {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema, "Unsupported type \"" + type + "\""
        );
      }
    }
  } else if (schema_obj.count("properties") || schema_obj.count("required") ||
             schema_obj.count("dependentRequired") || schema_obj.count("not") ||
             schema_obj.count("patternProperties") || schema_obj.count("propertyNames") ||
             schema_obj.count("additionalProperties") || schema_obj.count("unevaluatedProperties") ||
             schema_obj.count("minProperties") || schema_obj.count("maxProperties")) {
    auto obj_result = ParseObject(schema_obj);
    if (obj_result.IsErr()) return ResultErr(std::move(obj_result).UnwrapErr());
    result = SchemaSpec::Make(std::move(obj_result).Unwrap(), cache_key, rule_name_hint);
  } else if (schema_obj.count("items") || schema_obj.count("prefixItems") ||
             schema_obj.count("unevaluatedItems")) {
    auto array_result = ParseArray(schema_obj);
    if (array_result.IsErr()) return ResultErr(std::move(array_result).UnwrapErr());
    result = SchemaSpec::Make(std::move(array_result).Unwrap(), cache_key, rule_name_hint);
  } else {
    result = SchemaSpec::Make(AnySpec{}, cache_key, rule_name_hint);
  }

  schema_cache_[cache_key] = result;
  return ResultOk(result);
}

Result<IntegerSpec, SchemaError> SchemaParser::ParseInteger(const picojson::object& schema) {
  WarnUnsupportedKeywords(schema, {"multipleOf", "not"});
  IntegerSpec spec;

  auto checkAndConvertIntegerBound = [](const picojson::value& value
                                     ) -> Result<int64_t, SchemaError> {
    if (!value.is<int64_t>() && !value.is<double>()) {
      return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "Value must be a number");
    }
    if (value.is<int64_t>()) return ResultOk<int64_t>(value.get<int64_t>());
    double val = value.get<double>();
    if (val != std::floor(val)) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "Integer constraint must be a whole number"
      );
    }
    static const double PROBLEMATIC_MIN = -9223372036854776000.0;
    static const double PROBLEMATIC_MAX = 9223372036854776000.0;
    if (val == PROBLEMATIC_MIN) {
      XGRAMMAR_CHECK(false
      ) << "Integer exceeds minimum limit due to precision loss at 64-bit boundary";
    }

    if (val == PROBLEMATIC_MAX) {
      XGRAMMAR_CHECK(false
      ) << "Integer exceeds maximum limit due to precision loss at 64-bit boundary";
    }
    static const double MAX_INT64_AS_DOUBLE =
        static_cast<double>(std::numeric_limits<int64_t>::max());
    static const double MIN_INT64_AS_DOUBLE =
        static_cast<double>(std::numeric_limits<int64_t>::min());
    XGRAMMAR_CHECK(val <= MAX_INT64_AS_DOUBLE) << "Integer exceeds maximum limit";
    XGRAMMAR_CHECK(val >= MIN_INT64_AS_DOUBLE) << "Integer exceeds minimum limit";
    return ResultOk<int64_t>(static_cast<int64_t>(val));
  };

  if (schema.count("minimum")) {
    auto result = checkAndConvertIntegerBound(schema.at("minimum"));
    if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
    spec.minimum = std::move(result).Unwrap();
  }
  if (schema.count("maximum")) {
    auto result = checkAndConvertIntegerBound(schema.at("maximum"));
    if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
    spec.maximum = std::move(result).Unwrap();
  }
  if (schema.count("exclusiveMinimum")) {
    auto result = checkAndConvertIntegerBound(schema.at("exclusiveMinimum"));
    if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
    int64_t val = std::move(result).Unwrap();
    if (val == std::numeric_limits<int64_t>::max()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema, "exclusiveMinimum would cause integer overflow"
      );
    }
    spec.exclusive_minimum = val;
  }
  if (schema.count("exclusiveMaximum")) {
    auto result = checkAndConvertIntegerBound(schema.at("exclusiveMaximum"));
    if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
    int64_t val = std::move(result).Unwrap();
    if (val == std::numeric_limits<int64_t>::min()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema, "exclusiveMaximum would cause integer underflow"
      );
    }
    spec.exclusive_maximum = val;
  }

  int64_t effective_min = spec.minimum.value_or(std::numeric_limits<int64_t>::min());
  int64_t effective_max = spec.maximum.value_or(std::numeric_limits<int64_t>::max());
  if (spec.exclusive_minimum.has_value()) {
    effective_min = std::max(effective_min, *spec.exclusive_minimum + 1);
  }
  if (spec.exclusive_maximum.has_value()) {
    effective_max = std::min(effective_max, *spec.exclusive_maximum - 1);
  }
  if (effective_min > effective_max) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema, "Invalid range: minimum greater than maximum"
    );
  }
  return ResultOk(std::move(spec));
}

Result<NumberSpec, SchemaError> SchemaParser::ParseNumber(const picojson::object& schema) {
  WarnUnsupportedKeywords(schema, {"multipleOf", "not"});
  NumberSpec spec;

  auto getDouble = [](const picojson::value& value) -> Result<double, SchemaError> {
    if (!value.is<double>() && !value.is<int64_t>()) {
      return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "Value must be a number");
    }
    return ResultOk<double>(value.get<double>());
  };

  if (schema.count("minimum")) {
    auto result = getDouble(schema.at("minimum"));
    if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
    spec.minimum = std::move(result).Unwrap();
  }
  if (schema.count("maximum")) {
    auto result = getDouble(schema.at("maximum"));
    if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
    spec.maximum = std::move(result).Unwrap();
  }
  if (schema.count("exclusiveMinimum")) {
    auto result = getDouble(schema.at("exclusiveMinimum"));
    if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
    spec.exclusive_minimum = std::move(result).Unwrap();
  }
  if (schema.count("exclusiveMaximum")) {
    auto result = getDouble(schema.at("exclusiveMaximum"));
    if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
    spec.exclusive_maximum = std::move(result).Unwrap();
  }

  double effective_min = spec.minimum.value_or(-std::numeric_limits<double>::infinity());
  double effective_max = spec.maximum.value_or(std::numeric_limits<double>::infinity());
  if (spec.exclusive_minimum.has_value()) {
    effective_min = std::max(effective_min, *spec.exclusive_minimum);
  }
  if (spec.exclusive_maximum.has_value()) {
    effective_max = std::min(effective_max, *spec.exclusive_maximum);
  }
  if (effective_min > effective_max) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema, "Invalid range: minimum greater than maximum"
    );
  }
  return ResultOk(std::move(spec));
}

Result<StringSpec, SchemaError> SchemaParser::ParseString(const picojson::object& schema) {
  WarnUnsupportedKeywords(schema, {"not"});
  StringSpec spec;
  if (schema.count("format")) spec.format = schema.at("format").get<std::string>();
  if (schema.count("pattern")) spec.pattern = schema.at("pattern").get<std::string>();
  if (schema.count("minLength")) {
    if (!schema.at("minLength").is<int64_t>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "minLength must be an integer"
      );
    }
    spec.min_length = static_cast<int>(schema.at("minLength").get<int64_t>());
  }
  if (schema.count("maxLength")) {
    if (!schema.at("maxLength").is<int64_t>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "maxLength must be an integer"
      );
    }
    spec.max_length = static_cast<int>(schema.at("maxLength").get<int64_t>());
  }
  if (spec.max_length != -1 && spec.min_length > spec.max_length) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema,
        "minLength " + std::to_string(spec.min_length) + " is greater than maxLength " +
            std::to_string(spec.max_length)
    );
  }
  return ResultOk(std::move(spec));
}

Result<BooleanSpec, SchemaError> SchemaParser::ParseBoolean(const picojson::object& schema) {
  WarnUnsupportedKeywords(schema, {"not"});
  return ResultOk(BooleanSpec{});
}

Result<NullSpec, SchemaError> SchemaParser::ParseNull(const picojson::object& schema) {
  WarnUnsupportedKeywords(schema, {"not"});
  return ResultOk(NullSpec{});
}

Result<ArraySpec, SchemaError> SchemaParser::ParseArray(const picojson::object& schema) {
  WarnUnsupportedKeywords(schema, {"uniqueItems", "maxContains", "not"});
  ArraySpec spec;

  if (schema.count("prefixItems")) {
    if (!schema.at("prefixItems").is<picojson::array>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "prefixItems must be an array"
      );
    }
    for (const auto& item : schema.at("prefixItems").get<picojson::array>()) {
      if (item.is<bool>() && !item.get<bool>()) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kUnsatisfiableSchema, "prefixItems contains false"
        );
      } else if (!item.is<picojson::object>()) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema, "prefixItems must be an array of objects or booleans"
        );
      }
      auto item_result = Parse(item, "prefix_item");
      if (item_result.IsErr()) return ResultErr(std::move(item_result).UnwrapErr());
      spec.prefix_items.push_back(std::move(item_result).Unwrap());
    }
  }

  if (schema.count("items")) {
    auto items_value = schema.at("items");
    if (!items_value.is<bool>() && !items_value.is<picojson::object>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "items must be a boolean or an object"
      );
    }
    if (items_value.is<bool>() && !items_value.get<bool>()) {
      spec.allow_additional_items = false;
    } else {
      spec.allow_additional_items = true;
      auto items_result = Parse(items_value, "item");
      if (items_result.IsErr()) return ResultErr(std::move(items_result).UnwrapErr());
      spec.additional_items = std::move(items_result).Unwrap();
    }
  } else if (schema.count("unevaluatedItems")) {
    auto unevaluated_items_value = schema.at("unevaluatedItems");
    if (!unevaluated_items_value.is<bool>() && !unevaluated_items_value.is<picojson::object>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "unevaluatedItems must be a boolean or an object"
      );
    }
    if (unevaluated_items_value.is<bool>() && !unevaluated_items_value.get<bool>()) {
      spec.allow_additional_items = false;
    } else {
      spec.allow_additional_items = true;
      auto items_result = Parse(unevaluated_items_value, "unevaluated_item");
      if (items_result.IsErr()) return ResultErr(std::move(items_result).UnwrapErr());
      spec.additional_items = std::move(items_result).Unwrap();
    }
  } else if (schema.count("contains")) {
    spec.allow_additional_items = true;
    spec.additional_items = SchemaSpec::Make(AnySpec{}, "", "any");
  } else if (!config_.strict_mode) {
    spec.allow_additional_items = true;
    spec.additional_items = SchemaSpec::Make(AnySpec{}, "", "any");
  } else {
    spec.allow_additional_items = false;
  }

  if (schema.count("minItems")) {
    if (!schema.at("minItems").is<int64_t>()) {
      return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "minItems must be an integer");
    }
    spec.min_items = std::max(static_cast<int64_t>(0), schema.at("minItems").get<int64_t>());
  }
  if (schema.count("maxItems")) {
    if (!schema.at("maxItems").is<int64_t>() || schema.at("maxItems").get<int64_t>() < 0) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "maxItems must be a non-negative integer"
      );
    }
    spec.max_items = schema.at("maxItems").get<int64_t>();
  }

  if (spec.max_items != -1 && spec.min_items > spec.max_items) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema,
        "minItems is greater than maxItems: " + std::to_string(spec.min_items) + " > " +
            std::to_string(spec.max_items)
    );
  }
  if (spec.max_items != -1 && spec.max_items < static_cast<int64_t>(spec.prefix_items.size())) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema,
        "maxItems is less than the number of prefixItems: " + std::to_string(spec.max_items) +
            " < " + std::to_string(spec.prefix_items.size())
    );
  }
  if (!spec.allow_additional_items) {
    int64_t prefix_size = static_cast<int64_t>(spec.prefix_items.size());
    if (prefix_size < spec.min_items) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema,
          "minItems is greater than the number of prefixItems, but additional items are not "
          "allowed: " +
              std::to_string(spec.min_items) + " > " + std::to_string(prefix_size)
      );
    }
    if (spec.max_items != -1 && prefix_size > spec.max_items) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema,
          "maxItems is less than the number of prefixItems, but additional items are not "
          "allowed: " +
              std::to_string(spec.max_items) + " < " + std::to_string(prefix_size)
      );
    }
  }

  if (schema.count("contains")) {
    if (!spec.prefix_items.empty()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema,
          "contains is only supported for homogeneous arrays without prefixItems"
      );
    }
    if (!spec.allow_additional_items) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema,
          "contains requires additional items to be allowed"
      );
    }
    if (schema.count("maxContains")) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "maxContains is not supported"
      );
    }
    int64_t min_contains = 1;
    if (schema.count("minContains")) {
      if (!schema.at("minContains").is<int64_t>()) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema, "minContains must be an integer"
        );
      }
      min_contains = schema.at("minContains").get<int64_t>();
    }
    if (min_contains != 1) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema,
          "contains only supports minContains == 1 in this implementation"
      );
    }

    SchemaSpecPtr contains_spec;
    const auto& contains_value = schema.at("contains");
    if (contains_value.is<bool>()) {
      if (!contains_value.get<bool>()) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kUnsatisfiableSchema,
            "contains: false with minContains == 1 is unsatisfiable"
        );
      }
      contains_spec = spec.additional_items ? spec.additional_items : SchemaSpec::Make(AnySpec{}, "", "any");
    } else if (contains_value.is<picojson::object>()) {
      auto contains_result = Parse(contains_value, "contains_item");
      if (contains_result.IsErr()) return ResultErr(std::move(contains_result).UnwrapErr());
      contains_spec = std::move(contains_result).Unwrap();
    } else {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "contains must be a boolean or an object"
      );
    }

    if (spec.additional_items) {
      auto merge_result = MergeSchemaSpecs(spec.additional_items, contains_spec, "contains_item");
      if (merge_result.IsErr()) return ResultErr(std::move(merge_result).UnwrapErr());
      spec.contains_item = std::move(merge_result).Unwrap();
    } else {
      spec.contains_item = std::move(contains_spec);
    }
    spec.min_contains = 1;
  } else if (schema.count("minContains")) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kInvalidSchema, "minContains requires contains"
    );
  } else if (schema.count("maxContains")) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kInvalidSchema, "maxContains requires contains"
    );
  }

  if (spec.min_contains > 0 && spec.max_items != -1 &&
      std::max(spec.min_items, spec.min_contains) > spec.max_items) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema, "contains conflicts with array item count constraints"
    );
  }
  return ResultOk(std::move(spec));
}

Result<ObjectSpec, SchemaError> SchemaParser::ParseObject(const picojson::object& schema) {
  ObjectSpec spec;

  if (schema.count("properties")) {
    if (!schema.at("properties").is<picojson::object>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "properties must be an object"
      );
    }
    auto properties_obj = schema.at("properties").get<picojson::object>();
    for (const auto& key : properties_obj.ordered_keys()) {
      auto prop_result = Parse(properties_obj.at(key), key);
      if (prop_result.IsErr()) return ResultErr(std::move(prop_result).UnwrapErr());
      spec.properties.push_back({key, std::move(prop_result).Unwrap()});
    }
  }

  if (schema.count("required")) {
    if (!schema.at("required").is<picojson::array>()) {
      return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "required must be an array");
    }
    for (const auto& req : schema.at("required").get<picojson::array>()) {
      spec.required.insert(req.get<std::string>());
    }
  }

  if (schema.count("dependentRequired")) {
    if (!schema.at("dependentRequired").is<picojson::object>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "dependentRequired must be an object"
      );
    }
    const auto& dependent_required_obj = schema.at("dependentRequired").get<picojson::object>();
    for (const auto& key : dependent_required_obj.ordered_keys()) {
      const auto& deps_value = dependent_required_obj.at(key);
      if (!deps_value.is<picojson::array>()) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema, "dependentRequired values must be arrays"
        );
      }
      std::vector<std::string> deps;
      for (const auto& dep : deps_value.get<picojson::array>()) {
        if (!dep.is<std::string>()) {
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema,
              "dependentRequired arrays must contain only strings"
          );
        }
        const auto& dep_name = dep.get<std::string>();
        if (std::find(deps.begin(), deps.end(), dep_name) == deps.end()) {
          deps.push_back(dep_name);
        }
      }
      if (!deps.empty()) {
        spec.dependent_required[key] = std::move(deps);
      }
    }
  }

  if (schema.count("not")) {
    auto parse_required_only_object =
        [](const picojson::value& value, const std::string& context
        ) -> Result<std::vector<std::string>, SchemaError> {
      if (!value.is<picojson::object>()) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema, context + " must be an object"
        );
      }
      const auto& obj = value.get<picojson::object>();
      static const std::unordered_set<std::string> kIgnoredKeys = {
          "title",
          "default",
          "description",
          "examples",
          "deprecated",
          "readOnly",
          "writeOnly",
          "$comment",
      };
      bool has_required = obj.count("required");
      if (!has_required) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema, context + " must contain required"
        );
      }
      for (const auto& key : obj.ordered_keys()) {
        if (key == "required" || key == "type" || kIgnoredKeys.count(key) != 0) {
          continue;
        }
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema,
            context + " only supports required with optional type=object"
        );
      }
      if (obj.count("type")) {
        if (!obj.at("type").is<std::string>() || obj.at("type").get<std::string>() != "object") {
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema, context + " type must be object"
          );
        }
      }
      if (!obj.at("required").is<picojson::array>()) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema, context + " required must be an array"
        );
      }
      std::vector<std::string> group;
      for (const auto& item : obj.at("required").get<picojson::array>()) {
        if (!item.is<std::string>()) {
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema,
              context + " required must contain only strings"
          );
        }
        const auto& name = item.get<std::string>();
        if (std::find(group.begin(), group.end(), name) == group.end()) {
          group.push_back(name);
        }
      }
      return ResultOk(std::move(group));
    };

    const auto& not_value = schema.at("not");
    if (!not_value.is<picojson::object>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "object not must be an object"
      );
    }
    const auto& not_obj = not_value.get<picojson::object>();
    static const std::unordered_set<std::string> kIgnoredNotKeys = {
        "title",
        "default",
        "description",
        "examples",
        "deprecated",
        "readOnly",
        "writeOnly",
        "$comment",
    };
    auto validate_not_container_keys =
        [&](bool allow_required, bool allow_anyof) -> Result<bool, SchemaError> {
      for (const auto& key : not_obj.ordered_keys()) {
        if ((allow_required && key == "required") || (allow_anyof && key == "anyOf") ||
            key == "type" || kIgnoredNotKeys.count(key) != 0) {
          continue;
        }
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema,
            "object not only supports required or anyOf of required groups"
        );
      }
      if (not_obj.count("type")) {
        if (!not_obj.at("type").is<std::string>() || not_obj.at("type").get<std::string>() != "object") {
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema, "object not type must be object"
          );
        }
      }
      return ResultOk(true);
    };

    if (not_obj.count("required")) {
      auto validate_result = validate_not_container_keys(/*allow_required=*/true, /*allow_anyof=*/false);
      if (validate_result.IsErr()) return ResultErr(std::move(validate_result).UnwrapErr());
      auto group_result = parse_required_only_object(not_value, "not");
      if (group_result.IsErr()) return ResultErr(std::move(group_result).UnwrapErr());
      spec.forbidden_groups.push_back(std::move(group_result).Unwrap());
    } else if (not_obj.count("anyOf")) {
      auto validate_result = validate_not_container_keys(/*allow_required=*/false, /*allow_anyof=*/true);
      if (validate_result.IsErr()) return ResultErr(std::move(validate_result).UnwrapErr());
      if (!not_obj.at("anyOf").is<picojson::array>()) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema, "not anyOf must be an array"
        );
      }
      for (const auto& option : not_obj.at("anyOf").get<picojson::array>()) {
        auto group_result = parse_required_only_object(option, "not anyOf entry");
        if (group_result.IsErr()) return ResultErr(std::move(group_result).UnwrapErr());
        spec.forbidden_groups.push_back(std::move(group_result).Unwrap());
      }
    } else {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema,
          "object not only supports required or anyOf of required groups"
      );
    }
  }

  if (schema.count("patternProperties")) {
    if (!schema.at("patternProperties").is<picojson::object>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "patternProperties must be an object"
      );
    }
    auto pattern_props = schema.at("patternProperties").get<picojson::object>();
    for (const auto& key : pattern_props.ordered_keys()) {
      auto prop_result = Parse(pattern_props.at(key), "pattern_prop");
      if (prop_result.IsErr()) return ResultErr(std::move(prop_result).UnwrapErr());
      spec.pattern_properties.push_back({key, std::move(prop_result).Unwrap()});
    }
  }

  if (schema.count("propertyNames")) {
    if (!schema.at("propertyNames").is<picojson::object>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "propertyNames must be an object"
      );
    }
    auto property_names_obj = schema.at("propertyNames").get<picojson::object>();
    if (property_names_obj.count("type") && property_names_obj.at("type").is<std::string>() &&
        property_names_obj.at("type").get<std::string>() != "string") {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema,
          "propertyNames must be an object that validates string"
      );
    }
    auto prop_names_result = Parse(schema.at("propertyNames"), "property_name", "string");
    if (prop_names_result.IsErr()) return ResultErr(std::move(prop_names_result).UnwrapErr());
    spec.property_names = std::move(prop_names_result).Unwrap();
  }

  spec.allow_additional_properties = !config_.strict_mode;
  if (schema.count("additionalProperties")) {
    auto add_props = schema.at("additionalProperties");
    if (add_props.is<bool>()) {
      spec.allow_additional_properties = add_props.get<bool>();
    } else {
      spec.allow_additional_properties = true;
      auto add_props_result = Parse(add_props, "additional");
      if (add_props_result.IsErr()) return ResultErr(std::move(add_props_result).UnwrapErr());
      spec.additional_properties_schema = std::move(add_props_result).Unwrap();
    }
  }

  spec.allow_unevaluated_properties = true;
  if (schema.count("additionalProperties")) {
    spec.allow_unevaluated_properties = spec.allow_additional_properties;
  } else if (schema.count("unevaluatedProperties")) {
    auto uneval_props = schema.at("unevaluatedProperties");
    if (uneval_props.is<bool>()) {
      spec.allow_unevaluated_properties = uneval_props.get<bool>();
    } else {
      spec.allow_unevaluated_properties = true;
      auto uneval_result = Parse(uneval_props, "unevaluated");
      if (uneval_result.IsErr()) return ResultErr(std::move(uneval_result).UnwrapErr());
      spec.unevaluated_properties_schema = std::move(uneval_result).Unwrap();
    }
  } else if (config_.strict_mode) {
    spec.allow_unevaluated_properties = false;
  }

  if (schema.count("minProperties")) {
    if (!schema.at("minProperties").is<int64_t>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "minProperties must be an integer"
      );
    }
    spec.min_properties = static_cast<int>(schema.at("minProperties").get<int64_t>());
    if (spec.min_properties < 0) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema, "minProperties must be a non-negative integer"
      );
    }
  }
  if (schema.count("maxProperties")) {
    if (!schema.at("maxProperties").is<int64_t>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "maxProperties must be an integer"
      );
    }
    spec.max_properties = static_cast<int>(schema.at("maxProperties").get<int64_t>());
    if (spec.max_properties < 0) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema, "maxProperties must be a non-negative integer"
      );
    }
  }

  if (spec.max_properties != -1 && spec.min_properties > spec.max_properties) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema,
        "minProperties is greater than maxProperties: " + std::to_string(spec.min_properties) +
            " > " + std::to_string(spec.max_properties)
    );
  }
  if (spec.max_properties != -1 && static_cast<int>(spec.required.size()) > spec.max_properties) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema,
        "maxProperties is less than the number of required properties: " +
            std::to_string(spec.max_properties) + " < " + std::to_string(spec.required.size())
    );
  }
  if (spec.pattern_properties.empty() && !spec.property_names &&
      !spec.allow_additional_properties && !spec.allow_unevaluated_properties &&
      spec.min_properties > static_cast<int>(spec.properties.size())) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema,
        "minProperties is greater than the number of properties, but additional properties aren't "
        "allowed: " +
            std::to_string(spec.min_properties) + " > " + std::to_string(spec.properties.size())
    );
  }
  return ResultOk(std::move(spec));
}

Result<ConstSpec, SchemaError> SchemaParser::ParseConst(const picojson::object& schema) {
  ConstSpec spec;
  spec.json_value = schema.at("const").serialize();
  return ResultOk(std::move(spec));
}

Result<EnumSpec, SchemaError> SchemaParser::ParseEnum(const picojson::object& schema) {
  EnumSpec spec;
  if (!schema.at("enum").is<picojson::array>()) {
    return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "enum must be an array");
  }
  for (const auto& value : schema.at("enum").get<picojson::array>()) {
    spec.json_values.push_back(value.serialize());
  }
  return ResultOk(std::move(spec));
}

Result<RefSpec, SchemaError> SchemaParser::ParseRef(const picojson::object& schema) {
  if (!schema.at("$ref").is<std::string>()) {
    return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "$ref must be a string");
  }
  RefSpec spec;
  spec.uri = schema.at("$ref").get<std::string>();
  return ResultOk(std::move(spec));
}

Result<SchemaSpecPtr, SchemaError> SchemaParser::ResolveRef(
    const std::string& uri, const std::string& rule_name_hint
) {
  if (ref_cache_.count(uri)) return ResultOk(ref_cache_[uri]);

  if (uri == "#") {
    auto placeholder = SchemaSpec::Make(AnySpec{}, "", "root");
    ref_cache_[uri] = placeholder;
    auto result = Parse(root_schema_, "root");
    if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
    auto resolved = std::move(result).Unwrap();
    ref_cache_[uri] = resolved;
    return ResultOk(resolved);
  }

  if (uri.size() < 2 || uri[0] != '#' || uri[1] != '/') {
    XGRAMMAR_LOG(WARNING) << "URI should either be '#' or start with '#/' but got " << uri;
    return ResultOk(SchemaSpec::Make(AnySpec{}, "", "any"));
  }

  std::vector<std::string> parts;
  std::stringstream ss(uri.substr(2));
  std::string part;
  std::string new_rule_name_prefix;
  while (std::getline(ss, part, '/')) {
    if (!part.empty()) parts.push_back(part);
    if (!new_rule_name_prefix.empty()) new_rule_name_prefix += "_";
    for (const auto& c : part) {
      if (std::isalpha(c) || c == '_' || c == '-' || c == '.') new_rule_name_prefix += c;
    }
  }

  auto current = std::cref(root_schema_);
  for (const auto& p : parts) {
    if (!current.get().is<picojson::object>() || !current.get().contains(p)) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "Cannot find field " + p + " in " + uri
      );
    }
    current = current.get().get(p);
  }

  auto result = Parse(current, new_rule_name_prefix);
  if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
  auto resolved = std::move(result).Unwrap();
  ref_cache_[uri] = resolved;
  return ResultOk(resolved);
}

Result<std::vector<SchemaSpecPtr>, SchemaError> SchemaParser::ParseCompositeOptions(
    const picojson::object& schema,
    const std::string& keyword,
    const std::string& rule_name_prefix
) {
  if (!schema.at(keyword).is<picojson::array>()) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kInvalidSchema, keyword + " must be an array"
    );
  }

  std::optional<SchemaSpecPtr> base_spec;
  picojson::object base_schema = schema;
  base_schema.erase(keyword);
  if (!base_schema.empty()) {
    auto base_result = Parse(picojson::value(base_schema), rule_name_prefix + "_base");
    if (base_result.IsErr()) return ResultErr(std::move(base_result).UnwrapErr());
    base_spec = std::move(base_result).Unwrap();
  }

  std::vector<SchemaSpecPtr> options;
  int idx = 0;
  for (const auto& option : schema.at(keyword).get<picojson::array>()) {
    picojson::value option_to_parse = option;
    if (keyword == "anyOf" && base_spec.has_value() && option.is<picojson::object>() &&
        std::holds_alternative<ObjectSpec>((*base_spec)->spec)) {
      const auto& base_object = std::get<ObjectSpec>((*base_spec)->spec);
      const auto& option_obj = option.get<picojson::object>();
      bool has_concrete_base_properties = !base_object.properties.empty();
      bool is_required_only_fragment =
          !option_obj.count("properties") && !option_obj.count("patternProperties") &&
          !option_obj.count("propertyNames") && !option_obj.count("additionalProperties") &&
          !option_obj.count("unevaluatedProperties") &&
          (option_obj.count("required") || option_obj.count("dependentRequired") ||
           option_obj.count("not") || option_obj.count("minProperties") ||
           option_obj.count("maxProperties"));
      if (has_concrete_base_properties && is_required_only_fragment) {
        option_to_parse = NormalizePartialObjectFragment(option);
      }
    }

    auto option_result = Parse(option_to_parse, rule_name_prefix + "_" + std::to_string(idx));
    if (option_result.IsErr()) return ResultErr(std::move(option_result).UnwrapErr());
    auto option_spec = std::move(option_result).Unwrap();
    if (base_spec.has_value()) {
      auto merge_result = MergeSchemaSpecs(
          *base_spec, option_spec, rule_name_prefix + "_merged_" + std::to_string(idx)
      );
      if (merge_result.IsErr()) {
        auto err = std::move(merge_result).UnwrapErr();
        if (err.Type() == SchemaErrorType::kUnsatisfiableSchema) {
          ++idx;
          continue;
        }
        return ResultErr<SchemaError>(std::move(err));
      }
      option_spec = std::move(merge_result).Unwrap();
    }
    options.push_back(std::move(option_spec));
    ++idx;
  }

  if (options.empty()) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema,
        keyword + " options have no satisfiable intersection with sibling constraints"
    );
  }
  return ResultOk(std::move(options));
}

Result<AnyOfSpec, SchemaError> SchemaParser::ParseAnyOf(const picojson::object& schema) {
  AnyOfSpec spec;
  auto options = ParseCompositeOptions(schema, "anyOf", "case");
  if (options.IsErr()) return ResultErr(std::move(options).UnwrapErr());
  spec.options = std::move(options).Unwrap();
  return ResultOk(std::move(spec));
}

Result<std::optional<SchemaSpecPtr>, SchemaError> SchemaParser::TryParseExclusiveObjectOneOf(
    const picojson::object& schema, const std::string& rule_name_hint
) {
  if (!schema.at("oneOf").is<picojson::array>()) {
    return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "oneOf must be an array");
  }

  picojson::object base_schema = schema;
  base_schema.erase("oneOf");
  if (base_schema.empty()) {
    return ResultOk(std::nullopt);
  }

  auto base_result = Parse(picojson::value(base_schema), rule_name_hint + "_base");
  if (base_result.IsErr()) return ResultErr(std::move(base_result).UnwrapErr());
  auto base_spec = std::move(base_result).Unwrap();
  if (!std::holds_alternative<ObjectSpec>(base_spec->spec)) {
    return ResultOk(std::nullopt);
  }

  const auto& base_object = std::get<ObjectSpec>(base_spec->spec);
  if (!base_object.pattern_properties.empty() || base_object.property_names ||
      base_object.allow_additional_properties || base_object.allow_unevaluated_properties) {
    return ResultOk(std::nullopt);
  }

  auto parse_required_group =
      [](const picojson::value& value, const std::string& context
      ) -> Result<std::vector<std::string>, SchemaError> {
    if (!value.is<picojson::array>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, context + " must be an array"
      );
    }
    std::vector<std::string> group;
    for (const auto& item : value.get<picojson::array>()) {
      if (!item.is<std::string>()) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema, context + " must contain only strings"
        );
      }
      const auto& name = item.get<std::string>();
      if (std::find(group.begin(), group.end(), name) == group.end()) {
        group.push_back(name);
      }
    }
    return ResultOk(std::move(group));
  };

  struct PresencePredicate {
    std::vector<std::string> required_group;
    std::vector<std::vector<std::string>> forbidden_groups;
    std::vector<ObjectSpec::Property> property_refinements;
  };

  std::vector<PresencePredicate> predicates;
  std::vector<std::string> involved_properties;
  auto append_unique_name = [](std::vector<std::string>* names, const std::string& name) {
    if (std::find(names->begin(), names->end(), name) == names->end()) {
      names->push_back(name);
    }
  };
  auto is_ignorable_option_key = [](const std::string& key) {
    static const std::unordered_set<std::string> kIgnoredKeys = {
        "title",
        "default",
        "description",
        "examples",
        "deprecated",
        "readOnly",
        "writeOnly",
        "$comment",
    };
    return kIgnoredKeys.count(key) != 0;
  };
  auto parse_supported_not_groups =
      [&](const picojson::value& value) -> Result<std::vector<std::vector<std::string>>, SchemaError> {
    if (!value.is<picojson::object>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "oneOf not branch must be an object"
      );
    }
    const auto& not_obj = value.get<picojson::object>();
    std::vector<std::vector<std::string>> groups;
    if (not_obj.count("required")) {
      for (const auto& key : not_obj.ordered_keys()) {
        if (key == "required" || key == "type" || is_ignorable_option_key(key)) {
          continue;
        }
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema,
            "oneOf not branch only supports required or anyOf of required groups"
        );
      }
      if (not_obj.count("type")) {
        if (!not_obj.at("type").is<std::string>() || not_obj.at("type").get<std::string>() != "object") {
          return ResultErr<SchemaError>(
              SchemaErrorType::kInvalidSchema, "oneOf not branch type must be object"
          );
        }
      }
      auto required_result = parse_required_group(not_obj.at("required"), "required");
      if (required_result.IsErr()) return ResultErr(std::move(required_result).UnwrapErr());
      groups.push_back(std::move(required_result).Unwrap());
      return ResultOk(std::move(groups));
    }
    if (!not_obj.count("anyOf") || !not_obj.at("anyOf").is<picojson::array>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema,
          "oneOf not branch only supports required or anyOf of required groups"
      );
    }
    for (const auto& key : not_obj.ordered_keys()) {
      if (key == "anyOf" || key == "type" || is_ignorable_option_key(key)) {
        continue;
      }
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema,
          "oneOf not branch only supports required or anyOf of required groups"
      );
    }
    if (not_obj.count("type")) {
      if (!not_obj.at("type").is<std::string>() || not_obj.at("type").get<std::string>() != "object") {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema, "oneOf not branch type must be object"
        );
      }
    }
    for (const auto& negated_option : not_obj.at("anyOf").get<picojson::array>()) {
      if (!negated_option.is<picojson::object>()) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema, "oneOf not anyOf entries must be objects"
        );
      }
      const auto& negated_obj = negated_option.get<picojson::object>();
      if (!negated_obj.count("required") || negated_obj.size() != 1) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema,
            "oneOf not anyOf entries must contain only required"
        );
      }
      auto required_result = parse_required_group(negated_obj.at("required"), "required");
      if (required_result.IsErr()) return ResultErr(std::move(required_result).UnwrapErr());
      groups.push_back(std::move(required_result).Unwrap());
    }
    return ResultOk(std::move(groups));
  };

  for (const auto& option : schema.at("oneOf").get<picojson::array>()) {
    if (!option.is<picojson::object>()) {
      return ResultOk(std::nullopt);
    }
    const auto& option_obj = option.get<picojson::object>();
    if (option_obj.count("type")) {
      if (!option_obj.at("type").is<std::string>() ||
          option_obj.at("type").get<std::string>() != "object") {
        return ResultOk(std::nullopt);
      }
    }

    for (const auto& key : option_obj.ordered_keys()) {
      if (key == "required" || key == "properties" || key == "not" || key == "type" ||
          is_ignorable_option_key(key)) {
        continue;
      }
      return ResultOk(std::nullopt);
    }

    PresencePredicate predicate;
    bool has_supported_constraint = false;
    if (option_obj.count("required")) {
      auto required_result = parse_required_group(option_obj.at("required"), "required");
      if (required_result.IsErr()) return ResultErr(std::move(required_result).UnwrapErr());
      predicate.required_group = std::move(required_result).Unwrap();
      for (const auto& property_name : predicate.required_group) {
        append_unique_name(&involved_properties, property_name);
      }
      has_supported_constraint = true;
    }

    if (option_obj.count("not")) {
      auto forbidden_result = parse_supported_not_groups(option_obj.at("not"));
      if (forbidden_result.IsErr()) return ResultErr(std::move(forbidden_result).UnwrapErr());
      predicate.forbidden_groups = std::move(forbidden_result).Unwrap();
      for (const auto& group : predicate.forbidden_groups) {
        for (const auto& property_name : group) {
          append_unique_name(&involved_properties, property_name);
        }
      }
      has_supported_constraint = true;
    }

    if (!has_supported_constraint) {
      return ResultOk(std::nullopt);
    }

    if (option_obj.count("properties")) {
      if (!option_obj.at("properties").is<picojson::object>()) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema, "properties must be an object"
        );
      }
      const auto& properties_obj = option_obj.at("properties").get<picojson::object>();
      for (const auto& property_name : properties_obj.ordered_keys()) {
        if (std::find(predicate.required_group.begin(), predicate.required_group.end(), property_name) ==
                predicate.required_group.end() &&
            !base_object.required.count(property_name)) {
          return ResultOk(std::nullopt);
        }
        append_unique_name(&involved_properties, property_name);
        auto property_result = Parse(properties_obj.at(property_name), property_name);
        if (property_result.IsErr()) return ResultErr(std::move(property_result).UnwrapErr());
        predicate.property_refinements.push_back({property_name, std::move(property_result).Unwrap()});
      }
    }

    predicates.push_back(std::move(predicate));
  }

  if (involved_properties.empty()) {
    return ResultOk(std::nullopt);
  }
  if (involved_properties.size() > 10) {
    return ResultOk(std::nullopt);
  }

  std::unordered_map<std::string, ObjectSpec::Property> explicit_properties;
  for (const auto& property : base_object.properties) {
    explicit_properties[property.name] = property;
  }
  for (const auto& property_name : involved_properties) {
    if (!explicit_properties.count(property_name)) {
      return ResultOk(std::nullopt);
    }
  }

  auto is_group_satisfied =
      [](const std::vector<std::string>& group, const std::unordered_set<std::string>& present) {
    return std::all_of(group.begin(), group.end(), [&](const std::string& property_name) {
      return present.count(property_name);
    });
  };

  AnyOfSpec exact_anyof;
  uint64_t assignment_count = uint64_t{1} << involved_properties.size();
  for (uint64_t mask = 0; mask < assignment_count; ++mask) {
    std::unordered_set<std::string> present_properties;
    for (size_t i = 0; i < involved_properties.size(); ++i) {
      if (mask & (uint64_t{1} << i)) {
        present_properties.insert(involved_properties[i]);
      }
    }

    bool violates_base_required = false;
    for (const auto& property_name : base_object.required) {
      if (std::find(involved_properties.begin(), involved_properties.end(), property_name) !=
              involved_properties.end() &&
          !present_properties.count(property_name)) {
        violates_base_required = true;
        break;
      }
    }
    if (violates_base_required) {
      continue;
    }

    int matched_predicates = 0;
    int matched_predicate_index = -1;
    for (size_t predicate_idx = 0; predicate_idx < predicates.size(); ++predicate_idx) {
      const auto& predicate = predicates[predicate_idx];
      bool matched = predicate.required_group.empty() ||
                     is_group_satisfied(predicate.required_group, present_properties);
      if (matched) {
        matched = std::all_of(
            predicate.forbidden_groups.begin(),
            predicate.forbidden_groups.end(),
            [&](const std::vector<std::string>& group) {
              return !is_group_satisfied(group, present_properties);
            });
      }
      if (matched) {
        matched_predicate_index = static_cast<int>(predicate_idx);
        ++matched_predicates;
        if (matched_predicates > 1) {
          break;
        }
      }
    }
    if (matched_predicates != 1) {
      continue;
    }

    ObjectSpec branch_spec = base_object;
    branch_spec.properties.clear();
    for (const auto& property : base_object.properties) {
      bool is_involved =
          std::find(involved_properties.begin(), involved_properties.end(), property.name) !=
          involved_properties.end();
      if (!is_involved || present_properties.count(property.name)) {
        branch_spec.properties.push_back(property);
      }
    }
    for (const auto& property_name : involved_properties) {
      if (present_properties.count(property_name)) {
        branch_spec.required.insert(property_name);
      } else {
        branch_spec.required.erase(property_name);
      }
    }

    bool branch_is_unsatisfiable = false;
    const auto& matched_predicate = predicates[matched_predicate_index];
    for (const auto& refinement : matched_predicate.property_refinements) {
      auto property_it = std::find_if(
          branch_spec.properties.begin(),
          branch_spec.properties.end(),
          [&](const ObjectSpec::Property& property) { return property.name == refinement.name; }
      );
      if (property_it == branch_spec.properties.end()) {
        branch_is_unsatisfiable = true;
        break;
      }
      auto merge_result = MergeSchemaSpecs(
          property_it->schema, refinement.schema, rule_name_hint + "_" + refinement.name
      );
      if (merge_result.IsErr()) {
        auto err = std::move(merge_result).UnwrapErr();
        if (err.Type() == SchemaErrorType::kUnsatisfiableSchema) {
          branch_is_unsatisfiable = true;
          break;
        }
        return ResultErr<SchemaError>(std::move(err));
      }
      property_it->schema = std::move(merge_result).Unwrap();
    }
    if (branch_is_unsatisfiable) {
      continue;
    }

    if (branch_spec.max_properties != -1 &&
        static_cast<int>(branch_spec.required.size()) > branch_spec.max_properties) {
      continue;
    }
    if (branch_spec.max_properties != -1 && branch_spec.min_properties > branch_spec.max_properties) {
      continue;
    }
    if (branch_spec.min_properties > static_cast<int>(branch_spec.properties.size())) {
      continue;
    }

    exact_anyof.options.push_back(
        SchemaSpec::Make(std::move(branch_spec), "", rule_name_hint + "_presence")
    );
  }

  if (exact_anyof.options.empty()) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema,
        "oneOf presence constraints have no satisfiable exclusive branches"
    );
  }
  if (exact_anyof.options.size() == 1) {
    return ResultOk<std::optional<SchemaSpecPtr>>(exact_anyof.options[0]);
  }
  return ResultOk<std::optional<SchemaSpecPtr>>(
      SchemaSpec::Make(std::move(exact_anyof), "", rule_name_hint)
  );
}

Result<SchemaSpecPtr, SchemaError> SchemaParser::ParseOneOf(
    const picojson::object& schema, const std::string& rule_name_hint
) {
  auto exact_object_result = TryParseExclusiveObjectOneOf(schema, rule_name_hint);
  if (exact_object_result.IsErr()) return ResultErr(std::move(exact_object_result).UnwrapErr());
  auto exact_object_spec = std::move(exact_object_result).Unwrap();
  if (exact_object_spec.has_value()) {
    return ResultOk(*exact_object_spec);
  }

  OneOfSpec spec;
  auto options = ParseCompositeOptions(schema, "oneOf", "case");
  if (options.IsErr()) return ResultErr(std::move(options).UnwrapErr());
  spec.options = std::move(options).Unwrap();
  return ResultOk(SchemaSpec::Make(std::move(spec), "", rule_name_hint));
}

Result<AllOfSpec, SchemaError> SchemaParser::ParseAllOf(const picojson::object& schema) {
  AllOfSpec spec;
  std::vector<SchemaSpecPtr> merge_candidates;

  picojson::object base_schema = schema;
  base_schema.erase("allOf");
  if (!base_schema.empty()) {
    auto base_result = Parse(picojson::value(base_schema), "all_base");
    if (base_result.IsErr()) return ResultErr(std::move(base_result).UnwrapErr());
    merge_candidates.push_back(std::move(base_result).Unwrap());
  }

  if (!schema.at("allOf").is<picojson::array>()) {
    return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "allOf must be an array");
  }
  int idx = 0;
  for (const auto& sub_schema : schema.at("allOf").get<picojson::array>()) {
    Result<SchemaSpecPtr, SchemaError> sub_result = [&]() -> Result<SchemaSpecPtr, SchemaError> {
      if (!base_schema.empty() && sub_schema.is<picojson::object>()) {
        const auto& sub_schema_obj = sub_schema.get<picojson::object>();
        if (sub_schema_obj.count("oneOf") || sub_schema_obj.count("if") ||
            sub_schema_obj.count("then") || sub_schema_obj.count("else")) {
          picojson::object merged_sub_schema = base_schema;
          for (const auto& key : sub_schema_obj.ordered_keys()) {
            if (key == "required" && merged_sub_schema.count("required") &&
                merged_sub_schema.at("required").is<picojson::array>() &&
                sub_schema_obj.at(key).is<picojson::array>()) {
              std::vector<std::string> merged_required;
              for (const auto& item : merged_sub_schema.at("required").get<picojson::array>()) {
                if (item.is<std::string>()) {
                  merged_required.push_back(item.get<std::string>());
                }
              }
              for (const auto& item : sub_schema_obj.at(key).get<picojson::array>()) {
                if (item.is<std::string>() &&
                    std::find(
                        merged_required.begin(), merged_required.end(), item.get<std::string>()
                    ) == merged_required.end()) {
                  merged_required.push_back(item.get<std::string>());
                }
              }
              picojson::array required_arr;
              for (const auto& name : merged_required) {
                required_arr.push_back(picojson::value(name));
              }
              merged_sub_schema[key] = picojson::value(required_arr);
            } else {
              merged_sub_schema[key] = sub_schema_obj.at(key);
            }
          }
          if (sub_schema_obj.count("oneOf")) {
            return ParseOneOf(merged_sub_schema, "all_" + std::to_string(idx));
          }
          return ParseConditional(merged_sub_schema, "all_" + std::to_string(idx));
        }
      }
      return Parse(sub_schema, "all_" + std::to_string(idx));
    }();
    if (sub_result.IsErr()) return ResultErr(std::move(sub_result).UnwrapErr());
    auto parsed = std::move(sub_result).Unwrap();
    merge_candidates.push_back(std::move(parsed));
    ++idx;
  }

  auto merged_result = MergeAllOfSchemas(merge_candidates, "all");
  if (merged_result.IsErr()) return ResultErr(std::move(merged_result).UnwrapErr());
  spec.schemas = {std::move(merged_result).Unwrap()};
  return ResultOk(std::move(spec));
}

Result<TypeArraySpec, SchemaError> SchemaParser::ParseTypeArray(
    const picojson::object& schema, const std::string& rule_name_hint
) {
  TypeArraySpec spec;
  auto type_array = schema.at("type").get<picojson::array>();
  picojson::object schema_copy = schema;
  if (type_array.empty()) {
    schema_copy.erase("type");
    auto any_result = Parse(picojson::value(schema_copy), rule_name_hint);
    if (any_result.IsErr()) return ResultErr(std::move(any_result).UnwrapErr());
    spec.type_schemas.push_back(std::move(any_result).Unwrap());
    return ResultOk(std::move(spec));
  }
  for (const auto& type : type_array) {
    if (!type.is<std::string>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "type must be a string or an array of strings"
      );
    }
    schema_copy["type"] = type;
    auto type_result =
        Parse(picojson::value(schema_copy), rule_name_hint + "_" + type.get<std::string>());
    if (type_result.IsErr()) return ResultErr(std::move(type_result).UnwrapErr());
    spec.type_schemas.push_back(std::move(type_result).Unwrap());
  }
  return ResultOk(std::move(spec));
}

Result<bool, SchemaError> SchemaParser::AreSchemasDisjoint(
    const SchemaSpecPtr& lhs, const SchemaSpecPtr& rhs, const std::string& rule_name_hint
) {
  auto merge_result = MergeSchemaSpecs(lhs, rhs, rule_name_hint);
  if (merge_result.IsOk()) {
    auto merged = std::move(merge_result).Unwrap();
    if (std::holds_alternative<ObjectSpec>(merged->spec)) {
      const auto& object = std::get<ObjectSpec>(merged->spec);
      if (!object.dependent_required.empty() || !object.forbidden_groups.empty()) {
        auto normalized_result =
            NormalizeObjectPresenceConstraints(object, rule_name_hint + "_presence");
        if (normalized_result.IsOk()) {
          return ResultOk(false);
        }
        auto err = std::move(normalized_result).UnwrapErr();
        if (err.Type() == SchemaErrorType::kUnsatisfiableSchema) {
          return ResultOk(true);
        }
        return ResultErr<SchemaError>(std::move(err));
      }
    }
    return ResultOk(false);
  }
  auto err = std::move(merge_result).UnwrapErr();
  if (err.Type() == SchemaErrorType::kUnsatisfiableSchema) {
    return ResultOk(true);
  }
  return ResultErr<SchemaError>(std::move(err));
}

Result<SchemaSpecPtr, SchemaError> SchemaParser::NormalizeObjectPresenceConstraints(
    const ObjectSpec& object, const std::string& rule_name_hint
) {
  constexpr size_t kMaxExactPresenceProperties = 10;
  if (object.dependent_required.empty() && object.forbidden_groups.empty()) {
    return ResultOk(SchemaSpec::Make(object, "", rule_name_hint));
  }
  if (!object.pattern_properties.empty() || object.property_names ||
      object.allow_additional_properties || object.allow_unevaluated_properties) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kInvalidSchema,
        "object presence constraints are only supported for closed objects with explicit properties"
    );
  }

  std::unordered_map<std::string, ObjectSpec::Property> explicit_properties;
  for (const auto& property : object.properties) {
    explicit_properties[property.name] = property;
  }

  std::vector<std::string> involved_properties;
  auto append_unique_name = [](std::vector<std::string>* names, const std::string& name) {
    if (std::find(names->begin(), names->end(), name) == names->end()) {
      names->push_back(name);
    }
  };

  for (const auto& [trigger, deps] : object.dependent_required) {
    if (explicit_properties.count(trigger)) {
      append_unique_name(&involved_properties, trigger);
    }
    for (const auto& dep : deps) {
      if (explicit_properties.count(dep)) {
        append_unique_name(&involved_properties, dep);
      }
    }
  }
  for (const auto& group : object.forbidden_groups) {
    if (group.empty()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema,
          "object presence constraints have no satisfiable assignments"
      );
    }
    for (const auto& name : group) {
      if (explicit_properties.count(name)) {
        append_unique_name(&involved_properties, name);
      }
    }
  }

  if (involved_properties.empty()) {
    ObjectSpec normalized = object;
    normalized.dependent_required.clear();
    normalized.forbidden_groups.clear();
    return ResultOk(SchemaSpec::Make(std::move(normalized), "", rule_name_hint));
  }
  if (involved_properties.size() > kMaxExactPresenceProperties) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kInvalidSchema,
        "object presence constraints involve too many explicit properties to normalize exactly: " +
            rule_name_hint
    );
  }

  AnyOfSpec exact_anyof;
  uint64_t assignment_count = uint64_t{1} << involved_properties.size();
  for (uint64_t mask = 0; mask < assignment_count; ++mask) {
    std::unordered_set<std::string> present_properties;
    for (size_t i = 0; i < involved_properties.size(); ++i) {
      if (mask & (uint64_t{1} << i)) {
        present_properties.insert(involved_properties[i]);
      }
    }

    bool violates_required = false;
    for (const auto& property_name : object.required) {
      bool is_involved =
          std::find(involved_properties.begin(), involved_properties.end(), property_name) !=
          involved_properties.end();
      if (is_involved && !present_properties.count(property_name)) {
        violates_required = true;
        break;
      }
    }
    if (violates_required) {
      continue;
    }

    bool violates_dependency = false;
    for (const auto& [trigger, deps] : object.dependent_required) {
      if (!explicit_properties.count(trigger) || !present_properties.count(trigger)) {
        continue;
      }
      for (const auto& dep : deps) {
        if (!explicit_properties.count(dep) || !present_properties.count(dep)) {
          violates_dependency = true;
          break;
        }
      }
      if (violates_dependency) {
        break;
      }
    }
    if (violates_dependency) {
      continue;
    }

    bool violates_forbidden = false;
    for (const auto& group : object.forbidden_groups) {
      bool all_present = std::all_of(
          group.begin(),
          group.end(),
          [&](const std::string& name) {
            return explicit_properties.count(name) && present_properties.count(name);
          }
      );
      if (all_present) {
        violates_forbidden = true;
        break;
      }
    }
    if (violates_forbidden) {
      continue;
    }

    ObjectSpec branch_spec = object;
    branch_spec.dependent_required.clear();
    branch_spec.forbidden_groups.clear();
    branch_spec.properties.clear();
    for (const auto& property : object.properties) {
      bool is_involved =
          std::find(involved_properties.begin(), involved_properties.end(), property.name) !=
          involved_properties.end();
      if (!is_involved || present_properties.count(property.name)) {
        branch_spec.properties.push_back(property);
      }
    }
    for (const auto& property_name : involved_properties) {
      if (present_properties.count(property_name)) {
        branch_spec.required.insert(property_name);
      } else {
        branch_spec.required.erase(property_name);
      }
    }

    if (branch_spec.max_properties != -1 &&
        static_cast<int>(branch_spec.required.size()) > branch_spec.max_properties) {
      continue;
    }
    if (branch_spec.max_properties != -1 && branch_spec.min_properties > branch_spec.max_properties) {
      continue;
    }
    if (branch_spec.min_properties > static_cast<int>(branch_spec.properties.size())) {
      continue;
    }

    exact_anyof.options.push_back(
        SchemaSpec::Make(std::move(branch_spec), "", rule_name_hint + "_presence_constraint")
    );
  }

  if (exact_anyof.options.empty()) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema,
        "object presence constraints have no satisfiable assignments: " + rule_name_hint
    );
  }
  if (exact_anyof.options.size() == 1) {
    return ResultOk(exact_anyof.options[0]);
  }
  return ResultOk(SchemaSpec::Make(std::move(exact_anyof), "", rule_name_hint));
}

Result<SchemaSpecPtr, SchemaError> SchemaParser::NormalizeExclusiveDisjunctions(
    const SchemaSpecPtr& spec
) {
  std::unordered_set<const SchemaSpec*> active_specs;
  std::unordered_set<const SchemaSpec*> normalized_specs;
  return NormalizeExclusiveDisjunctionsImpl(
      spec, "root", &active_specs, &normalized_specs
  );
}

Result<SchemaSpecPtr, SchemaError> SchemaParser::NormalizeExclusiveDisjunctionsImpl(
    const SchemaSpecPtr& spec,
    const std::string& rule_name_hint,
    std::unordered_set<const SchemaSpec*>* active_specs,
    std::unordered_set<const SchemaSpec*>* normalized_specs
) {
  if (!spec || normalized_specs->count(spec.get()) || active_specs->count(spec.get())) {
    return ResultOk(spec);
  }
  active_specs->insert(spec.get());

  auto finish_ok = [&]() -> Result<SchemaSpecPtr, SchemaError> {
    active_specs->erase(spec.get());
    normalized_specs->insert(spec.get());
    return ResultOk(spec);
  };
  auto finish_err = [&](SchemaError err) -> Result<SchemaSpecPtr, SchemaError> {
    active_specs->erase(spec.get());
    return ResultErr<SchemaError>(std::move(err));
  };

  auto normalize_child = [&](const SchemaSpecPtr& child,
                             const std::string& child_hint) -> Result<SchemaSpecPtr, SchemaError> {
    return NormalizeExclusiveDisjunctionsImpl(child, child_hint, active_specs, normalized_specs);
  };

  if (std::holds_alternative<RefSpec>(spec->spec)) {
    const auto& ref = std::get<RefSpec>(spec->spec);
    auto resolved_result = ResolveRef(ref.uri, rule_name_hint + "_ref");
    if (resolved_result.IsErr()) return finish_err(std::move(resolved_result).UnwrapErr());
    auto normalized_result =
        normalize_child(std::move(resolved_result).Unwrap(), rule_name_hint + "_target");
    if (normalized_result.IsErr()) return finish_err(std::move(normalized_result).UnwrapErr());
    return finish_ok();
  }

  if (std::holds_alternative<AnyOfSpec>(spec->spec)) {
    auto& anyof = std::get<AnyOfSpec>(spec->spec);
    for (size_t i = 0; i < anyof.options.size(); ++i) {
      auto normalized_result = normalize_child(
          anyof.options[i], rule_name_hint + "_anyof_" + std::to_string(i)
      );
      if (normalized_result.IsErr()) return finish_err(std::move(normalized_result).UnwrapErr());
      anyof.options[i] = std::move(normalized_result).Unwrap();
    }
    return finish_ok();
  }

  if (std::holds_alternative<OneOfSpec>(spec->spec)) {
    auto oneof = std::get<OneOfSpec>(spec->spec);
    for (size_t i = 0; i < oneof.options.size(); ++i) {
      auto normalized_result = normalize_child(
          oneof.options[i], rule_name_hint + "_oneof_" + std::to_string(i)
      );
      if (normalized_result.IsErr()) return finish_err(std::move(normalized_result).UnwrapErr());
      oneof.options[i] = std::move(normalized_result).Unwrap();
    }

    for (size_t i = 0; i < oneof.options.size(); ++i) {
      for (size_t j = i + 1; j < oneof.options.size(); ++j) {
        auto disjoint_result = AreSchemasDisjoint(
            oneof.options[i],
            oneof.options[j],
            rule_name_hint + "_overlap_" + std::to_string(i) + "_" + std::to_string(j)
        );
        if (disjoint_result.IsErr()) {
          auto err = std::move(disjoint_result).UnwrapErr();
          return finish_err(SchemaError(
              SchemaErrorType::kInvalidSchema,
              std::string("oneOf branches could not be proven mutually exclusive: ") + err.what()
          ));
        }
        if (!std::move(disjoint_result).Unwrap()) {
          return finish_err(SchemaError(
              SchemaErrorType::kInvalidSchema, "oneOf branches are not mutually exclusive"
          ));
        }
      }
    }

    if (oneof.options.size() == 1) {
      spec->spec = oneof.options[0]->spec;
    } else {
      spec->spec = AnyOfSpec{std::move(oneof.options)};
    }
    return finish_ok();
  }

  if (std::holds_alternative<AllOfSpec>(spec->spec)) {
    auto& allof = std::get<AllOfSpec>(spec->spec);
    for (size_t i = 0; i < allof.schemas.size(); ++i) {
      auto normalized_result = normalize_child(
          allof.schemas[i], rule_name_hint + "_allof_" + std::to_string(i)
      );
      if (normalized_result.IsErr()) return finish_err(std::move(normalized_result).UnwrapErr());
      allof.schemas[i] = std::move(normalized_result).Unwrap();
    }
    return finish_ok();
  }

  if (std::holds_alternative<TypeArraySpec>(spec->spec)) {
    auto& type_array = std::get<TypeArraySpec>(spec->spec);
    for (size_t i = 0; i < type_array.type_schemas.size(); ++i) {
      auto normalized_result = normalize_child(
          type_array.type_schemas[i], rule_name_hint + "_type_" + std::to_string(i)
      );
      if (normalized_result.IsErr()) return finish_err(std::move(normalized_result).UnwrapErr());
      type_array.type_schemas[i] = std::move(normalized_result).Unwrap();
    }
    return finish_ok();
  }

  if (std::holds_alternative<ArraySpec>(spec->spec)) {
    auto& array = std::get<ArraySpec>(spec->spec);
    for (size_t i = 0; i < array.prefix_items.size(); ++i) {
      auto normalized_result = normalize_child(
          array.prefix_items[i], rule_name_hint + "_prefix_" + std::to_string(i)
      );
      if (normalized_result.IsErr()) return finish_err(std::move(normalized_result).UnwrapErr());
      array.prefix_items[i] = std::move(normalized_result).Unwrap();
    }
    if (array.additional_items) {
      auto normalized_result = normalize_child(array.additional_items, rule_name_hint + "_additional");
      if (normalized_result.IsErr()) return finish_err(std::move(normalized_result).UnwrapErr());
      array.additional_items = std::move(normalized_result).Unwrap();
    }
    if (array.contains_item) {
      auto normalized_result = normalize_child(array.contains_item, rule_name_hint + "_contains");
      if (normalized_result.IsErr()) return finish_err(std::move(normalized_result).UnwrapErr());
      array.contains_item = std::move(normalized_result).Unwrap();
    }
    return finish_ok();
  }

  if (std::holds_alternative<ObjectSpec>(spec->spec)) {
    auto& object = std::get<ObjectSpec>(spec->spec);
    for (auto& property : object.properties) {
      auto normalized_result =
          normalize_child(property.schema, rule_name_hint + "_" + property.name);
      if (normalized_result.IsErr()) return finish_err(std::move(normalized_result).UnwrapErr());
      property.schema = std::move(normalized_result).Unwrap();
    }
    for (size_t i = 0; i < object.pattern_properties.size(); ++i) {
      auto normalized_result = normalize_child(
          object.pattern_properties[i].schema, rule_name_hint + "_pattern_" + std::to_string(i)
      );
      if (normalized_result.IsErr()) return finish_err(std::move(normalized_result).UnwrapErr());
      object.pattern_properties[i].schema = std::move(normalized_result).Unwrap();
    }
    if (object.additional_properties_schema) {
      auto normalized_result = normalize_child(
          object.additional_properties_schema, rule_name_hint + "_additional"
      );
      if (normalized_result.IsErr()) return finish_err(std::move(normalized_result).UnwrapErr());
      object.additional_properties_schema = std::move(normalized_result).Unwrap();
    }
    if (object.unevaluated_properties_schema) {
      auto normalized_result = normalize_child(
          object.unevaluated_properties_schema, rule_name_hint + "_unevaluated"
      );
      if (normalized_result.IsErr()) return finish_err(std::move(normalized_result).UnwrapErr());
      object.unevaluated_properties_schema = std::move(normalized_result).Unwrap();
    }
    if (object.property_names) {
      auto normalized_result =
          normalize_child(object.property_names, rule_name_hint + "_property_names");
      if (normalized_result.IsErr()) return finish_err(std::move(normalized_result).UnwrapErr());
      object.property_names = std::move(normalized_result).Unwrap();
    }
    if (!object.dependent_required.empty() || !object.forbidden_groups.empty()) {
      auto normalized_result =
          NormalizeObjectPresenceConstraints(object, rule_name_hint + "_presence");
      if (normalized_result.IsErr()) return finish_err(std::move(normalized_result).UnwrapErr());
      spec->spec = std::move(normalized_result).Unwrap()->spec;
    }
    return finish_ok();
  }

  return finish_ok();
}

}  // namespace

// ==================== IndentManager Implementation ====================

IndentManager::IndentManager(
    std::optional<int> indent,
    const std::string& separator,
    bool any_whitespace,
    std::optional<int> max_whitespace_cnt
)
    : any_whitespace_(any_whitespace),
      enable_newline_(indent.has_value()),
      indent_(indent.value_or(0)),
      separator_(separator),
      total_indent_(0),
      is_first_({true}),
      max_whitespace_cnt_(max_whitespace_cnt) {
  if (max_whitespace_cnt.has_value() && max_whitespace_cnt.value() <= 0) {
    XGRAMMAR_LOG(FATAL) << "max_whitespace_cnt must be positive.";
  }
}

void IndentManager::StartIndent() {
  total_indent_ += indent_;
  is_first_.push_back(true);
}

void IndentManager::EndIndent() {
  total_indent_ -= indent_;
  is_first_.pop_back();
}

std::string IndentManager::StartSeparator() {
  if (any_whitespace_) {
    if (!max_whitespace_cnt_.has_value()) {
      return "[ \\n\\t]*";
    } else {
      return "[ \\n\\t]{0," + std::to_string(max_whitespace_cnt_.value()) + "}";
    }
  }
  if (!enable_newline_) {
    return "\"\"";
  }
  return "\"\\n" + std::string(total_indent_, ' ') + "\"";
}

std::string IndentManager::MiddleSeparator() {
  if (any_whitespace_) {
    std::string whitespace_part;
    if (!max_whitespace_cnt_.has_value()) {
      whitespace_part = "[ \\n\\t]*";
    } else {
      whitespace_part = "[ \\n\\t]{0," + std::to_string(max_whitespace_cnt_.value()) + "}";
    }
    return whitespace_part + " \"" + separator_ + "\" " + whitespace_part;
  }
  if (!enable_newline_) {
    return "\"" + separator_ + "\"";
  }
  return "\"" + separator_ + "\\n" + std::string(total_indent_, ' ') + "\"";
}

std::string IndentManager::EndSeparator() {
  if (any_whitespace_) {
    if (!max_whitespace_cnt_.has_value()) {
      return "[ \\n\\t]*";
    } else {
      return "[ \\n\\t]{0," + std::to_string(max_whitespace_cnt_.value()) + "}";
    }
  }
  if (!enable_newline_) {
    return "\"\"";
  }
  return "\"\\n" + std::string(total_indent_ - indent_, ' ') + "\"";
}

std::string IndentManager::EmptySeparator() {
  if (any_whitespace_) {
    if (!max_whitespace_cnt_.has_value()) {
      return "[ \\n\\t]*";
    } else {
      return "[ \\n\\t]{0," + std::to_string(max_whitespace_cnt_.value()) + "}";
    }
  }
  return "\"\"";
}

std::string IndentManager::NextSeparator(bool is_end) {
  if (any_whitespace_) {
    if (is_first_.back() || is_end) {
      is_first_.back() = false;
      if (!max_whitespace_cnt_.has_value()) {
        return "[ \\n\\t]*";
      } else {
        return "[ \\n\\t]{0," + std::to_string(max_whitespace_cnt_.value()) + "}";
      }
    } else {
      std::string whitespace_part;
      if (!max_whitespace_cnt_.has_value()) {
        whitespace_part = "[ \\n\\t]*";
      } else {
        whitespace_part = "[ \\n\\t]{0," + std::to_string(max_whitespace_cnt_.value()) + "}";
      }
      return whitespace_part + " \"" + separator_ + "\" " + whitespace_part;
    }
  }

  std::string res = "";
  if (!is_first_.back() && !is_end) {
    res += separator_;
  }
  is_first_.back() = false;

  if (enable_newline_) {
    res += "\\n";
  }

  if (!is_end) {
    res += std::string(total_indent_, ' ');
  } else {
    res += std::string(total_indent_ - indent_, ' ');
  }

  return "\"" + res + "\"";
}

// ==================== Static Constants ====================

const std::string JSONSchemaConverter::kBasicAny = "basic_any";
const std::string JSONSchemaConverter::kBasicInteger = "basic_integer";
const std::string JSONSchemaConverter::kBasicNumber = "basic_number";
const std::string JSONSchemaConverter::kBasicString = "basic_string";
const std::string JSONSchemaConverter::kBasicBoolean = "basic_boolean";
const std::string JSONSchemaConverter::kBasicNull = "basic_null";
const std::string JSONSchemaConverter::kBasicArray = "basic_array";
const std::string JSONSchemaConverter::kBasicObject = "basic_object";
const std::string JSONSchemaConverter::kBasicEscape = "basic_escape";
const std::string JSONSchemaConverter::kBasicStringSub = "basic_string_sub";

// ==================== JSONSchemaConverter Implementation ====================

JSONSchemaConverter::JSONSchemaConverter(
    std::optional<int> indent,
    std::optional<std::pair<std::string, std::string>> separators,
    bool any_whitespace,
    std::optional<int> max_whitespace_cnt,
    RefResolver ref_resolver
)
    : indent_manager_(
          indent,
          separators.has_value() ? separators->first
                                 : (any_whitespace ? "," : (indent.has_value() ? "," : ", ")),
          any_whitespace,
          max_whitespace_cnt
      ),
      any_whitespace_(any_whitespace),
      max_whitespace_cnt_(max_whitespace_cnt),
      ref_resolver_(std::move(ref_resolver)) {
  std::string colon_sep =
      separators.has_value() ? separators->second : (any_whitespace ? ":" : ": ");
  if (any_whitespace) {
    std::string whitespace_part;
    if (!max_whitespace_cnt_.has_value()) {
      whitespace_part = "[ \\n\\t]*";
    } else {
      whitespace_part = "[ \\n\\t]{0," + std::to_string(max_whitespace_cnt_.value()) + "}";
    }
    colon_pattern_ = whitespace_part + " \"" + colon_sep + "\" " + whitespace_part;
  } else {
    colon_pattern_ = "\"" + colon_sep + "\"";
  }
}

std::string JSONSchemaConverter::Convert(const SchemaSpecPtr& spec) {
  AddBasicRules();

  // Register the root rule for circular reference handling
  // This allows $ref: "#" to resolve to "root"
  std::string root_rule_name = ebnf_script_creator_.AllocateRuleName("root");
  uri_to_rule_name_["#"] = root_rule_name;

  // Check if the spec can be directly mapped to an existing rule
  auto cached_rule = GetCache(spec->cache_key);
  if (cached_rule.has_value()) {
    // Root schema matches a basic type, just reference it
    ebnf_script_creator_.AddRuleWithAllocatedName(root_rule_name, cached_rule.value());
  } else {
    // Generate the rule body
    if (!spec->cache_key.empty()) {
      AddCache(spec->cache_key, root_rule_name);
    }
    std::string root_body = GenerateFromSpec(spec, root_rule_name);
    ebnf_script_creator_.AddRuleWithAllocatedName(root_rule_name, root_body);
  }

  return ebnf_script_creator_.GetScript();
}

void JSONSchemaConverter::AddBasicRules() {
  AddHelperRules();

  // Create basic rules with a temporary indent manager for compact format
  auto saved_indent_manager = indent_manager_;
  if (any_whitespace_) {
    indent_manager_ = IndentManager(std::nullopt, ",", true, std::nullopt);
  } else {
    indent_manager_ = IndentManager(std::nullopt, ", ", false, std::nullopt);
  }

  // basic_any - use "{}" as the cache key for empty schema
  auto any_spec = SchemaSpec::Make(AnySpec{}, "{}", kBasicAny);
  std::string any_body = GenerateAny(std::get<AnySpec>(any_spec->spec), kBasicAny);
  ebnf_script_creator_.AddRule(kBasicAny, any_body);
  AddCache("{}", kBasicAny);

  // basic_integer - cache_key matches SchemaParser::ComputeCacheKey for {"type": "integer"}
  constexpr const char* kIntegerCacheKey = "{\"type\":\"integer\"}";
  auto int_spec = SchemaSpec::Make(IntegerSpec{}, kIntegerCacheKey, kBasicInteger);
  std::string int_body = GenerateInteger(std::get<IntegerSpec>(int_spec->spec), kBasicInteger);
  ebnf_script_creator_.AddRule(kBasicInteger, int_body);
  AddCache(kIntegerCacheKey, kBasicInteger);

  // basic_number - cache_key matches SchemaParser::ComputeCacheKey for {"type": "number"}
  constexpr const char* kNumberCacheKey = "{\"type\":\"number\"}";
  auto num_spec = SchemaSpec::Make(NumberSpec{}, kNumberCacheKey, kBasicNumber);
  std::string num_body = GenerateNumber(std::get<NumberSpec>(num_spec->spec), kBasicNumber);
  ebnf_script_creator_.AddRule(kBasicNumber, num_body);
  AddCache(kNumberCacheKey, kBasicNumber);

  // basic_string - cache_key matches SchemaParser::ComputeCacheKey for {"type": "string"}
  constexpr const char* kStringCacheKey = "{\"type\":\"string\"}";
  auto str_spec = SchemaSpec::Make(StringSpec{}, kStringCacheKey, kBasicString);
  std::string str_body = "[\"] " + kBasicStringSub;
  ebnf_script_creator_.AddRule(kBasicString, str_body);
  AddCache(kStringCacheKey, kBasicString);

  // basic_boolean - cache_key matches SchemaParser::ComputeCacheKey for {"type": "boolean"}
  constexpr const char* kBooleanCacheKey = "{\"type\":\"boolean\"}";
  auto bool_spec = SchemaSpec::Make(BooleanSpec{}, kBooleanCacheKey, kBasicBoolean);
  std::string bool_body = GenerateBoolean(std::get<BooleanSpec>(bool_spec->spec), kBasicBoolean);
  ebnf_script_creator_.AddRule(kBasicBoolean, bool_body);
  AddCache(kBooleanCacheKey, kBasicBoolean);

  // basic_null - cache_key matches SchemaParser::ComputeCacheKey for {"type": "null"}
  constexpr const char* kNullCacheKey = "{\"type\":\"null\"}";
  auto null_spec = SchemaSpec::Make(NullSpec{}, kNullCacheKey, kBasicNull);
  std::string null_body = GenerateNull(std::get<NullSpec>(null_spec->spec), kBasicNull);
  ebnf_script_creator_.AddRule(kBasicNull, null_body);
  AddCache(kNullCacheKey, kBasicNull);

  // basic_array - cache_key matches SchemaParser::ComputeCacheKey for {"type": "array"}
  constexpr const char* kArrayCacheKey = "{\"type\":\"array\"}";
  ArraySpec array_spec_val;
  array_spec_val.allow_additional_items = true;
  array_spec_val.additional_items = any_spec;
  auto array_spec = SchemaSpec::Make(std::move(array_spec_val), kArrayCacheKey, kBasicArray);
  std::string array_body = GenerateArray(std::get<ArraySpec>(array_spec->spec), kBasicArray);
  ebnf_script_creator_.AddRule(kBasicArray, array_body);
  AddCache(kArrayCacheKey, kBasicArray);

  // basic_object - cache_key matches SchemaParser::ComputeCacheKey for {"type": "object"}
  constexpr const char* kObjectCacheKey = "{\"type\":\"object\"}";
  ObjectSpec obj_spec_val;
  obj_spec_val.allow_additional_properties = true;
  obj_spec_val.additional_properties_schema = any_spec;
  auto obj_spec = SchemaSpec::Make(std::move(obj_spec_val), kObjectCacheKey, kBasicObject);
  std::string obj_body = GenerateObject(std::get<ObjectSpec>(obj_spec->spec), kBasicObject);
  ebnf_script_creator_.AddRule(kBasicObject, obj_body);
  AddCache(kObjectCacheKey, kBasicObject);

  indent_manager_ = saved_indent_manager;
}

void JSONSchemaConverter::AddHelperRules() {
  ebnf_script_creator_.AddRule(
      kBasicEscape, "[\"\\\\/bfnrt] | \"u\" [A-Fa-f0-9] [A-Fa-f0-9] [A-Fa-f0-9] [A-Fa-f0-9]"
  );
  std::string whitespace_part = GetWhitespacePattern();
  ebnf_script_creator_.AddRule(
      kBasicStringSub,
      "(\"\\\"\" | [^\\0-\\x1f\\\"\\\\\\r\\n] " + kBasicStringSub + " | \"\\\\\" " + kBasicEscape +
          " " + kBasicStringSub + ") (= " + whitespace_part + " [,}\\]:])"
  );
}

std::string JSONSchemaConverter::GetWhitespacePattern() const {
  if (!max_whitespace_cnt_.has_value()) {
    return "[ \\n\\t]*";
  } else {
    return "[ \\n\\t]{0," + std::to_string(max_whitespace_cnt_.value()) + "}";
  }
}

std::string JSONSchemaConverter::NextSeparator(bool is_end) {
  return indent_manager_.NextSeparator(is_end);
}

std::string JSONSchemaConverter::GetKeyPattern() const { return kBasicString; }

std::string JSONSchemaConverter::GetBasicAnyRuleName() const { return kBasicAny; }

void JSONSchemaConverter::AddCache(const std::string& key, const std::string& value) {
  if (key.empty()) {
    return;
  }
  rule_cache_manager_.AddCache(key, GetCacheContext(), value);
}

std::optional<std::string> JSONSchemaConverter::GetCache(const std::string& key) const {
  if (key.empty()) {
    return std::nullopt;
  }
  return rule_cache_manager_.GetCache(key, GetCacheContext());
}

int64_t JSONSchemaConverter::GetCacheContext() const {
  if (any_whitespace_ || !indent_manager_.enable_newline_) {
    return 0;
  }
  return indent_manager_.total_indent_;
}

std::string JSONSchemaConverter::CreateRule(
    const SchemaSpecPtr& spec, const std::string& rule_name_hint
) {
  auto cached = GetCache(spec->cache_key);
  if (cached.has_value()) {
    return cached.value();
  }

  std::string rule_name = ebnf_script_creator_.AllocateRuleName(rule_name_hint);
  if (!spec->cache_key.empty()) {
    AddCache(spec->cache_key, rule_name);
  }
  std::string rule_body = GenerateFromSpec(spec, rule_name);
  ebnf_script_creator_.AddRuleWithAllocatedName(rule_name, rule_body);

  return rule_name;
}

std::string JSONSchemaConverter::CreateGeneratedRule(
    const std::string& rule_name_hint, const std::string& rule_body
) {
  auto it = generated_rule_body_cache_.find(rule_body);
  if (it != generated_rule_body_cache_.end()) {
    return it->second;
  }

  std::string rule_name = ebnf_script_creator_.AllocateRuleName(rule_name_hint);
  ebnf_script_creator_.AddRuleWithAllocatedName(rule_name, rule_body);
  generated_rule_body_cache_.emplace(rule_body, rule_name);
  return rule_name;
}

std::string JSONSchemaConverter::GenerateFromSpec(
    const SchemaSpecPtr& spec, const std::string& rule_name_hint
) {
  return std::visit(
      [this, &rule_name_hint](const auto& s) -> std::string {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, IntegerSpec>) {
          return GenerateInteger(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, NumberSpec>) {
          return GenerateNumber(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, StringSpec>) {
          return GenerateString(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, BooleanSpec>) {
          return GenerateBoolean(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, NullSpec>) {
          return GenerateNull(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, ArraySpec>) {
          return GenerateArray(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, ObjectSpec>) {
          return GenerateObject(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, AnySpec>) {
          return GenerateAny(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, ConstSpec>) {
          return GenerateConst(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, EnumSpec>) {
          return GenerateEnum(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, RefSpec>) {
          return GenerateRef(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, AnyOfSpec>) {
          return GenerateAnyOf(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, OneOfSpec>) {
          return GenerateOneOf(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, AllOfSpec>) {
          return GenerateAllOf(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, TypeArraySpec>) {
          return GenerateTypeArray(s, rule_name_hint);
        } else {
          XGRAMMAR_LOG(FATAL) << "Unknown spec type";
          return "";
        }
      },
      spec->spec
  );
}

// ==================== Generate Methods ====================

std::string JSONSchemaConverter::GenerateInteger(
    const IntegerSpec& spec, const std::string& rule_name
) {
  std::optional<int64_t> start, end;
  if (spec.minimum.has_value()) {
    start = spec.minimum;
  }
  if (spec.exclusive_minimum.has_value()) {
    start = *spec.exclusive_minimum + 1;
  }
  if (spec.maximum.has_value()) {
    end = spec.maximum;
  }
  if (spec.exclusive_maximum.has_value()) {
    end = *spec.exclusive_maximum - 1;
  }

  if (start.has_value() || end.has_value()) {
    std::string range_regex = GenerateRangeRegex(start, end);
    return RegexToEBNF(range_regex, false);
  }
  return "(\"0\" | \"-\"? [1-9] [0-9]*)";
}

std::string JSONSchemaConverter::GenerateNumber(
    const NumberSpec& spec, const std::string& rule_name
) {
  std::optional<double> start, end;
  if (spec.minimum.has_value()) {
    start = spec.minimum;
  }
  if (spec.exclusive_minimum.has_value()) {
    start = spec.exclusive_minimum;
  }
  if (spec.maximum.has_value()) {
    end = spec.maximum;
  }
  if (spec.exclusive_maximum.has_value()) {
    end = spec.exclusive_maximum;
  }

  if (start.has_value() || end.has_value()) {
    std::string range_regex = GenerateFloatRangeRegex(start, end, 6);
    return RegexToEBNF(range_regex, false);
  }
  // Note: The format must be "-"? ("0" | ...) not ("0" | "-"? ...)
  // The first allows -0, -123, 0, 123
  // The second allows 0, -123, 123 but not -0
  return "\"-\"? (\"0\" | [1-9] [0-9]*) (\".\" [0-9]+)? ([eE] [+-]? [0-9]+)?";
}

std::string JSONSchemaConverter::GenerateString(
    const StringSpec& spec, const std::string& rule_name
) {
  // Check for format
  if (spec.format.has_value()) {
    const std::string& format = *spec.format;
    auto regex_pattern = JSONFormatToRegexPattern(format);

    if (regex_pattern.has_value()) {
      std::string converted_regex = RegexToEBNF(regex_pattern.value(), false);
      return "\"\\\"\" " + converted_regex + " \"\\\"\"";
    }
  }

  // Check for pattern
  if (spec.pattern.has_value()) {
    std::string converted_regex = RegexToEBNF(*spec.pattern, false);
    return "\"\\\"\" " + converted_regex + " \"\\\"\"";
  }

  // Check for length constraints
  if (spec.min_length != 0 || spec.max_length != -1) {
    std::string char_pattern = "[^\"\\\\\\r\\n]";
    std::string repetition;
    if (spec.max_length == -1) {
      repetition = "{" + std::to_string(spec.min_length) + ",}";
    } else {
      repetition =
          "{" + std::to_string(spec.min_length) + "," + std::to_string(spec.max_length) + "}";
    }
    return "\"\\\"\" " + char_pattern + repetition + " \"\\\"\"";
  }

  // Default string
  return "[\"] " + kBasicStringSub;
}

std::string JSONSchemaConverter::GenerateBoolean(
    const BooleanSpec& spec, const std::string& rule_name
) {
  return "\"true\" | \"false\"";
}

std::string JSONSchemaConverter::GenerateNull(const NullSpec& spec, const std::string& rule_name) {
  return "\"null\"";
}

std::string JSONSchemaConverter::GenerateArray(
    const ArraySpec& spec, const std::string& rule_name
) {
  indent_manager_.StartIndent();

  auto start_separator = indent_manager_.StartSeparator();
  auto mid_separator = indent_manager_.MiddleSeparator();
  auto end_separator = indent_manager_.EndSeparator();
  auto empty_separator = indent_manager_.EmptySeparator();

  std::vector<std::string> item_rule_names;
  std::string additional_rule_name;

  // Handle prefix items
  for (size_t i = 0; i < spec.prefix_items.size(); ++i) {
    item_rule_names.push_back(
        CreateRule(spec.prefix_items[i], rule_name + "_item_" + std::to_string(i))
    );
  }

  // Handle additional items
  if (spec.allow_additional_items && spec.additional_items) {
    additional_rule_name = CreateRule(spec.additional_items, rule_name + "_additional");
  }

  indent_manager_.EndIndent();

  // Construct the result
  const std::string& left_bracket = EBNFScriptCreator::Str("[");
  const std::string& right_bracket = EBNFScriptCreator::Str("]");

  if (spec.min_contains > 0) {
    XGRAMMAR_CHECK(spec.prefix_items.empty() && spec.allow_additional_items && !additional_rule_name.empty())
        << "contains is only supported for homogeneous arrays with additional items";
    std::string contains_rule_name = CreateRule(spec.contains_item, rule_name + "_contains");
    int64_t sequence_min_items = std::max<int64_t>(1, spec.min_items);
    int64_t sequence_max_items = spec.max_items;

    auto repeat_tail = [&](int64_t min_repeats, int64_t max_repeats) {
      std::string tail_pattern = EBNFScriptCreator::Concat({mid_separator, additional_rule_name});
      return EBNFScriptCreator::Repeat(
          tail_pattern,
          static_cast<int>(min_repeats),
          max_repeats == -1 ? -1 : static_cast<int>(max_repeats)
      );
    };

    std::unordered_map<std::string, std::string> contains_sequence_cache;
    std::function<std::string(int64_t, int64_t)> build_contains_sequence =
        [&](int64_t min_items, int64_t max_items) -> std::string {
      std::string cache_key =
          std::to_string(min_items) + ":" + (max_items == -1 ? "*" : std::to_string(max_items));
      auto it = contains_sequence_cache.find(cache_key);
      if (it != contains_sequence_cache.end()) {
        return it->second;
      }

      std::string sequence_rule_name =
          ebnf_script_creator_.AllocateRuleName(rule_name + "_contains_seq");
      contains_sequence_cache.emplace(cache_key, sequence_rule_name);

      std::vector<std::string> options;
      int64_t min_tail_items = std::max<int64_t>(0, min_items - 1);
      int64_t max_tail_items = max_items == -1 ? -1 : max_items - 1;
      if (max_tail_items == -1 || min_tail_items <= max_tail_items) {
        options.push_back(contains_rule_name + " " + repeat_tail(min_tail_items, max_tail_items));
      }

      if (max_items == -1 || max_items > 1) {
        int64_t next_min_items = std::max<int64_t>(1, min_items - 1);
        int64_t next_max_items = max_items == -1 ? -1 : max_items - 1;
        if (next_max_items == -1 || next_min_items <= next_max_items) {
          std::string next_sequence =
              build_contains_sequence(next_min_items, next_max_items);
          options.push_back(additional_rule_name + " " + mid_separator + " " + next_sequence);
        }
      }

      XGRAMMAR_CHECK(!options.empty()) << "contains array sequence must have at least one option";
      std::string sequence_rule_body =
          options.size() == 1 ? options[0] : EBNFScriptCreator::Or(options);
      ebnf_script_creator_.AddRuleWithAllocatedName(sequence_rule_name, sequence_rule_body);
      return sequence_rule_name;
    };

    std::string sequence_rule =
        build_contains_sequence(sequence_min_items, sequence_max_items);
    return EBNFScriptCreator::Concat(
        {left_bracket, start_separator, sequence_rule, end_separator, right_bracket}
    );
  }

  if (spec.prefix_items.empty()) {
    auto empty_part = EBNFScriptCreator::Concat({left_bracket, empty_separator, right_bracket});
    if (!spec.allow_additional_items) {
      return empty_part;
    } else if (spec.min_items == 0 && spec.max_items == 0) {
      return empty_part;
    } else if (spec.min_items == 0 && spec.max_items != 0) {
      return EBNFScriptCreator::Or(
          {EBNFScriptCreator::Concat(
               {left_bracket,
                start_separator,
                additional_rule_name,
                EBNFScriptCreator::Repeat(
                    EBNFScriptCreator::Concat({mid_separator, additional_rule_name}),
                    0,
                    spec.max_items == -1 ? -1 : static_cast<int>(spec.max_items - 1)
                ),
                end_separator,
                right_bracket}
           ),
           empty_part}
      );
    } else {
      return EBNFScriptCreator::Concat(
          {left_bracket,
           start_separator,
           additional_rule_name,
           EBNFScriptCreator::Repeat(
               EBNFScriptCreator::Concat({mid_separator, additional_rule_name}),
               static_cast<int>(spec.min_items - 1),
               spec.max_items == -1 ? -1 : static_cast<int>(spec.max_items - 1)
           ),
           end_separator,
           right_bracket}
      );
    }
  } else {
    std::vector<std::string> prefix_part;
    for (size_t i = 0; i < item_rule_names.size(); ++i) {
      if (i > 0) {
        prefix_part.push_back(mid_separator);
      }
      prefix_part.push_back(item_rule_names[i]);
    }
    auto prefix_part_str = EBNFScriptCreator::Concat(prefix_part);
    if (!spec.allow_additional_items) {
      return EBNFScriptCreator::Concat(
          {left_bracket, start_separator, prefix_part_str, end_separator, right_bracket}
      );
    } else {
      int64_t min_items = std::max(
          static_cast<int64_t>(0), spec.min_items - static_cast<int64_t>(item_rule_names.size())
      );
      return EBNFScriptCreator::Concat(
          {left_bracket,
           start_separator,
           prefix_part_str,
           EBNFScriptCreator::Repeat(
               EBNFScriptCreator::Concat({mid_separator, additional_rule_name}),
               static_cast<int>(min_items),
               spec.max_items == -1
                   ? -1
                   : static_cast<int>(spec.max_items - static_cast<int64_t>(item_rule_names.size()))
           ),
           end_separator,
           right_bracket}
      );
    }
  }
}

std::string JSONSchemaConverter::FormatPropertyKey(const std::string& key) {
  return "\"\\\"" + key + "\\\"\"";
}

std::string JSONSchemaConverter::FormatProperty(
    const std::string& key, const std::string& value_rule, const std::string& rule_name, int64_t idx
) {
  return FormatPropertyKey(key) + " " + colon_pattern_ + " " + value_rule;
}

std::string JSONSchemaConverter::FormatOtherProperty(
    const std::string& key_pattern,
    const std::string& value_rule,
    const std::string& rule_name,
    const std::string& rule_name_suffix
) {
  return key_pattern + " " + colon_pattern_ + " " + value_rule;
}

std::string JSONSchemaConverter::GetPropertyWithNumberConstraints(
    const std::string& pattern, int min_properties, int max_properties, int already_repeated_times
) {
  if (max_properties != -1 && max_properties == already_repeated_times) {
    return "\"\"";
  }
  int lower = std::max(0, min_properties - already_repeated_times);
  int upper = max_properties == -1 ? -1 : std::max(-1, max_properties - already_repeated_times);
  if (lower == 0 && upper == -1) {
    return "(" + pattern + ")*";
  } else if (lower == 0 && upper == 1) {
    return "(" + pattern + ")?";
  } else if (lower == 1 && upper == 1) {
    return pattern;
  } else {
    return "(" + pattern + "){" + std::to_string(lower) + "," +
           (upper == -1 ? "" : std::to_string(upper)) + "} ";
  }
}

std::string JSONSchemaConverter::GetPartialRuleForProperties(
    const std::vector<ObjectSpec::Property>& properties,
    const std::unordered_set<std::string>& required,
    const SchemaSpecPtr& additional,
    const std::string& rule_name,
    const std::string& additional_suffix,
    int min_properties,
    int max_properties
) {
  if (max_properties == 0) {
    return "";
  }

  std::string first_sep = NextSeparator();
  std::string mid_sep = NextSeparator();
  std::string last_sep = NextSeparator(true);

  std::string res = "";

  std::vector<std::string> prop_patterns;
  for (size_t idx = 0; idx < properties.size(); ++idx) {
    const auto& prop = properties[idx];
    std::string value_rule = CreateRule(prop.schema, rule_name + "_prop_" + std::to_string(idx));
    prop_patterns.push_back(FormatProperty(prop.name, value_rule, rule_name, idx));
  }

  if (min_properties == 0 && max_properties == -1) {
    // Case 1: No property number constraints
    std::vector<std::string> rule_names(properties.size(), "");
    std::vector<uint8_t> is_required(properties.size(), false);
    bool allow_additional = additional != nullptr;

    // Construct the last rule
    std::string additional_prop_pattern;
    if (allow_additional) {
      std::string add_value_rule = CreateRule(additional, rule_name + "_" + additional_suffix);
      additional_prop_pattern =
          FormatOtherProperty(GetKeyPattern(), add_value_rule, rule_name, additional_suffix);
      std::string last_rule_body = "(" + mid_sep + " " + additional_prop_pattern + ")*";
      std::string last_rule_name =
          rule_name + "_part_" + std::to_string(static_cast<int>(properties.size()) - 1);
      last_rule_name = CreateGeneratedRule(last_rule_name, last_rule_body);
      rule_names.back() = last_rule_name;
    } else {
      rule_names.back() = "\"\"";
    }

    // Construct 0~(len(properties) - 2) rules
    for (int i = static_cast<int>(properties.size()) - 2; i >= 0; --i) {
      const std::string& prop_pattern = prop_patterns[i + 1];
      const std::string& last_rule_name = rule_names[i + 1];
      std::string cur_rule_body = mid_sep + " " + prop_pattern + " " + last_rule_name;
      if (!required.count(properties[i + 1].name)) {
        cur_rule_body = last_rule_name + " | " + cur_rule_body;
      } else {
        is_required[i + 1] = true;
      }
      std::string cur_rule_name = rule_name + "_part_" + std::to_string(i);
      cur_rule_name = CreateGeneratedRule(cur_rule_name, cur_rule_body);
      rule_names[i] = cur_rule_name;
    }
    if (required.count(properties[0].name)) {
      is_required[0] = true;
    }

    // Construct the root rule
    for (size_t i = 0; i < properties.size(); ++i) {
      if (i != 0) {
        res += " | ";
      }
      res += "(" + prop_patterns[i] + " " + rule_names[i] + ")";
      if (is_required[i]) {
        break;
      }
    }

    if (allow_additional && required.empty()) {
      res += " | " + additional_prop_pattern + " " + rule_names.back();
    }

    res = first_sep + " (" + res + ") " + last_sep;
  } else if (max_properties == -1) {
    // Case 2: With constraint on the lower bound of the properties number
    const int properties_size = static_cast<int>(properties.size());
    std::vector<std::vector<std::string>> rule_names(properties_size, std::vector<std::string>());
    std::vector<int> key_matched_min(properties_size, 0);
    std::vector<uint8_t> is_required(properties_size, false);
    bool allow_additional = additional != nullptr;

    std::string additional_prop_pattern;
    if (allow_additional) {
      std::string add_value_rule = CreateRule(additional, rule_name + "_" + additional_suffix);
      additional_prop_pattern =
          FormatOtherProperty(GetKeyPattern(), add_value_rule, rule_name, additional_suffix);
    }

    // Get the range of matched properties for each rule
    bool get_first_required = required.count(properties[0].name);
    key_matched_min[0] = 1;
    for (int i = 1; i < properties_size; ++i) {
      if (required.count(properties[i].name)) {
        is_required[i] = true;
        key_matched_min[i] = key_matched_min[i - 1] + 1;
      } else {
        key_matched_min[i] = key_matched_min[i - 1];
      }
      if (!get_first_required) {
        key_matched_min[i] = 1;
      }
      if (is_required[i]) {
        get_first_required = true;
      }
    }
    if (required.count(properties[0].name)) {
      is_required[0] = true;
    }
    if (allow_additional) {
      key_matched_min.back() = std::max(1, key_matched_min.back());
    } else {
      key_matched_min.back() = std::max(min_properties, key_matched_min.back());
    }
    for (int i = properties_size - 2; i >= 0; --i) {
      key_matched_min[i] = std::max(key_matched_min[i], key_matched_min[i + 1] - 1);
    }

    // Construct the last rule
    if (allow_additional) {
      for (int matched = key_matched_min.back(); matched <= properties_size; ++matched) {
        std::string last_rule_body = GetPropertyWithNumberConstraints(
            mid_sep + " " + additional_prop_pattern, min_properties, max_properties, matched
        );
        std::string last_rule_name = rule_name + "_part_" + std::to_string(properties_size - 1) +
                                     "_" + std::to_string(matched);
        last_rule_name = CreateGeneratedRule(last_rule_name, last_rule_body);
        rule_names.back().push_back(last_rule_name);
      }
    } else {
      for (int matched = key_matched_min.back(); matched <= properties_size; ++matched) {
        rule_names.back().push_back("\"\"");
      }
    }

    // Construct 0~(len(properties) - 2) rules
    for (int i = properties_size - 2; i >= 0; --i) {
      const std::string& prop_pattern = prop_patterns[i + 1];
      for (int matched = key_matched_min[i]; matched <= i + 1; ++matched) {
        std::string cur_rule_body;
        if (is_required[i + 1] || matched == key_matched_min[i + 1] - 1) {
          cur_rule_body = mid_sep + " " + prop_pattern + " " +
                          rule_names[i + 1][matched + 1 - key_matched_min[i + 1]];
        } else {
          cur_rule_body = rule_names[i + 1][matched - key_matched_min[i + 1]] + " | " + mid_sep +
                          " " + prop_pattern + " " +
                          rule_names[i + 1][matched - key_matched_min[i + 1] + 1];
        }
        std::string cur_rule_name =
            rule_name + "_part_" + std::to_string(i) + "_" + std::to_string(matched);
        cur_rule_name = CreateGeneratedRule(cur_rule_name, cur_rule_body);
        rule_names[i].push_back(cur_rule_name);
      }
    }

    // Construct root rule
    bool is_first = true;
    for (int i = 0; i < properties_size; ++i) {
      if (key_matched_min[i] > 1) {
        break;
      }
      if (!is_first) {
        res += " | ";
      } else {
        is_first = false;
      }
      res += "(" + prop_patterns[i] + " " + rule_names[i][1 - key_matched_min[i]] + ")";
      if (is_required[i]) {
        break;
      }
    }

    if (allow_additional && required.empty()) {
      if (!is_first) {
        res += " | ";
      }
      res += "(" + additional_prop_pattern + " " +
             GetPropertyWithNumberConstraints(
                 mid_sep + " " + additional_prop_pattern, min_properties, max_properties, 1
             ) +
             ")";
    }

    res = first_sep + " (" + res + ") " + last_sep;
  } else {
    // Case 3: With constraints on both lower & upper bound of the properties number
    const int properties_size = static_cast<int>(properties.size());
    std::vector<std::vector<std::string>> rule_names(properties_size, std::vector<std::string>());
    std::vector<int> key_matched_min(properties_size, 0);
    std::vector<int> key_matched_max(properties_size, properties_size);
    std::vector<uint8_t> is_required(properties_size, false);
    bool allow_additional = additional != nullptr;

    std::string additional_prop_pattern;
    if (allow_additional) {
      std::string add_value_rule = CreateRule(additional, rule_name + "_" + additional_suffix);
      additional_prop_pattern =
          FormatOtherProperty(GetKeyPattern(), add_value_rule, rule_name, additional_suffix);
    }

    // Get the range of matched properties for each rule
    bool get_first_required = required.count(properties[0].name);
    key_matched_min[0] = 1;
    key_matched_max[0] = 1;
    for (int i = 1; i < properties_size; ++i) {
      if (required.count(properties[i].name)) {
        is_required[i] = true;
        key_matched_min[i] = key_matched_min[i - 1] + 1;
      } else {
        key_matched_min[i] = key_matched_min[i - 1];
      }
      if (!get_first_required) {
        key_matched_min[i] = 1;
      }
      key_matched_max[i] = key_matched_max[i - 1] + 1;
      if (is_required[i]) {
        get_first_required = true;
      }
    }
    if (required.count(properties[0].name)) {
      is_required[0] = true;
    }
    if (allow_additional) {
      key_matched_min.back() = std::max(1, key_matched_min.back());
      key_matched_max.back() = std::min(max_properties, key_matched_max.back());
    } else {
      key_matched_min.back() = std::max(min_properties, key_matched_min.back());
      key_matched_max.back() = std::min(max_properties, key_matched_max.back());
    }
    for (int i = properties_size - 2; i >= 0; --i) {
      key_matched_min[i] = std::max(key_matched_min[i], key_matched_min[i + 1] - 1);
      if (is_required[i + 1]) {
        key_matched_max[i] = std::min(key_matched_max[i], key_matched_max[i + 1] - 1);
      } else {
        key_matched_max[i] = std::min(key_matched_max[i], key_matched_max[i + 1]);
      }
    }

    // Construct the last rule
    if (allow_additional) {
      for (int matched = key_matched_min.back(); matched <= key_matched_max.back(); ++matched) {
        std::string last_rule_body = GetPropertyWithNumberConstraints(
            mid_sep + " " + additional_prop_pattern, min_properties, max_properties, matched
        );
        std::string last_rule_name = rule_name + "_part_" + std::to_string(properties_size - 1) +
                                     "_" + std::to_string(matched);
        last_rule_name = CreateGeneratedRule(last_rule_name, last_rule_body);
        rule_names.back().push_back(last_rule_name);
      }
    } else {
      for (int matched = key_matched_min.back(); matched <= key_matched_max.back(); ++matched) {
        rule_names.back().push_back("\"\"");
      }
    }

    // Construct 0~(len(properties) - 2) rules
    for (int i = properties_size - 2; i >= 0; --i) {
      const std::string& prop_pattern = prop_patterns[i + 1];
      for (int matched = key_matched_min[i]; matched <= key_matched_max[i]; ++matched) {
        std::string cur_rule_body;
        if (matched == key_matched_max[i + 1]) {
          cur_rule_body = rule_names[i + 1][matched - key_matched_min[i + 1]];
        } else if (is_required[i + 1] || matched == key_matched_min[i + 1] - 1) {
          cur_rule_body = mid_sep + " " + prop_pattern + " " +
                          rule_names[i + 1][matched + 1 - key_matched_min[i + 1]];
        } else {
          cur_rule_body = rule_names[i + 1][matched - key_matched_min[i + 1]] + " | " + mid_sep +
                          " " + prop_pattern + " " +
                          rule_names[i + 1][matched - key_matched_min[i + 1] + 1];
        }
        std::string cur_rule_name =
            rule_name + "_part_" + std::to_string(i) + "_" + std::to_string(matched);
        cur_rule_name = CreateGeneratedRule(cur_rule_name, cur_rule_body);
        rule_names[i].push_back(cur_rule_name);
      }
    }

    // Construct root rule
    bool is_first = true;
    for (int i = 0; i < properties_size; ++i) {
      if (key_matched_max[i] < key_matched_min[i]) {
        continue;
      }
      if (key_matched_min[i] > 1) {
        break;
      }
      if (!is_first) {
        res += " | ";
      } else {
        is_first = false;
      }
      res += "(" + prop_patterns[i] + " " + rule_names[i][1 - key_matched_min[i]] + ")";
      if (is_required[i]) {
        break;
      }
    }

    if (allow_additional && required.empty()) {
      if (!is_first) {
        res += " | ";
      }
      res += "(" + additional_prop_pattern + " " +
             GetPropertyWithNumberConstraints(
                 mid_sep + " " + additional_prop_pattern, min_properties, max_properties, 1
             ) +
             ")";
    }

    res = first_sep + " (" + res + ") " + last_sep;
  }

  return res;
}

std::string JSONSchemaConverter::GenerateObject(
    const ObjectSpec& spec, const std::string& rule_name, bool need_braces
) {
  std::string result = "";
  if (need_braces) {
    result += "\"{\"";
  }

  bool could_be_empty = false;

  // Determine additional property handling
  std::string additional_suffix = "";
  SchemaSpecPtr additional_property;
  if (spec.allow_additional_properties && spec.additional_properties_schema) {
    additional_suffix = "addl";
    additional_property = spec.additional_properties_schema;
  } else if (spec.allow_unevaluated_properties && spec.unevaluated_properties_schema) {
    additional_suffix = "uneval";
    additional_property = spec.unevaluated_properties_schema;
  } else if (spec.allow_additional_properties || spec.allow_unevaluated_properties) {
    additional_suffix = "addl";
    additional_property = SchemaSpec::Make(AnySpec{}, "", "any");
  }

  indent_manager_.StartIndent();

  if (!spec.pattern_properties.empty() || spec.property_names) {
    // Case 1: patternProperties or propertyNames defined
    std::string beg_seq = NextSeparator();

    std::string property_rule_body = "(";
    if (spec.max_properties != 0) {
      if (!spec.pattern_properties.empty()) {
        for (size_t i = 0; i < spec.pattern_properties.size(); ++i) {
          const auto& pp = spec.pattern_properties[i];
          std::string value = CreateRule(pp.schema, rule_name + "_prop_" + std::to_string(i));
          std::string property_pattern = "\"\\\"\"" + RegexToEBNF(pp.pattern, false) + "\"\\\"\" " +
                                         colon_pattern_ + " " + value;
          if (i != 0) {
            property_rule_body += " | ";
          }
          property_rule_body += "(" + beg_seq + " " + property_pattern + ")";
        }
        property_rule_body += ")";
      } else {
        auto key_pattern = CreateRule(spec.property_names, rule_name + "_name");
        property_rule_body +=
            beg_seq + " " + key_pattern + " " + colon_pattern_ + " " + GetBasicAnyRuleName() + ")";
      }

      auto prop_rule_name = CreateGeneratedRule(rule_name + "_prop", property_rule_body);

      result +=
          " " + prop_rule_name + " " +
          GetPropertyWithNumberConstraints(
              NextSeparator() + " " + prop_rule_name, spec.min_properties, spec.max_properties, 1
          ) +
          NextSeparator(true);
      could_be_empty = spec.min_properties == 0;
    }
  } else if (!spec.properties.empty()) {
    // Case 2: properties defined
    result += " " + GetPartialRuleForProperties(
                        spec.properties,
                        spec.required,
                        additional_property,
                        rule_name,
                        additional_suffix,
                        spec.min_properties,
                        spec.max_properties
                    );
    could_be_empty = spec.required.empty() && spec.min_properties == 0;
  } else if (additional_property) {
    // Case 3: no properties defined, additional properties allowed
    if (spec.max_properties != 0) {
      std::string add_value_rule =
          CreateRule(additional_property, rule_name + "_" + additional_suffix);
      std::string other_property_pattern =
          FormatOtherProperty(GetKeyPattern(), add_value_rule, rule_name, additional_suffix);
      result += " " + NextSeparator() + " " + other_property_pattern + " ";
      result += GetPropertyWithNumberConstraints(
                    NextSeparator() + " " + other_property_pattern,
                    spec.min_properties,
                    spec.max_properties,
                    1
                ) +
                " " + NextSeparator(true);
    }
    could_be_empty = spec.min_properties == 0;
  } else {
    // Case 4: no properties, no additional properties, no pattern properties
    // The object is unconditionally empty.
    could_be_empty = true;
  }

  indent_manager_.EndIndent();

  if (need_braces) {
    result += " \"}\"";
  }
  if (could_be_empty) {
    std::string whitespace_part = GetWhitespacePattern();
    auto rest = need_braces
                    ? "\"{\" " + std::string(any_whitespace_ ? whitespace_part + " " : "") + "\"}\""
                    : std::string(any_whitespace_ ? whitespace_part : "");
    if (result == "\"{\"  \"}\"" || result == "") {
      result = rest;
    } else {
      result = "(" + result + ") | " + rest;
    }
  }

  if (result.empty()) {
    return "\"\"";
  }
  return result;
}

std::string JSONSchemaConverter::GenerateAny(const AnySpec& spec, const std::string& rule_name) {
  return kBasicNumber + " | " + kBasicString + " | " + kBasicBoolean + " | " + kBasicNull + " | " +
         kBasicArray + " | " + kBasicObject;
}

std::string JSONSchemaConverter::GenerateConst(
    const ConstSpec& spec, const std::string& rule_name
) {
  return "\"" + JSONStrToPrintableStr(spec.json_value) + "\"";
}

std::string JSONSchemaConverter::GenerateEnum(const EnumSpec& spec, const std::string& rule_name) {
  std::string result = "";
  for (size_t i = 0; i < spec.json_values.size(); ++i) {
    if (i != 0) {
      result += " | ";
    }
    result += "(\"" + JSONStrToPrintableStr(spec.json_values[i]) + "\")";
  }
  if (result.empty()) {
    return "\"\"";
  }
  return result;
}

std::string JSONSchemaConverter::GenerateRef(const RefSpec& spec, const std::string& rule_name) {
  // First check if we have a direct URI mapping (for circular references)
  if (uri_to_rule_name_.count(spec.uri)) {
    return uri_to_rule_name_[spec.uri];
  }

  if (!ref_resolver_) {
    XGRAMMAR_LOG(FATAL) << "Ref resolver not set; cannot resolve $ref: " << spec.uri;
  }

  // Derive rule name from URI path (like original URIToRule) so that the same
  // $ref always gets the same rule name, and allocate before resolving to prevent
  // dead recursion when the ref target contains a ref back.
  std::string rule_name_hint = "ref";
  if (spec.uri.size() >= 2 && spec.uri[0] == '#' && spec.uri[1] == '/') {
    std::string new_rule_name_prefix;
    std::stringstream ss(spec.uri.substr(2));
    std::string part;
    while (std::getline(ss, part, '/')) {
      if (!part.empty()) {
        if (!new_rule_name_prefix.empty()) {
          new_rule_name_prefix += "_";
        }
        for (char c : part) {
          if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') {
            new_rule_name_prefix += c;
          }
        }
      }
    }
    if (!new_rule_name_prefix.empty()) {
      rule_name_hint = std::move(new_rule_name_prefix);
    }
  }

  std::string allocated_rule_name = ebnf_script_creator_.AllocateRuleName(rule_name_hint);
  uri_to_rule_name_[spec.uri] = allocated_rule_name;

  SchemaSpecPtr resolved = ref_resolver_(spec.uri, allocated_rule_name);
  std::string rule_body = GenerateFromSpec(resolved, allocated_rule_name);
  ebnf_script_creator_.AddRuleWithAllocatedName(allocated_rule_name, rule_body);

  if (!resolved->cache_key.empty()) {
    AddCache(resolved->cache_key, allocated_rule_name);
  }

  return allocated_rule_name;
}

std::string JSONSchemaConverter::GenerateAnyOf(
    const AnyOfSpec& spec, const std::string& rule_name
) {
  std::string result = "";
  for (size_t i = 0; i < spec.options.size(); ++i) {
    if (i != 0) {
      result += " | ";
    }
    result += CreateRule(spec.options[i], rule_name + "_case_" + std::to_string(i));
  }
  return result;
}

std::string JSONSchemaConverter::GenerateOneOf(
    const OneOfSpec&, const std::string& rule_name
) {
  XGRAMMAR_LOG(FATAL) << "OneOfSpec should be normalized before generation for " << rule_name;
  return "";
}

std::string JSONSchemaConverter::GenerateAllOf(
    const AllOfSpec& spec, const std::string& rule_name
) {
  XGRAMMAR_CHECK(spec.schemas.size() == 1)
      << "AllOfSpec should be merged into a single schema before generation";
  return GenerateFromSpec(spec.schemas[0], rule_name + "_case_0");
}

std::string JSONSchemaConverter::GenerateTypeArray(
    const TypeArraySpec& spec, const std::string& rule_name
) {
  std::string result = "";
  for (size_t i = 0; i < spec.type_schemas.size(); ++i) {
    if (i != 0) {
      result += " | ";
    }
    result += CreateRule(spec.type_schemas[i], rule_name + "_type_" + std::to_string(i));
  }
  return result;
}

// ==================== Static Helper Methods ====================

std::optional<std::string> JSONSchemaConverter::JSONFormatToRegexPattern(const std::string& format
) {
  static const auto regex_map = []() -> std::unordered_map<std::string, std::string> {
    std::unordered_map<std::string, std::string> m;

    std::string atext = "[\\w!#$%&'*+/=?^`{|}~-]";
    std::string dot_string = "(" + atext + "+(\\." + atext + "+)*)";
    std::string quoted_string =
        "\\\\\"(\\\\[\\x20-\\x7E]|[\\x20\\x21\\x23-\\x5B\\x5D-\\x7E])*\\\\\"";
    std::string domain =
        "([A-Za-z0-9]([\\-A-Za-z0-9]*[A-Za-z0-9])?)((\\.[A-Za-z0-9][\\-A-Za-z0-9]*[A-Za-z0-9])*"
        ")";
    m["email"] = "^(" + dot_string + "|" + quoted_string + ")@" + domain + "$";

    m["date"] = "^(\\d{4}-(0[1-9]|1[0-2])-(0[1-9]|[1-2]\\d|3[01]))$";
    m["time"] =
        "^([01]\\d|2[0-3]):[0-5]\\d:([0-5]\\d|60)(\\.\\d+)?(Z|[+-]([01]\\d|2[0-3]):[0-5]\\d)$";
    m["date-time"] =
        "^(\\d{4}-(0[1-9]|1[0-2])-(0[1-9]|[1-2]\\d|3[01]))T([01]\\d|2[0-3]):[0-5]\\d:([0-5]\\d|60)("
        "\\.\\d+)?(Z|[+-]([01]\\d|2[0-3]):[0-5]\\d)$";
    m["duration"] =
        "^P((\\d+D|\\d+M(\\d+D)?|\\d+Y(\\d+M(\\d+D)?)?)(T(\\d+S|\\d+M(\\d+S)?|\\d+H(\\d+M(\\d+"
        "S)?"
        ")?))?|T(\\d+S|\\d+M(\\d+S)?|\\d+H(\\d+M(\\d+S)?)?)|\\d+W)$";

    std::string decbyte = "(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)";
    m["ipv4"] = "^(" + decbyte + "\\.){3}" + decbyte + "$";

    m["ipv6"] =
        "("
        "([0-9a-fA-F]{1,4}:){7,7}[0-9a-fA-F]{1,4}|"
        "([0-9a-fA-F]{1,4}:){1,7}:|"
        "([0-9a-fA-F]{1,4}:){1,6}:[0-9a-fA-F]{1,4}|"
        "([0-9a-fA-F]{1,4}:){1,5}(:[0-9a-fA-F]{1,4}){1,2}|"
        "([0-9a-fA-F]{1,4}:){1,4}(:[0-9a-fA-F]{1,4}){1,3}|"
        "([0-9a-fA-F]{1,4}:){1,3}(:[0-9a-fA-F]{1,4}){1,4}|"
        "([0-9a-fA-F]{1,4}:){1,2}(:[0-9a-fA-F]{1,4}){1,5}|"
        "[0-9a-fA-F]{1,4}:((:[0-9a-fA-F]{1,4}){1,6})|"
        ":((:[0-9a-fA-F]{1,4}){1,7}|:)|"
        "::(ffff(:0{1,4}){0,1}:){0,1}"
        "((25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])\\.){3,3}"
        "(25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])|"
        "([0-9a-fA-F]{1,4}:){1,4}:"
        "((25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])\\.){3,3}"
        "(25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])"
        ")";

    m["hostname"] = "^([a-z0-9]([a-z0-9-]*[a-z0-9])?)(\\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)*$";
    m["uuid"] = "^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}$";

    std::string schema_pat = "[a-zA-Z][a-zA-Z+\\.-]*";
    std::string pchar = "([\\w\\.~!$&'()*+,;=:@-]|%[0-9A-Fa-f][0-9A-Fa-f])";
    std::string query_fragment_char = "([\\w\\.~!$&'()*+,;=:@/\\?-]|%[0-9A-Fa-f][0-9A-Fa-f])*";
    std::string query = "(\\?" + query_fragment_char + ")?";
    std::string fragment = "(#" + query_fragment_char + ")?";
    std::string path_abempty = "(/" + pchar + "*)*";
    std::string path_absolute_rootless_empty = "/?(" + pchar + "+(/" + pchar + "*)*)?";
    std::string userinfo = "([\\w\\.~!$&'()*+,;=:-]|%[0-9A-Fa-f][0-9A-Fa-f])*";
    std::string host = "([\\w\\.~!$&'()*+,;=-]|%[0-9A-Fa-f][0-9A-Fa-f])*";
    std::string authority = "(" + userinfo + "@)?" + host + "(:\\d*)?";
    std::string hier_part =
        "(//" + authority + path_abempty + "|" + path_absolute_rootless_empty + ")";
    m["uri"] = "^" + schema_pat + ":" + hier_part + query + fragment + "$";

    pchar = "([\\w\\.~!$&'()*+,;=:@-]|%[0-9A-Fa-f][0-9A-Fa-f])";
    query_fragment_char = "([\\w\\.~!$&'()*+,;=:@/\\?-]|%[0-9A-Fa-f][0-9A-Fa-f])*";
    query = "(\\?" + query_fragment_char + ")?";
    fragment = "(#" + query_fragment_char + ")?";
    path_abempty = "(/" + pchar + "*)*";
    std::string path_absolute = "/(" + pchar + "+(/" + pchar + "*)*)?";
    std::string segment_nz_nc = "([\\w\\.~!$&'()*+,;=@-]|%[0-9A-Fa-f][0-9A-Fa-f])+";
    std::string path_noscheme = segment_nz_nc + "(/" + pchar + "*)*";
    userinfo = "([\\w\\.~!$&'()*+,;=:-]|%[0-9A-Fa-f][0-9A-Fa-f])*";
    host = "([\\w\\.~!$&'()*+,;=-]|%[0-9A-Fa-f][0-9A-Fa-f])*";
    authority = "(" + userinfo + "@)?" + host + "(:\\d*)?";
    std::string relative_part =
        "(//" + authority + path_abempty + "|" + path_absolute + "|" + path_noscheme + ")?";
    m["uri-reference"] = "^" + relative_part + query + fragment + "$";

    std::string literals =
        "([\\x21\\x23-\\x24\\x26\\x28-\\x3B\\x3D\\x3F-\\x5B\\x5D\\x5F\\x61-\\x7A\\x7E]"
        "|%[0-9A-Fa-f][0-9A-Fa-f])";
    std::string op = "[+#\\./;\\?&=,!@|]";
    std::string varchar = "(\\w|%[0-9A-Fa-f][0-9A-Fa-f])";
    std::string varname = varchar + "(\\.?" + varchar + ")*";
    std::string varspec = varname + "(:[1-9]\\d?\\d?\\d?|\\*)?";
    std::string variable_list = varspec + "(," + varspec + ")*";
    std::string expression = "\\{(" + op + ")?" + variable_list + "\\}";
    m["uri-template"] = "^(" + literals + "|" + expression + ")*$";

    m["json-pointer"] = "^(/([\\x00-\\x2E]|[\\x30-\\x7D]|[\\x7F-\\U0010FFFF]|~[01])*)*$";
    m["relative-json-pointer"] =
        "^(0|[1-9][0-9]*)(#|(/([\\x00-\\x2E]|[\\x30-\\x7D]|[\\x7F-\\U0010FFFF]|~[01])*)*)$";

    return m;
  }();

  auto it = regex_map.find(format);
  if (it == regex_map.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::string JSONSchemaConverter::JSONStrToPrintableStr(const std::string& json_str) {
  static const std::vector<std::pair<std::string, std::string>> kReplaceMapping = {
      {"\\", "\\\\"}, {"\"", "\\\""}
  };
  std::string result = json_str;
  for (const auto& [k, v] : kReplaceMapping) {
    size_t pos = 0;
    while ((pos = result.find(k, pos)) != std::string::npos) {
      result.replace(pos, k.length(), v);
      pos += v.length();
    }
  }
  return result;
}

bool JSONSchemaConverter::StringSpecKey::operator==(const StringSpecKey& other) const {
  return pattern == other.pattern && min_length == other.min_length &&
         max_length == other.max_length && wrapper == other.wrapper;
}

size_t JSONSchemaConverter::StringSpecKeyHash::operator()(const StringSpecKey& key) const {
  return HashCombine(
      std::hash<std::string>()(key.pattern),
      key.min_length,
      key.max_length,
      std::hash<std::string>()(key.wrapper.first),
      std::hash<std::string>()(key.wrapper.second)
  );
}

// ==================== Range Regex Generation (moved from original) ====================

std::string JSONSchemaConverter::MakePatternForDigitRange(
    char start, char end, int remainingDigits
) {
  std::ostringstream oss;
  if (start == end) {
    oss << start;
  } else {
    oss << "[" << start << "-" << end << "]";
  }
  if (remainingDigits > 0) {
    oss << "\\d{" << remainingDigits << "}";
  }
  return oss.str();
}

std::vector<std::string> JSONSchemaConverter::GenerateNumberPatterns(int64_t lower, int64_t upper) {
  std::vector<std::string> patterns;

  int lower_len = static_cast<int>(std::to_string(lower).size());
  int upper_len = static_cast<int>(std::to_string(upper).size());

  for (int len = lower_len; len <= upper_len; ++len) {
    const int64_t digit_min = static_cast<int64_t>(std::pow(10, len - 1));
    const int64_t digit_max = static_cast<int64_t>(std::pow(10, len)) - 1;

    int64_t start = (len == lower_len) ? lower : digit_min;
    int64_t end = (len == upper_len) ? upper : digit_max;

    std::string start_str = std::to_string(start);
    std::string end_str = std::to_string(end);

    if (len == 1) {
      patterns.push_back(MakePatternForDigitRange(start_str[0], end_str[0], 0));
      continue;
    }

    int prefix = 0;
    while (prefix < len && start_str[prefix] == end_str[prefix]) {
      prefix++;
    }

    if (prefix == len) {
      patterns.push_back(start_str);
      continue;
    }

    if (prefix > 0 && prefix >= len - 2) {
      std::string common_part = start_str.substr(0, prefix);
      patterns.push_back(
          common_part +
          MakePatternForDigitRange(start_str[prefix], end_str[prefix], len - prefix - 1)
      );
      continue;
    }

    if (len == lower_len && len == upper_len) {
      if (start == digit_max) {
        patterns.push_back(start_str);
      } else if (start == digit_min) {
        if (end == digit_max) {
          patterns.push_back("[1-9]\\d{" + std::to_string(len - 1) + "}");
        } else {
          for (size_t i = 0; i < end_str.size(); i++) {
            if (i == 0) {
              if (end_str[0] > '1') {
                patterns.push_back(
                    MakePatternForDigitRange('1', static_cast<char>(end_str[0] - 1), len - 1)
                );
              }
            } else {
              std::string pref = end_str.substr(0, i);
              if (end_str[i] > '0') {
                patterns.push_back(
                    pref +
                    MakePatternForDigitRange('0', static_cast<char>(end_str[i] - 1), len - i - 1)
                );
              }
            }
          }
          patterns.push_back(end_str);
        }
      } else if (end == digit_max) {
        for (size_t i = 0; i < start_str.size(); i++) {
          if (i == 0) {
            if (start_str[0] < '9') {
              patterns.push_back(
                  MakePatternForDigitRange(static_cast<char>(start_str[0] + 1), '9', len - 1)
              );
            }
          } else {
            std::string pref = start_str.substr(0, i);
            if (start_str[i] < '9') {
              patterns.push_back(
                  pref +
                  MakePatternForDigitRange(static_cast<char>(start_str[i] + 1), '9', len - i - 1)
              );
            }
          }
        }
        patterns.push_back(start_str);
      } else {
        char start_first_digit = start_str[0];
        char end_first_digit = end_str[0];

        if (end_first_digit - start_first_digit > 1) {
          patterns.push_back(MakePatternForDigitRange(
              static_cast<char>(start_first_digit + 1),
              static_cast<char>(end_first_digit - 1),
              len - 1
          ));
        }

        for (size_t i = 0; i < start_str.size(); i++) {
          if (i == 0) {
            std::string pref = start_str.substr(0, 1);
            if (start_str[1] < '9') {
              patterns.push_back(
                  pref + MakePatternForDigitRange(static_cast<char>(start_str[1] + 1), '9', len - 2)
              );
            }
          } else {
            std::string pref = start_str.substr(0, i);
            if (start_str[i] < '9') {
              patterns.push_back(
                  pref +
                  MakePatternForDigitRange(static_cast<char>(start_str[i] + 1), '9', len - i - 1)
              );
            }
          }
        }
        patterns.push_back(start_str);

        for (size_t i = 0; i < end_str.size(); i++) {
          if (i == 0) {
            std::string pref = end_str.substr(0, 1);
            if (end_str[1] > '0') {
              patterns.push_back(
                  pref + MakePatternForDigitRange('0', static_cast<char>(end_str[1] - 1), len - 2)
              );
            }
          } else {
            std::string pref = end_str.substr(0, i);
            if (end_str[i] > '0') {
              patterns.push_back(
                  pref +
                  MakePatternForDigitRange('0', static_cast<char>(end_str[i] - 1), len - i - 1)
              );
            }
          }
        }
        patterns.push_back(end_str);
      }
    } else if (len == lower_len && len != upper_len) {
      if (start == digit_min) {
        patterns.push_back("[1-9]\\d{" + std::to_string(len - 1) + "}");
      } else {
        for (size_t i = 0; i < start_str.size(); i++) {
          if (i == 0) {
            if (start_str[0] < '9') {
              patterns.push_back(
                  MakePatternForDigitRange(static_cast<char>(start_str[0] + 1), '9', len - 1)
              );
            }
          } else {
            std::string pref = start_str.substr(0, i);
            if (start_str[i] < '9') {
              patterns.push_back(
                  pref +
                  MakePatternForDigitRange(static_cast<char>(start_str[i] + 1), '9', len - i - 1)
              );
            }
          }
        }
        patterns.push_back(start_str);
      }
    } else if (len != lower_len && len == upper_len) {
      if (end == digit_max) {
        patterns.push_back("[1-9]\\d{" + std::to_string(len - 1) + "}");
      } else {
        for (size_t i = 0; i < end_str.size(); i++) {
          if (i == 0) {
            if (end_str[0] > '1') {
              patterns.push_back(
                  MakePatternForDigitRange('1', static_cast<char>(end_str[0] - 1), len - 1)
              );
            }
          } else {
            std::string pref = end_str.substr(0, i);
            if (end_str[i] > '0') {
              patterns.push_back(
                  pref +
                  MakePatternForDigitRange('0', static_cast<char>(end_str[i] - 1), len - i - 1)
              );
            }
          }
        }
        patterns.push_back(end_str);
      }
    } else {
      patterns.push_back("[1-9]\\d{" + std::to_string(len - 1) + "}");
    }
  }

  return patterns;
}

std::string JSONSchemaConverter::GenerateSubRangeRegex(int64_t lower, int64_t upper) {
  std::vector<std::string> patterns = GenerateNumberPatterns(lower, upper);
  std::ostringstream oss;
  for (size_t i = 0; i < patterns.size(); ++i) {
    if (i > 0) {
      oss << "|";
    }
    oss << patterns[i];
  }
  return "(" + oss.str() + ")";
}

std::string JSONSchemaConverter::GenerateRangeRegex(
    std::optional<int64_t> start, std::optional<int64_t> end
) {
  std::vector<std::string> parts;
  std::ostringstream result;

  if (!start && !end) {
    return "^-?\\d+$";
  }

  if (start && !end) {
    if (start.value() <= 0) {
      if (start.value() < 0) {
        parts.push_back("-" + GenerateSubRangeRegex(-(-start.value()), 1));
      }
      parts.push_back("0");
      parts.push_back("[1-9]\\d*");
    } else {
      std::string start_str = std::to_string(start.value());
      int len = static_cast<int>(start_str.length());

      if (len == 1) {
        parts.push_back(MakePatternForDigitRange(start_str[0], '9', 0));
        parts.push_back("[1-9]\\d*");
      } else {
        parts.push_back(start_str);

        for (size_t i = 0; i < start_str.size(); i++) {
          if (i == 0) {
            if (start_str[0] < '9') {
              parts.push_back(
                  MakePatternForDigitRange(static_cast<char>(start_str[0] + 1), '9', len - 1)
              );
            }
          } else {
            std::string pref = start_str.substr(0, i);
            if (start_str[i] < '9') {
              parts.push_back(
                  pref +
                  MakePatternForDigitRange(static_cast<char>(start_str[i] + 1), '9', len - i - 1)
              );
            }
          }
        }

        parts.push_back("[1-9]\\d{" + std::to_string(len) + ",}");
      }
    }
  }

  if (!start && end) {
    if (end.value() >= 0) {
      parts.push_back("-[1-9]\\d*");
      parts.push_back("0");
      if (end.value() > 0) {
        parts.push_back(GenerateSubRangeRegex(1, end.value()));
      }
    } else {
      std::string end_str = std::to_string(-end.value());
      int len = static_cast<int>(end_str.length());

      if (len == 1) {
        parts.push_back("-" + MakePatternForDigitRange(end_str[0], '9', 0));
        parts.push_back("-[1-9]\\d*");
      } else {
        parts.push_back(std::to_string(end.value()));

        for (size_t i = 0; i < end_str.size(); i++) {
          if (i == 0) {
            if (end_str[0] > '1') {
              parts.push_back(
                  "-" + MakePatternForDigitRange('1', static_cast<char>(end_str[0] - 1), len - 1)
              );
            }
          } else {
            std::string pref = end_str.substr(0, i);
            if (end_str[i] > '0') {
              parts.push_back(
                  "-" + pref +
                  MakePatternForDigitRange('0', static_cast<char>(end_str[i] - 1), len - i - 1)
              );
            }
          }
        }

        parts.push_back("-[1-9]\\d{" + std::to_string(len) + ",}");
      }
    }
  }

  if (start && end) {
    int64_t range_start = start.value();
    int64_t range_end = end.value();

    if (range_start > range_end) {
      return "^()$";
    }

    if (range_start < 0) {
      int64_t neg_start = range_start;
      int64_t neg_end = std::min(static_cast<int64_t>(-1), range_end);
      parts.push_back("-" + GenerateSubRangeRegex(-neg_end, -neg_start));
    }

    if (range_start <= 0 && range_end >= 0) {
      parts.push_back("0");
    }

    if (range_end > 0) {
      int64_t pos_start = std::max(static_cast<int64_t>(1), range_start);
      parts.push_back(GenerateSubRangeRegex(pos_start, range_end));
    }
  }

  result << "^(";
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      result << "|";
    }
    result << parts[i];
  }
  result << ")$";

  return result.str();
}

std::string JSONSchemaConverter::FormatFloat(double value, int precision) {
  if (value == static_cast<int64_t>(value)) {
    return std::to_string(static_cast<int64_t>(value));
  }

  std::ostringstream oss;
  oss << std::fixed << std::setprecision(precision) << value;
  std::string result = oss.str();

  size_t decimalPos = result.find('.');
  if (decimalPos != std::string::npos) {
    size_t lastNonZero = result.find_last_not_of('0');
    if (lastNonZero != std::string::npos && lastNonZero > decimalPos) {
      result.erase(lastNonZero + 1);
    } else if (lastNonZero == decimalPos) {
      result.erase(decimalPos);
    }
  }

  return result;
}

std::string JSONSchemaConverter::GenerateFloatRangeRegex(
    std::optional<double> start, std::optional<double> end, int precision
) {
  if ((start && end) && (start.value() > end.value())) {
    return "^()$";
  }

  if (!start && !end) {
    return "^-?\\d+(\\.\\d{1," + std::to_string(precision) + "})?$";
  }

  std::vector<std::string> parts;

  int64_t startInt = 0;
  int64_t endInt = 0;
  double startFrac = 0.0;
  double endFrac = 0.0;
  bool isStartNegative = false;
  bool isEndNegative = false;

  if (start) {
    isStartNegative = start.value() < 0;
    startInt = static_cast<int64_t>(floor(start.value()));
    startFrac = start.value() - startInt;
  }

  if (end) {
    isEndNegative = end.value() < 0;
    endInt = static_cast<int64_t>(floor(end.value()));
    endFrac = end.value() - endInt;
  }

  if (start && !end) {
    std::string startIntStr = FormatFloat(start.value(), precision);
    parts.push_back(startIntStr);

    if (startFrac > 0.0) {
      size_t dotPos = startIntStr.find('.');
      if (dotPos != std::string::npos) {
        std::string intPartStr = startIntStr.substr(0, dotPos);
        std::string fracPartStr = startIntStr.substr(dotPos + 1);

        if (!fracPartStr.empty()) {
          for (size_t i = 0; i < fracPartStr.length(); i++) {
            if (i == 0) {
              if (isStartNegative) {
                for (char d = '0'; d < fracPartStr[0]; d++) {
                  parts.push_back(
                      intPartStr + "\\." + d + "\\d{0," + std::to_string(precision - 1) + "}"
                  );
                }
              } else {
                for (char d = fracPartStr[0] + 1; d <= '9'; d++) {
                  parts.push_back(
                      intPartStr + "\\." + d + "\\d{0," + std::to_string(precision - 1) + "}"
                  );
                }
              }
            } else {
              std::string pref = fracPartStr.substr(0, i);
              if (isStartNegative) {
                if (fracPartStr[i] > '0') {
                  for (char d = '0'; d < fracPartStr[i]; d++) {
                    parts.push_back(
                        intPartStr + "\\." + pref + d + "\\d{0," +
                        std::to_string(precision - i - 1) + "}"
                    );
                  }
                }
              } else {
                for (char d = fracPartStr[i] + 1; d <= '9'; d++) {
                  parts.push_back(
                      intPartStr + "\\." + pref + d + "\\d{0," + std::to_string(precision - i - 1) +
                      "}"
                  );
                }
              }
            }
          }
        }
      }
    }

    if (startInt < INT64_MAX - 1) {
      std::string intRangeRegex = GenerateRangeRegex(startInt + 1, std::nullopt);
      intRangeRegex = intRangeRegex.substr(1, intRangeRegex.length() - 2);
      parts.push_back(intRangeRegex + "(\\.\\d{1," + std::to_string(precision) + "})?");
    }
  } else if (!start && end) {
    std::string endIntStr = FormatFloat(end.value(), precision);
    parts.push_back(endIntStr);

    if (endFrac > 0.0) {
      size_t dotPos = endIntStr.find('.');
      if (dotPos != std::string::npos) {
        std::string intPartStr = endIntStr.substr(0, dotPos);
        std::string fracPartStr = endIntStr.substr(dotPos + 1);

        if (!fracPartStr.empty()) {
          for (size_t i = 0; i < fracPartStr.length(); i++) {
            if (i == 0) {
              if (isEndNegative) {
                for (char d = fracPartStr[0] + 1; d <= '9'; d++) {
                  parts.push_back(
                      intPartStr + "\\." + d + "\\d{0," + std::to_string(precision - 1) + "}"
                  );
                }
              } else {
                for (char d = '0'; d < fracPartStr[0]; d++) {
                  parts.push_back(
                      intPartStr + "\\." + d + "\\d{0," + std::to_string(precision - 1) + "}"
                  );
                }
              }
            } else {
              if (isEndNegative) {
                std::string pref = fracPartStr.substr(0, i);
                for (char d = fracPartStr[i] + 1; d <= '9'; d++) {
                  parts.push_back(
                      intPartStr + "\\." + pref + d + "\\d{0," + std::to_string(precision - i - 1) +
                      "}"
                  );
                }
              } else if (fracPartStr[i] > '0') {
                std::string pref = fracPartStr.substr(0, i);
                for (char d = '0'; d < fracPartStr[i]; d++) {
                  parts.push_back(
                      intPartStr + "\\." + pref + d + "\\d{0," + std::to_string(precision - i - 1) +
                      "}"
                  );
                }
              }
            }
          }
        }
      }
    }

    if (endInt > INT64_MIN + 1) {
      std::string intRangeRegex = GenerateRangeRegex(std::nullopt, endInt - 1);
      intRangeRegex = intRangeRegex.substr(1, intRangeRegex.length() - 2);
      parts.push_back(intRangeRegex + "(\\.\\d{1," + std::to_string(precision) + "})?");
    }
  } else if (start && end) {
    if (startInt == endInt) {
      if (startFrac == 0.0 && endFrac == 0.0) {
        parts.push_back(std::to_string(startInt));
      } else {
        std::string startStr = FormatFloat(start.value(), precision);
        parts.push_back(startStr);

        std::string endStr = FormatFloat(end.value(), precision);
        if (startStr != endStr) {
          parts.push_back(endStr);
        }
      }
    } else {
      std::string startStr = FormatFloat(start.value(), precision);
      parts.push_back(startStr);

      std::string endStr = FormatFloat(end.value(), precision);
      if (startStr != endStr) {
        parts.push_back(endStr);
      }

      if (endInt > startInt + 1) {
        std::string intRangeRegex = GenerateRangeRegex(startInt + 1, endInt - 1);
        intRangeRegex = intRangeRegex.substr(1, intRangeRegex.length() - 2);
        parts.push_back(intRangeRegex + "(\\.\\d{1," + std::to_string(precision) + "})?");
      }

      if (startFrac > 0.0) {
        size_t dotPos = startStr.find('.');
        if (dotPos != std::string::npos) {
          std::string intPartStr = startStr.substr(0, dotPos);
          std::string fracPartStr = startStr.substr(dotPos + 1);

          for (size_t i = 0; i < fracPartStr.length(); i++) {
            if (i == 0) {
              if (isStartNegative) {
                for (char d = '0'; d < fracPartStr[0]; d++) {
                  parts.push_back(
                      intPartStr + "\\." + d + "\\d{0," + std::to_string(precision - 1) + "}"
                  );
                }
              } else {
                for (char d = fracPartStr[0] + 1; d <= '9'; d++) {
                  parts.push_back(
                      intPartStr + "\\." + d + "\\d{0," + std::to_string(precision - 1) + "}"
                  );
                }
              }
            } else {
              std::string pref = fracPartStr.substr(0, i);
              if (isStartNegative) {
                if (fracPartStr[i] > '0') {
                  for (char d = '0'; d < fracPartStr[i]; d++) {
                    parts.push_back(
                        intPartStr + "\\." + pref + d + "\\d{0," +
                        std::to_string(precision - i - 1) + "}"
                    );
                  }
                }
              } else {
                for (char d = fracPartStr[i] + 1; d <= '9'; d++) {
                  parts.push_back(
                      intPartStr + "\\." + pref + d + "\\d{0," + std::to_string(precision - i - 1) +
                      "}"
                  );
                }
              }
            }
          }
        }
      } else {
        parts.push_back(std::to_string(startInt) + "\\.\\d{1," + std::to_string(precision) + "}");
      }

      if (endFrac > 0.0) {
        size_t dotPos = endStr.find('.');
        if (dotPos != std::string::npos) {
          std::string intPartStr = endStr.substr(0, dotPos);
          std::string fracPartStr = endStr.substr(dotPos + 1);

          for (size_t i = 0; i < fracPartStr.length(); i++) {
            if (i == 0) {
              if (isEndNegative) {
                for (char d = fracPartStr[0] + 1; d <= '9'; d++) {
                  parts.push_back(
                      intPartStr + "\\." + d + "\\d{0," + std::to_string(precision - 1) + "}"
                  );
                }
              } else {
                for (char d = '0'; d < fracPartStr[0]; d++) {
                  parts.push_back(
                      intPartStr + "\\." + d + "\\d{0," + std::to_string(precision - 1) + "}"
                  );
                }
              }
            } else {
              if (isEndNegative) {
                std::string pref = fracPartStr.substr(0, i);
                for (char d = fracPartStr[i] + 1; d <= '9'; d++) {
                  parts.push_back(
                      intPartStr + "\\." + pref + d + "\\d{0," + std::to_string(precision - i - 1) +
                      "}"
                  );
                }
              } else if (fracPartStr[i] > '0') {
                std::string pref = fracPartStr.substr(0, i);
                for (char d = '0'; d < fracPartStr[i]; d++) {
                  parts.push_back(
                      intPartStr + "\\." + pref + d + "\\d{0," + std::to_string(precision - i - 1) +
                      "}"
                  );
                }
              }
            }
          }
        }
      } else {
        parts.push_back(std::to_string(endInt) + "\\.\\d{1," + std::to_string(precision) + "}");
      }
    }
  }

  std::ostringstream result;
  result << "^(";
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      result << "|";
    }
    result << parts[i];
  }
  result << ")$";

  return result.str();
}

// ==================== Public API Functions ====================

std::string JSONSchemaToEBNF(
    const std::string& schema,
    bool any_whitespace,
    std::optional<int> indent,
    std::optional<std::pair<std::string, std::string>> separators,
    bool strict_mode,
    std::optional<int> max_whitespace_cnt,
    JSONFormat json_format
) {
  picojson::value schema_value;
  std::string err = picojson::parse(schema_value, schema);
  XGRAMMAR_CHECK(err.empty()) << "Failed to parse JSON: " << err
                              << ". The JSON string is:" << schema;
  return JSONSchemaToEBNF(
      schema_value, any_whitespace, indent, separators, strict_mode, max_whitespace_cnt, json_format
  );
}

std::string JSONSchemaToEBNF(
    const picojson::value& schema,
    bool any_whitespace,
    std::optional<int> indent,
    std::optional<std::pair<std::string, std::string>> separators,
    bool strict_mode,
    std::optional<int> max_whitespace_cnt,
    JSONFormat json_format
) {
  // Parse JSON Schema to SchemaSpec
  SchemaParser parser(schema, {strict_mode, json_format});
  auto spec_result = parser.Parse(schema, "root");
  if (spec_result.IsErr()) {
    XGRAMMAR_LOG(FATAL) << std::move(spec_result).UnwrapErr().what();
  }
  auto spec = std::move(spec_result).Unwrap();
  auto normalized_result = parser.NormalizeExclusiveDisjunctions(spec);
  if (normalized_result.IsErr()) {
    XGRAMMAR_LOG(FATAL) << std::move(normalized_result).UnwrapErr().what();
  }
  spec = std::move(normalized_result).Unwrap();

  auto ref_resolver = [&parser](const std::string& uri, const std::string& rule_name_hint) {
    auto r = parser.ResolveRef(uri, rule_name_hint);
    if (r.IsErr()) {
      XGRAMMAR_LOG(FATAL) << std::move(r).UnwrapErr().what();
    }
    return std::move(r).Unwrap();
  };

  // Create converter based on format
  switch (json_format) {
    case JSONFormat::kJSON: {
      JSONSchemaConverter converter(
          indent, separators, any_whitespace, max_whitespace_cnt, ref_resolver
      );
      return converter.Convert(spec);
    }
    case JSONFormat::kQwenXML:
    case JSONFormat::kMiniMaxXML:
    case JSONFormat::kDeepSeekXML:
    case JSONFormat::kGlmXML: {
      XMLToolCallingConverter converter(
          indent, separators, any_whitespace, max_whitespace_cnt, ref_resolver, json_format
      );
      return converter.Convert(spec);
    }
    default:
      XGRAMMAR_LOG(FATAL) << "Invalid JSON format: " << static_cast<int>(json_format);
  }
}

// Wrapper functions for testing
std::string GenerateRangeRegex(std::optional<int64_t> start, std::optional<int64_t> end) {
  return JSONSchemaConverter::GenerateRangeRegex(start, end);
}

std::string GenerateFloatRangeRegex(std::optional<double> start, std::optional<double> end) {
  return JSONSchemaConverter::GenerateFloatRangeRegex(start, end, 6);
}

std::string QwenXMLToolCallingToEBNF(const std::string& schema) {
  picojson::value json_value;
  std::string err = picojson::parse(json_value, schema);
  if (!err.empty()) {
    XGRAMMAR_LOG(FATAL) << "Failed to parse JSON schema: " << err;
  }
  return JSONSchemaToEBNF(
      json_value, true, std::nullopt, std::nullopt, true, std::nullopt, JSONFormat::kQwenXML
  );
}

std::string MiniMaxXMLToolCallingToEBNF(const std::string& schema) {
  picojson::value json_value;
  std::string err = picojson::parse(json_value, schema);
  if (!err.empty()) {
    XGRAMMAR_LOG(FATAL) << "Failed to parse JSON schema: " << err;
  }
  return JSONSchemaToEBNF(
      json_value, true, std::nullopt, std::nullopt, true, std::nullopt, JSONFormat::kMiniMaxXML
  );
}

std::string DeepSeekXMLToolCallingToEBNF(const std::string& schema) {
  picojson::value json_value;
  std::string err = picojson::parse(json_value, schema);
  if (!err.empty()) {
    XGRAMMAR_LOG(FATAL) << "Failed to parse JSON schema: " << err;
  }
  return JSONSchemaToEBNF(
      json_value, true, std::nullopt, std::nullopt, true, std::nullopt, JSONFormat::kDeepSeekXML
  );
}

std::string GlmXMLToolCallingToEBNF(const std::string& schema) {
  picojson::value json_value;
  std::string err = picojson::parse(json_value, schema);
  if (!err.empty()) {
    XGRAMMAR_LOG(FATAL) << "Failed to parse JSON schema: " << err;
  }
  return JSONSchemaToEBNF(
      json_value, true, std::nullopt, std::nullopt, true, std::nullopt, JSONFormat::kGlmXML
  );
}

}  // namespace xgrammar
