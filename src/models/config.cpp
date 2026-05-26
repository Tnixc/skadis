#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <json_struct/json_struct.h>
#include <string>
#include <utility>
#include <vector>
namespace skadis {

enum class Source {
  Notion,
  Reminders
};

struct NotionId {
  std::string value;
};

struct RemindersList {
  std::string value;
};

using SyncPair = std::pair<NotionId, RemindersList>;

struct Config {
  Source source{};
  std::filesystem::path ntn_path;
  std::vector<SyncPair> pairs;
  std::filesystem::path reminders_path;

  std::expected<void, std::string> validate() const;
  static std::expected<Config, std::string> load();
};

inline std::string normalize_source_name(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

inline std::expected<Source, std::string>
parse_source(const std::string &raw_source) {
  const auto normalized = normalize_source_name(raw_source);
  if (normalized == "notion") {
    return Source::Notion;
  }

  if (normalized == "reminders") {
    return Source::Reminders;
  }

  return std::unexpected("Unsupported source: " + raw_source);
}

struct RawPair {
  std::string notion_id;
  std::string reminders_list;

  JS_OBJECT(JS_MEMBER_WITH_NAME_AND_ALIASES(notion_id, "notion_id", "notion"),
            JS_MEMBER_WITH_NAME_AND_ALIASES(reminders_list, "reminders_list",
                                            "reminders",
                                            "reminders_list_name"));
};

struct RawConfig {
  std::string source;
  std::string ntn_path;
  std::string reminders_path;
  std::vector<RawPair> pairs;

  JS_OBJECT(JS_MEMBER_WITH_NAME_AND_ALIASES(source, "source",
                                            "source_of_truth"),
            JS_MEMBER_WITH_NAME(ntn_path, "ntn_path"),
            JS_MEMBER_WITH_NAME(reminders_path, "reminders_path"),
            JS_MEMBER_WITH_NAME_AND_ALIASES(pairs, "pairs", "lists"));
};

inline std::expected<Config, std::string>
to_config(const RawConfig &raw_config) {
  const auto source = parse_source(raw_config.source);
  if (!source) {
    return std::unexpected(source.error());
  }

  Config config{
      .source = *source,
      .ntn_path = std::filesystem::path(raw_config.ntn_path),
      .reminders_path = std::filesystem::path(raw_config.reminders_path),
  };

  config.pairs.reserve(raw_config.pairs.size());
  for (const auto &raw_pair : raw_config.pairs) {
    config.pairs.emplace_back(NotionId{raw_pair.notion_id},
                              RemindersList{raw_pair.reminders_list});
  }

  return config;
}

inline std::expected<Config, std::string>
deserialize_config(std::string raw_config) {
  RawConfig parsed_config;
  JS::ParseContext context(raw_config);

  context.tokenizer.allowAsciiType(true);
  context.tokenizer.allowComments(true);
  context.tokenizer.allowNewLineAsTokenDelimiter(true);
  context.tokenizer.allowSuperfluousComma(true);

  if (context.parseTo(parsed_config) != JS::Error::NoError) {
    return std::unexpected(context.makeErrorString());
  }

  auto config = to_config(parsed_config);
  if (!config) {
    return std::unexpected(config.error());
  }

  if (auto validation = config->validate(); !validation) {
    return std::unexpected(validation.error());
  }

  return config;
}

inline std::expected<void, std::string> Config::validate() const {
  if (!std::filesystem::exists(ntn_path)) {
    return std::unexpected("The provided path for `ntn` doesn't exist: " +
                           ntn_path.generic_string());
  }

  if (!std::filesystem::exists(reminders_path)) {
    return std::unexpected("The provided path for `reminders` doesn't exist: " +
                           reminders_path.generic_string());
  }

  return {};
}

inline std::expected<Config, std::string> Config::load() {
  const auto *home = std::getenv("HOME");
  if (!home) {
    return std::unexpected("HOME is not set");
  }

  const auto config_path =
      std::filesystem::path(home) / ".config" / "skadis" / "config.json";
  if (!std::filesystem::exists(config_path)) {
    return std::unexpected("The config file doesn't exist: " +
                           config_path.generic_string());
  }

  std::ifstream file(config_path);
  const auto config_str = std::string(std::istreambuf_iterator<char>(file),
                                      std::istreambuf_iterator<char>());
  return deserialize_config(config_str);
}

} // namespace skadis
