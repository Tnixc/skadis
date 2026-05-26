#include <cstdio>
#include <ctime>
#include <optional>
#include <string>

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

inline std::optional<std::string>
normalize_iso_utc(const std::string &iso) {
  if (iso.empty()) {
    return std::nullopt;
  }

  int y = 0;
  int mo = 0;
  int d = 0;
  int h = 0;
  int mi = 0;
  int s = 0;
  int ms = 0;
  char tz_sign = '+';
  int tz_h = 0;
  int tz_m = 0;
  bool parsed = false;

  int n = std::sscanf(iso.c_str(),
                      "%4d-%2d-%2dT%2d:%2d:%2d.%3d%c%2d:%2d", &y, &mo, &d,
                      &h, &mi, &s, &ms, &tz_sign, &tz_h, &tz_m);
  if (n == 10 && (tz_sign == '+' || tz_sign == '-')) {
    parsed = true;
  }
  if (!parsed) {
    n = std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%c%2d:%2d", &y, &mo,
                    &d, &h, &mi, &s, &tz_sign, &tz_h, &tz_m);
    if (n == 9 && (tz_sign == '+' || tz_sign == '-')) {
      parsed = true;
    }
  }
  if (!parsed) {
    char z = 0;
    n = std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%c", &y, &mo, &d, &h,
                    &mi, &s, &z);
    if (n == 7 && z == 'Z') {
      parsed = true;
      tz_sign = '+';
      tz_h = 0;
      tz_m = 0;
    }
  }
  if (!parsed) {
    n = std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h,
                    &mi, &s);
    if (n == 6) {
      parsed = true;
      tz_sign = '+';
      tz_h = 0;
      tz_m = 0;
    }
  }
  if (!parsed) {
    n = std::sscanf(iso.c_str(), "%4d-%2d-%2d", &y, &mo, &d);
    if (n == 3) {
      parsed = true;
      h = 0;
      mi = 0;
      s = 0;
      tz_sign = '+';
      tz_h = 0;
      tz_m = 0;
    }
  }
  if (!parsed) {
    return iso;
  }

  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = s;
  std::time_t t = ::timegm(&tm);
  int offset_seconds = (tz_h * 3600 + tz_m * 60) * (tz_sign == '-' ? -1 : 1);
  t -= offset_seconds;

  std::tm utc{};
  ::gmtime_r(&t, &utc);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return std::string(buf);
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
    const bool notion_is_done =
        notion_side.raw_status.has_value() && *notion_side.raw_status == "Done";
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
