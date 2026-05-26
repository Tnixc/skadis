#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#define JS_STL_MAP
#include <json_struct/json_struct.h>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace skadis::state {

struct Link {
  std::string notion_page_id;
  std::string reminders_external_id;
  JS_OBJECT(JS_MEMBER(notion_page_id), JS_MEMBER(reminders_external_id));
};

struct PairState {
  std::vector<Link> links;
  JS_OBJECT(JS_MEMBER(links));
};

struct State {
  std::map<std::string, PairState> pairs;
  JS_OBJECT(JS_MEMBER(pairs));
};

inline std::filesystem::path default_path() {
  const auto *home = std::getenv("HOME");
  if (!home) {
    return {};
  }
  return std::filesystem::path(home) / ".config" / "skadis" / "state.json";
}

inline std::string pair_key(const std::string &notion_database_id,
                            const std::string &reminders_list_name) {
  return notion_database_id + "::" + reminders_list_name;
}

inline std::expected<State, std::string>
load(const std::filesystem::path &path = default_path()) {
  if (path.empty()) {
    return std::unexpected("HOME is not set");
  }
  if (!std::filesystem::exists(path)) {
    return State{};
  }
  std::ifstream file(path);
  std::string raw((std::istreambuf_iterator<char>(file)),
                  std::istreambuf_iterator<char>());

  State state;
  JS::ParseContext ctx(raw);
  ctx.tokenizer.allowAsciiType(true);
  ctx.tokenizer.allowComments(true);
  ctx.tokenizer.allowNewLineAsTokenDelimiter(true);
  ctx.tokenizer.allowSuperfluousComma(true);
  if (ctx.parseTo(state) != JS::Error::NoError) {
    return std::unexpected("Failed to parse state file " + path.string() +
                           ": " + ctx.makeErrorString());
  }
  return state;
}

inline std::expected<void, std::string>
save(const State &state, const std::filesystem::path &path = default_path()) {
  if (path.empty()) {
    return std::unexpected("HOME is not set");
  }
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return std::unexpected("Failed to create state directory: " + ec.message());
  }

  std::string serialized = JS::serializeStruct(state);

  auto tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
    if (!out) {
      return std::unexpected("Failed to open " + tmp.string() + " for writing");
    }
    out << serialized;
    if (!out) {
      return std::unexpected("Failed to write " + tmp.string());
    }
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    return std::unexpected("Failed to rename " + tmp.string() + " -> " +
                           path.string() + ": " + ec.message());
  }
  return {};
}

} // namespace skadis::state
