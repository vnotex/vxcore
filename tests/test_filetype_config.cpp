#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>

#include "core/filetype_config.h"
#include "test_utils.h"

using nlohmann::json;
using vxcore::FileTypeEntry;
using vxcore::FileTypesConfig;

namespace {

json legacyRows() {
  return json::array({
      {{"name", "Text"},
       {"suffixes", {"TxT", "notes"}},
       {"isNewable", false},
       {"isSearchable", false},
       {"displayName", "My text"},
       {"metadata", {{"displayName_ja_JP", "テキスト"}, {"owner", "user"}}}},
      {{"name", "Others"},
       {"suffixes", json::array()},
       {"isNewable", true},
       {"isSearchable", false},
       {"displayName", "Everything else"}},
      {{"name", "Custom"},
       {"suffixes", {"custom"}},
       {"isNewable", true},
       {"isSearchable", true},
       {"displayName", "Custom documents"},
       {"metadata", {{"settings", {{"enabled", true}, {"order", {3, 1, 2}}}}}}}
  });
}

json typstRow(const json &suffixes = json::array({"typst"})) {
  return {{"name", "Typst"},
          {"suffixes", suffixes},
          {"isNewable", true},
          {"isSearchable", true},
          {"displayName", "Typst"}};
}

int test_default_registry_without_explicit_array() {
  const FileTypesConfig defaults;
  ASSERT_EQ(defaults.schema_version, std::uint64_t{1});
  const auto *typst = defaults.GetByName("tYpSt");
  ASSERT_NOT_NULL(typst);
  ASSERT_EQ(typst->ToJson(), typstRow());
  ASSERT_TRUE(defaults.GetBySuffix("TYPST") == typst);
  ASSERT_TRUE(defaults.GetBySuffix("typ") != typst);

  for (const auto &input : {json::object(), json{{"fileTypes", nullptr}},
                           json{{"fileTypes", "not an array"}}}) {
    const auto config = FileTypesConfig::FromJson(input);
    ASSERT_EQ(config.ToJson(), defaults.ToJson());
  }
  const auto saved = defaults.ToJson();
  ASSERT_TRUE(saved["schemaVersion"].is_number_integer());
  ASSERT_EQ(saved["schemaVersion"].get<std::uint64_t>(), std::uint64_t{1});
  ASSERT_EQ(FileTypesConfig::FromJson(json::parse(saved.dump())).ToJson(), saved);
  return 0;
}

int test_legacy_registry_migration() {
  const auto rows = legacyRows();
  auto expected_rows = rows;
  expected_rows.push_back(typstRow());
  const json expected{{"fileTypes", expected_rows}, {"schemaVersion", 1}};
  json inputs = json::array({json{{"fileTypes", rows}}});
  for (const auto &version : json::array(
           {nullptr, false, "1", 1.0, 0, std::uint64_t{0}, -1,
            std::numeric_limits<std::int64_t>::min(), json::array({1}), json::object()})) {
    inputs.push_back({{"fileTypes", rows}, {"schemaVersion", version}});
  }

  for (const auto &input : inputs) {
    const auto config = FileTypesConfig::FromJson(input);
    ASSERT_EQ(config.schema_version, std::uint64_t{1});
    ASSERT_EQ(config.ToJson(), expected);
    const auto *typst = config.GetBySuffix("TYPST");
    ASSERT_NOT_NULL(typst);
    ASSERT_EQ(typst->name, "Typst");
    const auto *text = config.GetBySuffix("txt");
    ASSERT_NOT_NULL(text);
    ASSERT_EQ(text->ToJson(), rows[0]);
    ASSERT_EQ(text->GetDisplayName("ja_JP"), "テキスト");

    const auto once = FileTypesConfig::FromJson(json::parse(config.ToJson().dump()));
    const auto twice = FileTypesConfig::FromJson(json::parse(once.ToJson().dump()));
    ASSERT_EQ(once.ToJson(), expected);
    ASSERT_EQ(twice.ToJson(), expected);
  }
  return 0;
}

int test_claimed_suffix_precedence() {
  // Even Others can own an explicit suffix; it must not be mistaken for a miss.
  for (const auto owner : {0, 1, 2}) {
    auto rows = legacyRows();
    rows[owner]["suffixes"].push_back("TyPsT");
    const auto config = FileTypesConfig::FromJson({{"fileTypes", rows}});
    auto expected_rows = rows;
    expected_rows.push_back(typstRow(json::array()));
    ASSERT_EQ(config.ToJson()["fileTypes"], expected_rows);
    const auto *typst = config.GetByName("Typst");
    ASSERT_NOT_NULL(typst);
    ASSERT_TRUE(typst->suffixes.empty());
    ASSERT_TRUE(typst->is_newable);
    ASSERT_TRUE(typst->is_searchable);
    for (const auto *suffix : {"typst", "TYPST"}) {
      const auto *entry = config.GetBySuffix(suffix);
      ASSERT_NOT_NULL(entry);
      ASSERT_EQ(entry->ToJson(), rows[owner]);
    }
    const auto saved = config.ToJson();
    ASSERT_EQ(FileTypesConfig::FromJson(json::parse(saved.dump())).ToJson(), saved);
  }
  return 0;
}

int test_existing_typst_is_authoritative() {
  for (const auto &suffixes : {json::array(), json::array({"CustomTypst", "draft"})}) {
    auto typst = typstRow(suffixes);
    typst["name"] = "tYpSt";
    typst["displayName"] = "My typesetting";
    typst["isNewable"] = false;
    typst["isSearchable"] = false;
    typst["metadata"] = {{"displayName_en_US", "User documents"},
                         {"custom", {{"values", {1, "two", false}}}}};
    auto rows = legacyRows();
    rows.insert(rows.begin() + 1, typst);
    const auto config = FileTypesConfig::FromJson({{"fileTypes", rows}});
    ASSERT_EQ(config.ToJson()["fileTypes"], rows);
    const auto *entry = config.GetByName("TYPST");
    ASSERT_NOT_NULL(entry);
    ASSERT_EQ(entry->ToJson(), typst);
    ASSERT_FALSE(entry->is_newable);
    ASSERT_FALSE(entry->is_searchable);
    ASSERT_EQ(entry->GetDisplayName("en_US"), "User documents");
    ASSERT_TRUE(config.GetBySuffix("typst") != entry);
    for (const auto &suffix : suffixes) {
      ASSERT_TRUE(config.GetBySuffix(suffix.get<std::string>()) == entry);
    }
    const auto saved = config.ToJson();
    ASSERT_EQ(FileTypesConfig::FromJson(json::parse(saved.dump())).ToJson(), saved);
  }
  return 0;
}

int test_missing_searchable_flag_migration() {
  json rows = json::array();
  json expected_rows = json::array();
  for (const auto *name : {"tYpSt", "mArKdOwN", "tExT", "mInDmAp", "PDF", "Others", "Custom"}) {
    json row{{"name", name}, {"suffixes", json::array()},
             {"displayName", name}, {"isNewable", false}};
    rows.push_back(row);
    row["isSearchable"] = std::string(name) == "tYpSt" || std::string(name) == "mArKdOwN" ||
                          std::string(name) == "tExT" || std::string(name) == "mInDmAp";
    expected_rows.push_back(row);
  }
  const auto migrated = FileTypesConfig::FromJson({{"fileTypes", rows}});
  ASSERT_EQ(migrated.ToJson()["fileTypes"], expected_rows);
  const auto saved = migrated.ToJson();
  ASSERT_EQ(FileTypesConfig::FromJson(json::parse(saved.dump())).ToJson(), saved);

  for (auto &row : rows) {
    row["isSearchable"] = false;
  }
  ASSERT_EQ(FileTypesConfig::FromJson({{"fileTypes", rows}}).ToJson()["fileTypes"], rows);
  return 0;
}

int test_explicit_empty_registry() {
  json inputs = json::array({json{{"fileTypes", json::array()}}});
  inputs.push_back({{"fileTypes", json::array()}, {"schemaVersion", 1}});
  inputs.push_back({{"fileTypes", json::array()}, {"schemaVersion", 17}});
  for (const auto &input : inputs) {
    const auto config = FileTypesConfig::FromJson(input);
    ASSERT_TRUE(config.types.empty());
    ASSERT_NULL(config.GetByName("Typst"));
    ASSERT_NULL(config.GetBySuffix("typst"));
    const auto saved = config.ToJson();
    ASSERT_EQ(saved["fileTypes"], json::array());
    ASSERT_EQ(saved["schemaVersion"], input.value("schemaVersion", json(1)));
    ASSERT_EQ(FileTypesConfig::FromJson(json::parse(saved.dump())).ToJson(), saved);
  }
  return 0;
}

int test_deliberate_deletion_survives_reload() {
  auto config = FileTypesConfig::FromJson({{"fileTypes", legacyRows()}});
  ASSERT_NOT_NULL(config.GetByName("Typst"));
  auto replacement = config.types;
  replacement.erase(std::remove_if(replacement.begin(), replacement.end(),
                                   [](const FileTypeEntry &entry) { return entry.name == "Typst"; }),
                    replacement.end());
  ASSERT_TRUE(config.SetTypes(std::move(replacement)).success);
  ASSERT_EQ(config.schema_version, std::uint64_t{1});
  ASSERT_EQ(config.ToJson()["fileTypes"], legacyRows());
  const auto saved = config.ToJson();
  const auto reloaded = FileTypesConfig::FromJson(json::parse(saved.dump()));
  ASSERT_EQ(reloaded.ToJson(), saved);
  ASSERT_NULL(reloaded.GetByName("Typst"));
  const auto *fallback = reloaded.GetBySuffix("TYPST");
  ASSERT_NOT_NULL(fallback);
  ASSERT_EQ(fallback->name, "Others");
  return 0;
}

int test_current_and_future_schema_versions() {
  for (const auto &version : json::array(
           {1, 17, std::numeric_limits<std::int64_t>::max(),
            std::uint64_t{1} << 63, std::numeric_limits<std::uint64_t>::max()})) {
    const json input{{"fileTypes", legacyRows()}, {"schemaVersion", version}};
    auto config = FileTypesConfig::FromJson(input);
    ASSERT_EQ(config.schema_version, version.get<std::uint64_t>());
    ASSERT_NULL(config.GetByName("Typst"));
    ASSERT_EQ(config.ToJson(), input);
    const auto reloaded = FileTypesConfig::FromJson(json::parse(config.ToJson().dump()));
    ASSERT_EQ(reloaded.schema_version, version.get<std::uint64_t>());
    ASSERT_EQ(reloaded.ToJson(), input);

    ASSERT_TRUE(config.SetTypes({FileTypeEntry("Custom", {"custom"}, false, "Saved", true)}).success);
    ASSERT_EQ(config.schema_version, version.get<std::uint64_t>());
    const auto saved = config.ToJson();
    ASSERT_EQ(saved["schemaVersion"].get<std::uint64_t>(), version.get<std::uint64_t>());
    ASSERT_EQ(FileTypesConfig::FromJson(json::parse(saved.dump())).ToJson(), saved);
    ASSERT_TRUE(config.SetTypes({}).success);
    ASSERT_EQ(config.schema_version, version.get<std::uint64_t>());
    ASSERT_TRUE(FileTypesConfig::FromJson(json::parse(config.ToJson().dump())).types.empty());
  }
  return 0;
}

}  // namespace

int main() {
  // Pure in-memory config tests: no backend context or user configuration is opened.
  RUN_TEST(test_default_registry_without_explicit_array);
  RUN_TEST(test_legacy_registry_migration);
  RUN_TEST(test_claimed_suffix_precedence);
  RUN_TEST(test_existing_typst_is_authoritative);
  RUN_TEST(test_missing_searchable_flag_migration);
  RUN_TEST(test_explicit_empty_registry);
  RUN_TEST(test_deliberate_deletion_survives_reload);
  RUN_TEST(test_current_and_future_schema_versions);
  std::cout << "All filetype config tests passed" << std::endl;
  return 0;
}
