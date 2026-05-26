#include <algorithm>
#include <cctype>
#include <expected>
#include <filesystem>
#include <json_struct/json_struct.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skadis::notion {

struct PageRecord {
  std::string id;
  Item item;
  NotionExtras extras;
  std::optional<std::string> recency_iso;
};

struct SchemaInfo {
  std::string done_status_name;
  std::string not_done_status_name;
};

namespace raw {

struct DataSourceEntry {
  std::string id;
  JS_OBJECT(JS_MEMBER(id));
};

struct ResolveResponse {
  std::vector<DataSourceEntry> data_sources;
  JS_OBJECT(JS_MEMBER(data_sources));
};

struct TitleText {
  std::string plain_text;
  JS_OBJECT(JS_MEMBER(plain_text));
};

struct TitleProp {
  std::vector<TitleText> title;
  JS_OBJECT(JS_MEMBER(title));
};

struct DateValue {
  std::string start;
  JS_OBJECT(JS_MEMBER(start));
};

struct DateProp {
  std::shared_ptr<DateValue> date;
  JS_OBJECT(JS_MEMBER(date));
};

struct StatusValue {
  std::string name;
  JS_OBJECT(JS_MEMBER(name));
};

struct StatusProp {
  std::shared_ptr<StatusValue> status;
  JS_OBJECT(JS_MEMBER(status));
};

struct PageProperties {
  TitleProp name_prop;
  DateProp due_date_prop;
  StatusProp status_prop;
  JS_OBJECT(JS_MEMBER_WITH_NAME(name_prop, "Name"),
            JS_MEMBER_WITH_NAME(due_date_prop, "Due Date"),
            JS_MEMBER_WITH_NAME(status_prop, "Status"));
};

struct Page {
  std::string id;
  std::optional<std::string> created_time;
  std::optional<std::string> last_edited_time;
  PageProperties properties;
  JS_OBJECT(JS_MEMBER(id), JS_MEMBER(created_time),
            JS_MEMBER(last_edited_time), JS_MEMBER(properties));
};

struct QueryResponse {
  std::vector<Page> results;
  bool has_more{};
  std::shared_ptr<std::string> next_cursor;
  JS_OBJECT(JS_MEMBER(results), JS_MEMBER(has_more), JS_MEMBER(next_cursor));
};

struct MarkdownBody {
  std::string markdown;
  JS_OBJECT(JS_MEMBER(markdown));
};

struct PageGetResponse {
  MarkdownBody markdown;
  JS_OBJECT(JS_MEMBER(markdown));
};

struct PageWithIdOnly {
  std::string id;
  JS_OBJECT(JS_MEMBER(id));
};

struct PageCreateResponse {
  std::optional<std::string> id;
  std::optional<PageWithIdOnly> page;
  JS_OBJECT(JS_MEMBER(id), JS_MEMBER(page));
};

struct DatabaseResponse {
  std::vector<TitleText> title;
  JS_OBJECT(JS_MEMBER(title));
};

struct StatusOption {
  std::string name;
  JS_OBJECT(JS_MEMBER(name));
};

struct StatusSchema {
  std::vector<StatusOption> options;
  JS_OBJECT(JS_MEMBER(options));
};

struct DataSourceProperty {
  std::string type;
  std::optional<StatusSchema> status;
  JS_OBJECT(JS_MEMBER(type), JS_MEMBER(status));
};

struct DataSourceResponse {
  std::map<std::string, DataSourceProperty> properties;
  JS_OBJECT(JS_MEMBER(properties));
};

template <typename T>
inline std::expected<T, std::string> parse_json(const std::string &input) {
  T out{};
  JS::ParseContext ctx(input);
  ctx.tokenizer.allowAsciiType(true);
  ctx.tokenizer.allowComments(true);
  ctx.tokenizer.allowNewLineAsTokenDelimiter(true);
  ctx.tokenizer.allowSuperfluousComma(true);
  ctx.allow_missing_members = true;
  ctx.allow_unasigned_required_members = false;
  if (ctx.parseTo(out) != JS::Error::NoError) {
    return std::unexpected(ctx.makeErrorString());
  }
  return out;
}

} // namespace raw

namespace request {

struct TextContent {
  std::string content;
  JS_OBJECT(JS_MEMBER(content));
};

struct TextValue {
  TextContent text;
  JS_OBJECT(JS_MEMBER(text));
};

struct TitleProperty {
  std::vector<TextValue> title;
  JS_OBJECT(JS_MEMBER(title));
};

struct DateValue {
  std::string start;
  JS_OBJECT(JS_MEMBER(start));
};

struct DateProperty {
  std::shared_ptr<DateValue> date;
  JS_OBJECT(JS_MEMBER(date));
};

struct StatusValue {
  std::string name;
  JS_OBJECT(JS_MEMBER(name));
};

struct StatusProperty {
  StatusValue status;
  JS_OBJECT(JS_MEMBER(status));
};

struct PageProperties {
  std::optional<TitleProperty> name_prop;
  std::optional<DateProperty> due_date_prop;
  std::optional<StatusProperty> status_prop;
  JS_OBJECT(JS_MEMBER_WITH_NAME(name_prop, "Name"),
            JS_MEMBER_WITH_NAME(due_date_prop, "Due Date"),
            JS_MEMBER_WITH_NAME(status_prop, "Status"));
};

struct Parent {
  std::string data_source_id;
  JS_OBJECT(JS_MEMBER(data_source_id));
};

struct PageCreateRequest {
  Parent parent;
  PageProperties properties;
  JS_OBJECT(JS_MEMBER(parent), JS_MEMBER(properties));
};

struct PageUpdateRequest {
  PageProperties properties;
  JS_OBJECT(JS_MEMBER(properties));
};

} // namespace request

inline std::string ascii_lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

inline std::optional<std::string>
find_status_option_case_insensitive(const std::vector<raw::StatusOption> &options,
                                    const std::vector<std::string> &candidates) {
  for (const auto &candidate : candidates) {
    const auto needle = ascii_lower(candidate);
    for (const auto &option : options) {
      if (ascii_lower(option.name) == needle) {
        return option.name;
      }
    }
  }
  return std::nullopt;
}

struct PropertyFields {
  std::optional<std::string> title;
  std::optional<std::string> due_iso;
  bool clear_due_date{false};
  std::optional<std::string> status_name;

  bool empty() const {
    return !title && !due_iso && !clear_due_date && !status_name;
  }
};

inline request::PageProperties build_properties(const PropertyFields &fields) {
  request::PageProperties properties;
  if (fields.title) {
    properties.name_prop = request::TitleProperty{{request::TextValue{{*fields.title}}}};
  }
  if (fields.due_iso) {
    properties.due_date_prop =
        request::DateProperty{std::make_shared<request::DateValue>(
            request::DateValue{*fields.due_iso})};
  } else if (fields.clear_due_date) {
    properties.due_date_prop = request::DateProperty{nullptr};
  }
  if (fields.status_name) {
    properties.status_prop = request::StatusProperty{{*fields.status_name}};
  }
  return properties;
}

inline std::expected<std::string, std::string>
get_database_title(const std::filesystem::path &ntn_path,
                   const std::string &database_id) {
  auto out = process::run_capture_stdout(
      {ntn_path.string(), "api", "v1/databases/" + database_id});
  if (!out) {
    return std::unexpected(out.error());
  }
  auto parsed = raw::parse_json<raw::DatabaseResponse>(*out);
  if (!parsed) {
    return std::unexpected("Failed to parse `ntn api v1/databases` JSON: " +
                           parsed.error());
  }
  std::string title;
  for (const auto &chunk : parsed->title) {
    title += chunk.plain_text;
  }
  return title;
}

inline std::expected<std::string, std::string>
resolve_data_source(const std::filesystem::path &ntn_path,
                    const std::string &database_id) {
  auto out = process::run_capture_stdout(
      {ntn_path.string(), "datasources", "resolve", database_id, "--json"});
  if (!out) {
    return std::unexpected(out.error());
  }
  auto parsed = raw::parse_json<raw::ResolveResponse>(*out);
  if (!parsed) {
    return std::unexpected("Failed to parse `ntn datasources resolve` JSON: " +
                           parsed.error());
  }
  if (parsed->data_sources.empty()) {
    return std::unexpected("No data sources for database " + database_id);
  }
  if (parsed->data_sources.size() > 1) {
    return std::unexpected("Database " + database_id +
                           " has multiple data sources; cannot auto-select");
  }
  return parsed->data_sources.front().id;
}

inline std::expected<SchemaInfo, std::string>
load_schema(const std::filesystem::path &ntn_path,
            const std::string &data_source_id) {
  auto out = process::run_capture_stdout(
      {ntn_path.string(), "api", "v1/data_sources/" + data_source_id});
  if (!out) {
    return std::unexpected(out.error());
  }

  auto parsed = raw::parse_json<raw::DataSourceResponse>(*out);
  if (!parsed) {
    return std::unexpected("Failed to parse `ntn api v1/data_sources` JSON: " +
                           parsed.error());
  }

  const auto require_property_type =
      [&](const std::string &property_name,
          const std::string &expected_type)
      -> std::expected<const raw::DataSourceProperty *, std::string> {
    const auto it = parsed->properties.find(property_name);
    if (it == parsed->properties.end()) {
      return std::unexpected("Data source " + data_source_id +
                             " is missing required property `" + property_name +
                             "`");
    }
    if (it->second.type != expected_type) {
      return std::unexpected("Data source " + data_source_id +
                             " property `" + property_name + "` has type `" +
                             it->second.type + "`, expected `" + expected_type +
                             "`");
    }
    return &it->second;
  };

  auto name_prop = require_property_type("Name", "title");
  if (!name_prop) {
    return std::unexpected(name_prop.error());
  }

  auto due_date_prop = require_property_type("Due Date", "date");
  if (!due_date_prop) {
    return std::unexpected(due_date_prop.error());
  }

  auto status_prop = require_property_type("Status", "status");
  if (!status_prop) {
    return std::unexpected(status_prop.error());
  }
  if (!(*status_prop)->status) {
    return std::unexpected("Data source " + data_source_id +
                           " property `Status` is missing its status schema");
  }

  auto not_done_status_name = find_status_option_case_insensitive(
      (*status_prop)->status->options, {"Not started"});
  if (!not_done_status_name) {
    return std::unexpected(
        "Data source " + data_source_id +
        " property `Status` needs an option matching `Not started`"
        " (case-insensitive)");
  }

  auto done_status_name = find_status_option_case_insensitive(
      (*status_prop)->status->options, {"Done", "Completed"});
  if (!done_status_name) {
    return std::unexpected(
        "Data source " + data_source_id +
        " property `Status` needs an option matching `Done` or `Completed`"
        " (case-insensitive)");
  }

  return SchemaInfo{
      .done_status_name = *done_status_name,
      .not_done_status_name = *not_done_status_name,
  };
}

inline std::expected<std::string, std::string>
get_body(const std::filesystem::path &ntn_path, const std::string &page_id) {
  auto out = process::run_capture_stdout(
      {ntn_path.string(), "pages", "get", page_id, "--json"});
  if (!out) {
    return std::unexpected(out.error());
  }
  auto parsed = raw::parse_json<raw::PageGetResponse>(*out);
  if (!parsed) {
    return std::unexpected("Failed to parse `ntn pages get` JSON: " +
                           parsed.error());
  }
  return parsed->markdown.markdown;
}

inline std::expected<std::vector<PageRecord>, std::string>
query_pages(const std::filesystem::path &ntn_path,
            const std::string &data_source_id) {
  std::vector<PageRecord> records;
  std::optional<std::string> cursor;

  while (true) {
    std::vector<std::string> argv = {ntn_path.string(), "datasources", "query",
                                     data_source_id, "--limit", "100",
                                     "--json"};
    if (cursor) {
      argv.push_back("--start-cursor=" + *cursor);
    }
    auto out = process::run_capture_stdout(argv);
    if (!out) {
      return std::unexpected(out.error());
    }
    auto parsed = raw::parse_json<raw::QueryResponse>(*out);
    if (!parsed) {
      return std::unexpected("Failed to parse `ntn datasources query` JSON: " +
                             parsed.error());
    }

    for (auto &page : parsed->results) {
      PageRecord record;
      record.id = std::move(page.id);
      if (page.created_time) {
        record.recency_iso =
            more_recent_iso_timestamp(record.recency_iso,
                                      normalize_iso_utc(*page.created_time));
      }
      if (page.last_edited_time) {
        record.recency_iso = more_recent_iso_timestamp(
            record.recency_iso, normalize_iso_utc(*page.last_edited_time));
      }

      std::string title;
      for (const auto &chunk : page.properties.name_prop.title) {
        title += chunk.plain_text;
      }
      record.item.title = std::move(title);

      if (page.properties.due_date_prop.date) {
        record.item.due_iso =
            normalize_iso_utc(page.properties.due_date_prop.date->start);
      }

      if (page.properties.status_prop.status) {
        record.extras.raw_status = page.properties.status_prop.status->name;
        record.item.done =
            is_done_status(page.properties.status_prop.status->name);
      } else {
        record.item.done = false;
      }

      auto body = get_body(ntn_path, record.id);
      if (!body) {
        return std::unexpected(body.error());
      }
      record.item.notes = std::move(*body);

      records.push_back(std::move(record));
    }

    if (!parsed->has_more) {
      break;
    }
    if (!parsed->next_cursor) {
      break;
    }
    cursor = *parsed->next_cursor;
  }

  return records;
}

inline std::expected<void, std::string>
write_body(const std::filesystem::path &ntn_path, const std::string &page_id,
           const std::string &body) {
  auto out = process::run_capture_stdout(
      {ntn_path.string(), "pages", "update", page_id,
       "--allow-deleting-content"},
      std::string_view(body));
  if (!out) {
    return std::unexpected(out.error());
  }
  return {};
}

inline std::expected<std::string, std::string>
create_page(const std::filesystem::path &ntn_path,
            const std::string &parent_data_source_id, const std::string &title,
            const std::string &body, std::optional<std::string> due_iso,
            std::optional<std::string> status_name) {
  PropertyFields fields{
      .title = title,
      .due_iso = std::move(due_iso),
      .status_name = std::move(status_name),
  };
  std::string request_body =
      JS::serializeStruct(request::PageCreateRequest{
          .parent = request::Parent{.data_source_id = parent_data_source_id},
          .properties = build_properties(fields),
      });

  auto out = process::run_capture_stdout(
      {ntn_path.string(), "api", "v1/pages"}, std::string_view(request_body));
  if (!out) {
    return std::unexpected(out.error());
  }

  auto parsed = raw::parse_json<raw::PageCreateResponse>(*out);
  if (!parsed) {
    return std::unexpected("Failed to parse `ntn api v1/pages` create JSON: " +
                           parsed.error());
  }
  std::string page_id;
  if (parsed->page && !parsed->page->id.empty()) {
    page_id = parsed->page->id;
  } else if (parsed->id && !parsed->id->empty()) {
    page_id = *parsed->id;
  } else {
    return std::unexpected(
        "Could not find created page id in `ntn api v1/pages` response: " +
        *out);
  }

  if (!body.empty()) {
    auto wrote = write_body(ntn_path, page_id, body);
    if (!wrote) {
      return std::unexpected("Created page " + page_id +
                             " but failed to write body: " + wrote.error());
    }
  }

  return page_id;
}

inline std::expected<void, std::string>
update_page(const std::filesystem::path &ntn_path, const std::string &page_id,
            const PropertyFields &fields,
            const std::optional<std::string> &body) {
  if (!fields.empty()) {
    std::string request_body =
        JS::serializeStruct(request::PageUpdateRequest{
            .properties = build_properties(fields),
        });
    auto out = process::run_capture_stdout(
        {ntn_path.string(), "api", "v1/pages/" + page_id, "-X", "PATCH"},
        std::string_view(request_body));
    if (!out) {
      return std::unexpected(out.error());
    }
  }

  if (body) {
    auto wrote = write_body(ntn_path, page_id, *body);
    if (!wrote) {
      return std::unexpected(wrote.error());
    }
  }

  return {};
}

} // namespace skadis::notion
