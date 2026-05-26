#include "../models/config.cpp"
#include "../models/process.cpp"
#include "../models/item.cpp"
#include "../models/notion.cpp"
#include "../models/reminders.cpp"
#include "../models/state.cpp"
#include "../models/sync.cpp"

#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct CliArgs {
  bool dry_run{false};
  bool show_help{false};
};

std::expected<CliArgs, std::string> parse_args(int argc, char **argv) {
  CliArgs args;
  std::vector<std::string_view> rest;
  for (int i = 1; i < argc; ++i) {
    rest.emplace_back(argv[i]);
  }

  size_t i = 0;
  if (i < rest.size() && rest[i] == "sync") {
    ++i;
  }
  for (; i < rest.size(); ++i) {
    auto arg = rest[i];
    if (arg == "--dry-run") {
      args.dry_run = true;
    } else if (arg == "-h" || arg == "--help") {
      args.show_help = true;
    } else {
      return std::unexpected(std::string("Unknown argument: ") +
                             std::string(arg));
    }
  }
  return args;
}

void print_help() {
  std::println("Usage: skadis [sync] [--dry-run]");
  std::println("");
  std::println("Syncs each configured Notion <-> Reminders pair using the");
  std::println("`source_of_truth` from ~/.config/skadis/config.json. With");
  std::println("--dry-run, prints the plan without mutating either side.");
}

int run_sync(const skadis::Config &config, bool dry_run) {
  auto state = skadis::state::load();
  if (!state) {
    std::println(stderr, "Failed to load state: {}", state.error());
    return 1;
  }

  bool any_failed = false;
  bool state_dirty = false;

  for (const auto &pair : config.pairs) {
    const auto &notion_database_id = pair.first.value;
    const auto &reminders_list = pair.second.value;
    const auto key =
        skadis::state::pair_key(notion_database_id, reminders_list);

    std::println("\n== {} <-> {} ==", notion_database_id, reminders_list);

    auto data_source_id =
        skadis::notion::resolve_data_source(config.ntn_path,
                                            notion_database_id);
    if (!data_source_id) {
      std::println(stderr, "  resolve failed: {}", data_source_id.error());
      any_failed = true;
      continue;
    }

    auto notion_records =
        skadis::notion::query_pages(config.ntn_path, *data_source_id);
    if (!notion_records) {
      std::println(stderr, "  notion query failed: {}",
                   notion_records.error());
      any_failed = true;
      continue;
    }

    auto reminders_records =
        skadis::reminders::list_items(config.reminders_path, reminders_list);
    if (!reminders_records) {
      std::println(stderr, "  reminders list failed: {}",
                   reminders_records.error());
      any_failed = true;
      continue;
    }

    skadis::sync::PlanContext ctx{
        .truth = config.source,
        .notion_database_id = notion_database_id,
        .data_source_id = *data_source_id,
        .reminders_list_name = reminders_list,
    };

    const auto pair_state_it = state->pairs.find(key);
    const skadis::state::PairState empty_state{};
    const auto &pair_state =
        pair_state_it != state->pairs.end() ? pair_state_it->second
                                            : empty_state;

    auto plan = skadis::sync::plan_pair(
        ctx, pair_state, std::span(*notion_records),
        std::span(*reminders_records));

    std::print("{}", skadis::sync::render_plan(plan));

    if (dry_run) {
      continue;
    }
    if (plan.ops.empty()) {
      state->pairs[key] = {.links = plan.matched_links};
      state_dirty = true;
      continue;
    }

    auto applied =
        skadis::sync::apply_plan(plan, config.ntn_path, config.reminders_path);
    if (!applied) {
      std::println(stderr, "  apply failed: {}", applied.error());
      any_failed = true;
      continue;
    }
    state->pairs[key] = std::move(*applied);
    state_dirty = true;
  }

  if (!dry_run && state_dirty) {
    auto saved = skadis::state::save(*state);
    if (!saved) {
      std::println(stderr, "Failed to save state: {}", saved.error());
      any_failed = true;
    }
  }

  return any_failed ? 1 : 0;
}

} // namespace

int main(int argc, char **argv) {
  auto args = parse_args(argc, argv);
  if (!args) {
    std::println(stderr, "{}", args.error());
    print_help();
    return 2;
  }
  if (args->show_help) {
    print_help();
    return 0;
  }

  const auto config = skadis::Config::load();
  if (!config) {
    std::println(stderr, "{}", config.error());
    return 1;
  }

  return run_sync(*config, args->dry_run);
}
