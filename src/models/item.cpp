#include <array>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace skadis {

struct Item {
  std::string title;
  std::string notes;
  std::optional<std::string> due_iso;
  bool done{};
};

struct NotionExtras {
  std::optional<std::string> raw_status;
};

namespace {

struct DateTimeParts {
  int year{};
  int month{};
  int day{};
  int hour{};
  int minute{};
  int second{};
  int offset_seconds{};
};

inline bool is_digit(char c) { return c >= '0' && c <= '9'; }

inline std::optional<int> parse_fixed_int(std::string_view input, size_t pos,
                                          size_t digits) {
  if (pos + digits > input.size()) {
    return std::nullopt;
  }

  int value = 0;
  for (size_t i = 0; i < digits; ++i) {
    const char c = input[pos + i];
    if (!is_digit(c)) {
      return std::nullopt;
    }
    value = value * 10 + (c - '0');
  }
  return value;
}

inline bool is_leap_year(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

inline int days_in_month(int year, int month) {
  static constexpr std::array<int, 12> month_lengths = {
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
  };
  if (month == 2 && is_leap_year(year)) {
    return 29;
  }
  return month_lengths[static_cast<size_t>(month - 1)];
}

inline bool validate_date(const DateTimeParts &parts) {
  if (parts.month < 1 || parts.month > 12) {
    return false;
  }
  if (parts.day < 1 || parts.day > days_in_month(parts.year, parts.month)) {
    return false;
  }
  return true;
}

inline bool validate_time(const DateTimeParts &parts) {
  return parts.hour >= 0 && parts.hour <= 23 && parts.minute >= 0 &&
         parts.minute <= 59 && parts.second >= 0 && parts.second <= 59;
}

inline bool parse_date(std::string_view input, DateTimeParts &parts) {
  if (input.size() != 10 || input[4] != '-' || input[7] != '-') {
    return false;
  }
  auto year = parse_fixed_int(input, 0, 4);
  auto month = parse_fixed_int(input, 5, 2);
  auto day = parse_fixed_int(input, 8, 2);
  if (!year || !month || !day) {
    return false;
  }

  parts.year = *year;
  parts.month = *month;
  parts.day = *day;
  return validate_date(parts);
}

inline bool parse_time(std::string_view input, DateTimeParts &parts) {
  if (input.size() != 8 || input[2] != ':' || input[5] != ':') {
    return false;
  }
  auto hour = parse_fixed_int(input, 0, 2);
  auto minute = parse_fixed_int(input, 3, 2);
  auto second = parse_fixed_int(input, 6, 2);
  if (!hour || !minute || !second) {
    return false;
  }

  parts.hour = *hour;
  parts.minute = *minute;
  parts.second = *second;
  return validate_time(parts);
}

inline bool parse_offset(std::string_view input, int &offset_seconds) {
  if (input.size() != 6 || (input[0] != '+' && input[0] != '-') ||
      input[3] != ':') {
    return false;
  }

  auto offset_hours = parse_fixed_int(input, 1, 2);
  auto offset_minutes = parse_fixed_int(input, 4, 2);
  if (!offset_hours || !offset_minutes) {
    return false;
  }
  if (*offset_hours > 23 || *offset_minutes > 59) {
    return false;
  }

  offset_seconds = (*offset_hours * 3600 + *offset_minutes * 60) *
                   (input[0] == '-' ? -1 : 1);
  return true;
}

inline std::optional<DateTimeParts>
parse_iso_utc_normalizable(std::string_view input) {
  DateTimeParts parts;

  if (input.size() == 10) {
    if (!parse_date(input, parts)) {
      return std::nullopt;
    }
    return parts;
  }

  if (input.size() == 19 && input[10] == 'T') {
    if (!parse_date(input.substr(0, 10), parts) ||
        !parse_time(input.substr(11, 8), parts)) {
      return std::nullopt;
    }
    return parts;
  }

  if (input.size() == 20 && input[10] == 'T' && input[19] == 'Z') {
    if (!parse_date(input.substr(0, 10), parts) ||
        !parse_time(input.substr(11, 8), parts)) {
      return std::nullopt;
    }
    return parts;
  }

  if (input.size() == 25 && input[10] == 'T') {
    if (!parse_date(input.substr(0, 10), parts) ||
        !parse_time(input.substr(11, 8), parts) ||
        !parse_offset(input.substr(19, 6), parts.offset_seconds)) {
      return std::nullopt;
    }
    return parts;
  }

  if (input.size() == 29 && input[10] == 'T' && input[19] == '.') {
    auto milliseconds = parse_fixed_int(input, 20, 3);
    if (!milliseconds || !parse_date(input.substr(0, 10), parts) ||
        !parse_time(input.substr(11, 8), parts) ||
        !parse_offset(input.substr(23, 6), parts.offset_seconds)) {
      return std::nullopt;
    }
    return parts;
  }

  return std::nullopt;
}

inline std::string format_utc_iso(const DateTimeParts &parts) {
  std::tm tm{};
  tm.tm_year = parts.year - 1900;
  tm.tm_mon = parts.month - 1;
  tm.tm_mday = parts.day;
  tm.tm_hour = parts.hour;
  tm.tm_min = parts.minute;
  tm.tm_sec = parts.second;

  std::time_t timestamp = ::timegm(&tm);
  timestamp -= parts.offset_seconds;

  std::tm utc{};
  ::gmtime_r(&timestamp, &utc);

  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return std::string(buf);
}

} // namespace

inline bool is_done_status(const std::string &name) {
  return name == "Done" || name == "Completed";
}

inline std::optional<std::string> normalize_iso_utc(const std::string &iso) {
  if (iso.empty()) {
    return std::nullopt;
  }

  auto parsed = parse_iso_utc_normalizable(iso);
  if (!parsed) {
    return iso;
  }

  return format_utc_iso(*parsed);
}

inline bool items_equal_for_truth(const Item &truth, const Item &other,
                                  const NotionExtras &notion_side,
                                  Source truth_source) {
  if (truth.title != other.title) {
    return false;
  }
  if (truth.notes != other.notes) {
    return false;
  }
  if (truth.due_iso != other.due_iso) {
    return false;
  }

  if (truth_source == Source::Reminders) {
    const bool notion_is_done = notion_side.raw_status.has_value() &&
                                is_done_status(*notion_side.raw_status);
    if (truth.done) {
      if (!notion_is_done) {
        return false;
      }
    } else {
      if (notion_is_done) {
        return false;
      }
    }
  } else {
    if (truth.done != other.done) {
      return false;
    }
  }
  return true;
}

} // namespace skadis
