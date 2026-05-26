#include <expected>
#include <filesystem>
#include <json_struct/json_struct.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace skadis::reminders {

struct ReminderRecord {
  std::string external_id;
  Item item;
};

namespace raw {

struct Reminder {
  std::string externalId;
  std::string title;
  std::shared_ptr<std::string> notes;
  std::shared_ptr<std::string> dueDate;
  bool isCompleted{};
  JS_OBJECT(JS_MEMBER(externalId), JS_MEMBER(title), JS_MEMBER(notes),
            JS_MEMBER(dueDate), JS_MEMBER(isCompleted));
};

struct AddedReminder {
  std::string externalId;
  JS_OBJECT(JS_MEMBER(externalId));
};

template <typename T>
inline std::expected<T, std::string> parse_json(const std::string &input) {
  T out{};
  JS::ParseContext ctx(input);
  ctx.tokenizer.allowAsciiType(true);
  ctx.tokenizer.allowComments(true);
  ctx.tokenizer.allowNewLineAsTokenDelimiter(true);
  ctx.tokenizer.allowSuperfluousComma(true);
  if (ctx.parseTo(out) != JS::Error::NoError) {
    return std::unexpected(ctx.makeErrorString());
  }
  return out;
}

} // namespace raw

inline std::expected<std::vector<ReminderRecord>, std::string>
list_items(const std::filesystem::path &reminders_path,
           const std::string &list_name) {
  auto out = process::run_capture_stdout(
      {reminders_path.string(), "show", list_name, "--include-completed",
       "--format", "json"});
  if (!out) {
    return std::unexpected(out.error());
  }

  auto parsed = raw::parse_json<std::vector<raw::Reminder>>(*out);
  if (!parsed) {
    return std::unexpected("Failed to parse `reminders show` JSON: " +
                           parsed.error());
  }

  std::vector<ReminderRecord> records;
  records.reserve(parsed->size());
  for (auto &r : *parsed) {
    ReminderRecord rec;
    rec.external_id = std::move(r.externalId);
    rec.item.title = std::move(r.title);
    rec.item.notes = r.notes ? std::move(*r.notes) : std::string();
    if (r.dueDate) {
      rec.item.due_iso = normalize_iso_utc(*r.dueDate);
    }
    rec.item.done = r.isCompleted;
    records.push_back(std::move(rec));
  }
  return records;
}

inline std::expected<std::string, std::string>
add(const std::filesystem::path &reminders_path, const std::string &list_name,
    const std::string &title, const std::optional<std::string> &notes,
    const std::optional<std::string> &due_iso, bool completed) {
  std::vector<std::string> argv = {reminders_path.string(), "add", list_name,
                                   title, "--format", "json"};
  if (notes && !notes->empty()) {
    argv.push_back("--notes");
    argv.push_back(*notes);
  }
  if (due_iso) {
    argv.push_back("--due-date");
    argv.push_back(*due_iso);
  }

  auto out = process::run_capture_stdout(argv);
  if (!out) {
    return std::unexpected(out.error());
  }

  auto parsed = raw::parse_json<raw::AddedReminder>(*out);
  if (!parsed) {
    return std::unexpected("Failed to parse `reminders add` JSON: " +
                           parsed.error());
  }
  if (parsed->externalId.empty()) {
    return std::unexpected("`reminders add` did not return an externalId");
  }

  if (completed) {
    auto follow_up = process::run_capture_stdout(
        {reminders_path.string(), "complete", list_name, parsed->externalId});
    if (!follow_up) {
      return std::unexpected(follow_up.error());
    }
  }

  return parsed->externalId;
}

inline std::expected<void, std::string>
edit(const std::filesystem::path &reminders_path, const std::string &list_name,
     const std::string &external_id, const std::optional<std::string> &title,
     const std::optional<std::string> &notes) {
  std::vector<std::string> argv = {reminders_path.string(), "edit", list_name,
                                   external_id};
  if (notes) {
    argv.push_back("--notes");
    argv.push_back(*notes);
  }
  if (title) {
    argv.push_back(*title);
  }
  auto out = process::run_capture_stdout(argv);
  if (!out) {
    return std::unexpected(out.error());
  }
  return {};
}

inline std::expected<void, std::string>
complete(const std::filesystem::path &reminders_path,
         const std::string &list_name, const std::string &external_id) {
  auto out = process::run_capture_stdout(
      {reminders_path.string(), "complete", list_name, external_id});
  if (!out) {
    return std::unexpected(out.error());
  }
  return {};
}

inline std::expected<void, std::string>
uncomplete(const std::filesystem::path &reminders_path,
           const std::string &list_name, const std::string &external_id) {
  auto out = process::run_capture_stdout(
      {reminders_path.string(), "uncomplete", list_name, external_id});
  if (!out) {
    return std::unexpected(out.error());
  }
  return {};
}

inline std::expected<void, std::string>
remove(const std::filesystem::path &reminders_path,
       const std::string &list_name, const std::string &external_id) {
  auto out = process::run_capture_stdout(
      {reminders_path.string(), "delete", list_name, external_id});
  if (!out) {
    return std::unexpected(out.error());
  }
  return {};
}

} // namespace skadis::reminders
