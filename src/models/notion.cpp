#include <expected>
#include <filesystem>
#include <json_struct/json_struct.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skadis::notion {

struct PageRecord {
  std::string id;
  Item item;
  NotionExtras extras;
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
  PageProperties properties;
  JS_OBJECT(JS_MEMBER(id), JS_MEMBER(properties));
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
  std::string id;
  std::shared_ptr<PageWithIdOnly> page;
  JS_OBJECT(JS_MEMBER(id), JS_MEMBER(page));
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

inline std::string yaml_single_quote(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('\'');
  for (char c : s) {
    if (c == '\'') {
      out.push_back('\'');
    }
    out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

struct FrontmatterFields {
  std::optional<std::string> title;
  std::optional<std::string> due_iso;
  std::optional<std::string> status_name;
};

inline std::string build_content(const FrontmatterFields &fields,
                                 std::optional<std::string_view> body) {
  std::string out;
  const bool any = fields.title.has_value() || fields.due_iso.has_value() ||
                   fields.status_name.has_value();
  if (any) {
    out += "---\n";
    if (fields.title) {
      out += "Name: ";
      out += yaml_single_quote(*fields.title);
      out += '\n';
    }
    if (fields.due_iso) {
      out += "Due Date:\n  start: ";
      out += yaml_single_quote(*fields.due_iso);
      out += '\n';
    }
    if (fields.status_name) {
      out += "Status: ";
      out += yaml_single_quote(*fields.status_name);
      out += '\n';
    }
    out += "---\n\n";
  }
  if (body) {
    out += *body;
  }
  return out;
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
    std::vector<std::string> argv = {
        ntn_path.string(), "datasources", "query", data_source_id,
        "--limit",         "100",         "--json"};
    if (cursor) {
      argv.push_back("--start-cursor");
      argv.push_back(*cursor);
    }
    auto out = process::run_capture_stdout(argv);
    if (!out) {
      return std::unexpected(out.error());
    }
    auto parsed = raw::parse_json<raw::QueryResponse>(*out);
    if (!parsed) {
      return std::unexpected(
          "Failed to parse `ntn datasources query` JSON: " + parsed.error());
    }

    for (auto &page : parsed->results) {
      PageRecord record;
      record.id = std::move(page.id);

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
            page.properties.status_prop.status->name == "Done";
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

inline std::expected<std::string, std::string>
create_page(const std::filesystem::path &ntn_path,
            const std::string &parent_data_source_id, const std::string &title,
            const std::string &body, std::optional<std::string> due_iso,
            std::optional<std::string> status_name) {
  FrontmatterFields fields{
      .title = title,
      .due_iso = std::move(due_iso),
      .status_name = std::move(status_name),
  };
  std::string content = build_content(fields, body);

  auto out = process::run_capture_stdout(
      {ntn_path.string(), "pages", "create", "--parent",
       "data-source:" + parent_data_source_id, "--json"},
      std::string_view(content));
  if (!out) {
    return std::unexpected(out.error());
  }

  auto parsed = raw::parse_json<raw::PageCreateResponse>(*out);
  if (!parsed) {
    return std::unexpected("Failed to parse `ntn pages create` JSON: " +
                           parsed.error());
  }
  if (parsed->page && !parsed->page->id.empty()) {
    return parsed->page->id;
  }
  if (!parsed->id.empty()) {
    return parsed->id;
  }
  return std::unexpected(
      "Could not find created page id in `ntn pages create` response: " + *out);
}

inline std::expected<void, std::string>
update_page(const std::filesystem::path &ntn_path, const std::string &page_id,
            std::optional<std::string> title, std::optional<std::string> body,
            std::optional<std::string> due_iso,
            std::optional<std::string> status_name) {
  FrontmatterFields fields{
      .title = std::move(title),
      .due_iso = std::move(due_iso),
      .status_name = std::move(status_name),
  };
  std::optional<std::string_view> body_view;
  if (body) {
    body_view = std::string_view(*body);
  }
  std::string content = build_content(fields, body_view);
  if (content.empty()) {
    return {};
  }

  auto out = process::run_capture_stdout(
      {ntn_path.string(), "pages", "update", page_id, "--json"},
      std::string_view(content));
  if (!out) {
    return std::unexpected(out.error());
  }
  return {};
}

} // namespace skadis::notion
