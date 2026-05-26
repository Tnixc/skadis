#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace skadis::sync {

struct PlanContext {
  Source truth{};
  std::string notion_database_id;
  std::string data_source_id;
  std::string reminders_list_name;
};

struct Op {
  enum class Kind {
    CreateReminder,
    CreateNotionPage,
    UpdateReminder,
    UpdateNotionPage,
  };

  Kind kind{};
  Item desired;
  Item current;
  NotionExtras current_notion_extras;
  std::string source_id;
  std::string target_id;
  std::string description;
};

struct PairPlan {
  PlanContext ctx;
  std::vector<state::Link> matched_links;
  std::vector<Op> ops;
};

inline PairPlan
plan_pair(const PlanContext &ctx, const state::PairState &existing,
          std::span<const notion::PageRecord> notion_items,
          std::span<const reminders::ReminderRecord> reminders_items) {
  PairPlan plan;
  plan.ctx = ctx;

  std::map<std::string, const notion::PageRecord *> notion_by_id;
  for (const auto &r : notion_items) {
    notion_by_id[r.id] = &r;
  }
  std::map<std::string, const reminders::ReminderRecord *> reminders_by_id;
  for (const auto &r : reminders_items) {
    reminders_by_id[r.external_id] = &r;
  }

  std::set<std::string> matched_notion_ids;
  std::set<std::string> matched_reminders_ids;

  auto record_match = [&](const notion::PageRecord &n,
                          const reminders::ReminderRecord &r) {
    plan.matched_links.push_back({n.id, r.external_id});

    const Item &truth = (ctx.truth == Source::Notion) ? n.item : r.item;
    const Item &other = (ctx.truth == Source::Notion) ? r.item : n.item;
    if (items_equal_for_truth(truth, other, n.extras, ctx.truth)) {
      return;
    }

    Op op;
    if (ctx.truth == Source::Notion) {
      op.kind = Op::Kind::UpdateReminder;
      op.source_id = n.id;
      op.target_id = r.external_id;
      op.desired = n.item;
      op.current = r.item;
      op.description = "update reminder \"" + n.item.title + "\"";
    } else {
      op.kind = Op::Kind::UpdateNotionPage;
      op.source_id = r.external_id;
      op.target_id = n.id;
      op.desired = r.item;
      op.current = n.item;
      op.current_notion_extras = n.extras;
      op.description = "update page \"" + r.item.title + "\"";
    }
    plan.ops.push_back(std::move(op));
  };

  for (const auto &link : existing.links) {
    auto ni = notion_by_id.find(link.notion_page_id);
    auto ri = reminders_by_id.find(link.reminders_external_id);
    if (ni != notion_by_id.end() && ri != reminders_by_id.end()) {
      record_match(*ni->second, *ri->second);
      matched_notion_ids.insert(link.notion_page_id);
      matched_reminders_ids.insert(link.reminders_external_id);
    }
  }

  std::map<std::string, std::vector<const notion::PageRecord *>>
      notion_by_title;
  for (const auto &r : notion_items) {
    if (!matched_notion_ids.contains(r.id)) {
      notion_by_title[r.item.title].push_back(&r);
    }
  }
  for (const auto &r : reminders_items) {
    if (matched_reminders_ids.contains(r.external_id)) {
      continue;
    }
    auto it = notion_by_title.find(r.item.title);
    if (it == notion_by_title.end() || it->second.empty()) {
      continue;
    }
    const notion::PageRecord *np = it->second.front();
    it->second.erase(it->second.begin());
    record_match(*np, r);
    matched_notion_ids.insert(np->id);
    matched_reminders_ids.insert(r.external_id);
  }

  for (const auto &r : notion_items) {
    if (matched_notion_ids.contains(r.id)) {
      continue;
    }
    Op op;
    op.kind = Op::Kind::CreateReminder;
    op.source_id = r.id;
    op.desired = r.item;
    op.description = "create reminder \"" + r.item.title + "\" (from Notion)";
    plan.ops.push_back(std::move(op));
  }
  for (const auto &r : reminders_items) {
    if (matched_reminders_ids.contains(r.external_id)) {
      continue;
    }
    Op op;
    op.kind = Op::Kind::CreateNotionPage;
    op.source_id = r.external_id;
    op.desired = r.item;
    op.description = "create page \"" + r.item.title + "\" (from Reminders)";
    plan.ops.push_back(std::move(op));
  }

  return plan;
}

inline std::string render_plan(const PairPlan &plan) {
  std::string out;
  out += "Pair: notion=" + plan.ctx.notion_database_id +
         "  reminders=" + plan.ctx.reminders_list_name + "  (truth=";
  out += (plan.ctx.truth == Source::Notion) ? "Notion" : "Reminders";
  out += ")\n";
  out += "  matched: " + std::to_string(plan.matched_links.size()) +
         "  ops: " + std::to_string(plan.ops.size()) + "\n";
  for (const auto &op : plan.ops) {
    char marker = '~';
    if (op.kind == Op::Kind::CreateReminder ||
        op.kind == Op::Kind::CreateNotionPage) {
      marker = '+';
    }
    out += "  ";
    out.push_back(marker);
    out += ' ';
    out += op.description;
    out += '\n';
  }
  return out;
}

inline void drop_link_for_reminders_id(state::PairState &s,
                                       const std::string &external_id) {
  for (auto it = s.links.begin(); it != s.links.end();) {
    if (it->reminders_external_id == external_id) {
      it = s.links.erase(it);
    } else {
      ++it;
    }
  }
}

inline std::expected<state::PairState, std::string>
apply_plan(const PairPlan &plan, const std::filesystem::path &ntn_path,
           const std::filesystem::path &reminders_path) {
  state::PairState new_state;
  new_state.links = plan.matched_links;

  for (const auto &op : plan.ops) {
    switch (op.kind) {
    case Op::Kind::CreateReminder: {
      auto eid = reminders::add(reminders_path, plan.ctx.reminders_list_name,
                                op.desired.title, op.desired.notes,
                                op.desired.due_iso, op.desired.done);
      if (!eid) {
        return std::unexpected("create reminder failed: " + eid.error());
      }
      new_state.links.push_back({op.source_id, *eid});
      break;
    }
    case Op::Kind::CreateNotionPage: {
      std::optional<std::string> status;
      if (op.desired.done) {
        status = "Done";
      }
      auto pid = notion::create_page(ntn_path, plan.ctx.data_source_id,
                                     op.desired.title, op.desired.notes,
                                     op.desired.due_iso, status);
      if (!pid) {
        return std::unexpected("create page failed: " + pid.error());
      }
      new_state.links.push_back({*pid, op.source_id});
      break;
    }
    case Op::Kind::UpdateReminder: {
      if (op.current.due_iso != op.desired.due_iso) {
        auto del = reminders::remove(
            reminders_path, plan.ctx.reminders_list_name, op.target_id);
        if (!del) {
          return std::unexpected("delete-for-recreate failed: " + del.error());
        }
        drop_link_for_reminders_id(new_state, op.target_id);
        auto eid = reminders::add(reminders_path, plan.ctx.reminders_list_name,
                                  op.desired.title, op.desired.notes,
                                  op.desired.due_iso, op.desired.done);
        if (!eid) {
          return std::unexpected("recreate reminder failed: " + eid.error());
        }
        new_state.links.push_back({op.source_id, *eid});
      } else {
        std::optional<std::string> title_arg;
        std::optional<std::string> notes_arg;
        if (op.current.title != op.desired.title) {
          title_arg = op.desired.title;
        }
        if (op.current.notes != op.desired.notes) {
          notes_arg = op.desired.notes;
        }
        if (title_arg || notes_arg) {
          auto e = reminders::edit(reminders_path, plan.ctx.reminders_list_name,
                                   op.target_id, title_arg, notes_arg);
          if (!e) {
            return std::unexpected("edit reminder failed: " + e.error());
          }
        }
        if (op.current.done != op.desired.done) {
          auto e = op.desired.done
                       ? reminders::complete(reminders_path,
                                             plan.ctx.reminders_list_name,
                                             op.target_id)
                       : reminders::uncomplete(reminders_path,
                                               plan.ctx.reminders_list_name,
                                               op.target_id);
          if (!e) {
            return std::unexpected("complete/uncomplete failed: " + e.error());
          }
        }
      }
      break;
    }
    case Op::Kind::UpdateNotionPage: {
      std::optional<std::string> title_arg;
      std::optional<std::string> body_arg;
      std::optional<std::string> due_arg;
      std::optional<std::string> status_arg;

      if (op.current.title != op.desired.title) {
        title_arg = op.desired.title;
      }
      if (op.current.notes != op.desired.notes) {
        body_arg = op.desired.notes;
      }
      if (op.current.due_iso != op.desired.due_iso && op.desired.due_iso) {
        due_arg = *op.desired.due_iso;
      }
      const bool notion_is_done =
          op.current_notion_extras.raw_status.has_value() &&
          *op.current_notion_extras.raw_status == "Done";
      if (op.desired.done) {
        if (!notion_is_done) {
          status_arg = "Done";
        }
      } else {
        if (notion_is_done) {
          status_arg = "Not started";
        }
      }

      auto e = notion::update_page(ntn_path, op.target_id, title_arg, body_arg,
                                   due_arg, status_arg);
      if (!e) {
        return std::unexpected("update page failed: " + e.error());
      }
      break;
    }
    }
  }

  return new_state;
}

} // namespace skadis::sync
