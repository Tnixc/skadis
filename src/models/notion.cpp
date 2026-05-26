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

struct DatabaseResponse {
  std::vector<TitleText> title;
  JS_OBJECT(JS_MEMBER(title));
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

inline std::string json_escape_string(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      } else {
        out.push_back(static_cast<char>(c));
      }
    }
  }
  out.push_back('"');
  return out;
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

inline std::string build_properties_json(const PropertyFields &fields) {
  std::string out = "{";
  bool first = true;
  auto comma = [&]() {
    if (!first) {
      out += ',';
    }
    first = false;
  };
  if (fields.title) {
    comma();
    out += "\"Name\":{\"title\":[{\"text\":{\"content\":";
    out += json_escape_string(*fields.title);
    out += "}}]}";
  }
  if (fields.due_iso) {
    comma();
    out += "\"Due Date\":{\"date\":{\"start\":";
    out += json_escape_string(*fields.due_iso);
    out += "}}";
  } else if (fields.clear_due_date) {
    comma();
    out += "\"Due Date\":{\"date\":null}";
  }
  if (fields.status_name) {
    comma();
    out += "\"Status\":{\"status\":{\"name\":";
    out += json_escape_string(*fields.status_name);
    out += "}}";
  }
  out += '}';
  return out;
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
                                     data_source_id,    "--limit",     "100",
                                     "--json"};
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
      return std::unexpected("Failed to parse `ntn datasources query` JSON: " +
                             parsed.error());
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
  std::string request_body = "{\"parent\":{\"data_source_id\":";
  request_body += json_escape_string(parent_data_source_id);
  request_body += "},\"properties\":";
  request_body += build_properties_json(fields);
  request_body += "}";

  auto out = process::run_capture_stdout(
      {ntn_path.string(), "api", "v1/pages", "-d", request_body});
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
  } else if (!parsed->id.empty()) {
    page_id = parsed->id;
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
    std::string request_body = "{\"properties\":";
    request_body += build_properties_json(fields);
    request_body += "}";
    auto out = process::run_capture_stdout(
        {ntn_path.string(), "api", "v1/pages/" + page_id, "-X", "PATCH", "-d",
         request_body});
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
