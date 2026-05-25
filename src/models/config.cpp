#include <algorithm>
#include <cctype>
#include <expected>
#include <filesystem>
#include <json_struct/json_struct.h>
#include <string>
#include <string_view>
#include <vector>
using namespace std;

enum class Source { Notion, Reminders };

struct NotionId {
  string value;
};

struct RemindersList {
  string value;
};

using SyncPair = pair<NotionId, RemindersList>;

string normalize_source_name(string value) {
  transform(value.begin(), value.end(), value.begin(),
            [](unsigned char character) {
              return static_cast<char>(tolower(character));
            });
  return value;
}

expected<Source, string> parse_source(const string &raw_source) {
  const auto normalized = normalize_source_name(raw_source);
  if (normalized == "notion") {
    return Source::Notion;
  }

  if (normalized == "reminders") {
    return Source::Reminders;
  }

  return unexpected("Unsupported source: " + raw_source);
}

struct RawPair {
  string notion_id;
  string reminders_list;

  JS_OBJECT(JS_MEMBER_WITH_NAME_AND_ALIASES(notion_id, "notion_id", "notion"),
            JS_MEMBER_WITH_NAME_AND_ALIASES(reminders_list, "reminders_list",
                                            "reminders"));
};

struct RawConfig {
  string source;
  string ntn_path;
  string reminders_path;
  vector<RawPair> pairs;

  JS_OBJ(source, ntn_path, reminders_path, pairs);
};

struct Config {
  Source source{};
  filesystem::path ntn_path;
  vector<SyncPair> pairs;
  filesystem::path reminders_path;

  expected<void, string> validate() const {
    if (!filesystem::exists(ntn_path)) {
      return unexpected("The provided path for `ntn` doesn't exist: " +
                        ntn_path.generic_string());
    }

    if (!filesystem::exists(reminders_path)) {
      return unexpected("The provided path for `reminders` doesn't exist: " +
                        reminders_path.generic_string());
    }

    return {};
  }
};

expected<Config, string> to_config(const RawConfig &raw_config) {
  const auto source = parse_source(raw_config.source);
  if (!source) {
    return unexpected(source.error());
  }

  Config config{
      .source = *source,
      .ntn_path = filesystem::path(raw_config.ntn_path),
      .reminders_path = filesystem::path(raw_config.reminders_path),
  };

  config.pairs.reserve(raw_config.pairs.size());
  for (const auto &raw_pair : raw_config.pairs) {
    config.pairs.emplace_back(NotionId{raw_pair.notion_id},
                              RemindersList{raw_pair.reminders_list});
  }

  return config;
}

expected<Config, string> deserialize_config(string_view raw_config) {
  RawConfig parsed_config;
  string config_text(raw_config);
  JS::ParseContext context(config_text);

  context.tokenizer.allowAsciiType(true);
  context.tokenizer.allowComments(true);
  context.tokenizer.allowNewLineAsTokenDelimiter(true);
  context.tokenizer.allowSuperfluousComma(true);

  if (context.parseTo(parsed_config) != JS::Error::NoError) {
    return unexpected(context.makeErrorString());
  }

  auto config = to_config(parsed_config);
  if (!config) {
    return unexpected(config.error());
  }

  if (auto validation = config->validate(); !validation) {
    return unexpected(validation.error());
  }

  return config;
}
