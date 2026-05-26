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
  std::string notion_done_status_name;
  std::string notion_not_done_status_name;
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

inline std::expected<void, std::string>
validate_existing_links(std::span<const state::Link> links) {
  std::map<std::string, std::string> notion_to_reminders;
  std::map<std::string, std::string> reminders_to_notion;

  for (const auto &link : links) {
    auto [notion_it, notion_inserted] = notion_to_reminders.emplace(
        link.notion_page_id, link.reminders_external_id);
    if (!notion_inserted && notion_it->second != link.reminders_external_id) {
      return std::unexpected("Conflicting state links for Notion page `" +
                             link.notion_page_id + "`");
    }

    auto [reminder_it, reminder_inserted] = reminders_to_notion.emplace(
        link.reminders_external_id, link.notion_page_id);
    if (!reminder_inserted && reminder_it->second != link.notion_page_id) {
      return std::unexpected("Conflicting state links for reminder `" +
                             link.reminders_external_id + "`");
    }
  }

  return {};
}

inline const char *source_name(Source source) {
  return source == Source::Notion ? "Notion" : "Reminders";
}

inline Source authoritative_source_for_match(
    const PlanContext &ctx, const notion::PageRecord &notion_record,
    const reminders::ReminderRecord &reminder_record) {
  if (notion_record.recency_iso && reminder_record.recency_iso) {
    if (*notion_record.recency_iso > *reminder_record.recency_iso) {
      return Source::Notion;
    }
    if (*notion_record.recency_iso < *reminder_record.recency_iso) {
      return Source::Reminders;
    }
  } else if (notion_record.recency_iso) {
    return Source::Notion;
  } else if (reminder_record.recency_iso) {
    return Source::Reminders;
  }

  return ctx.truth;
}

inline std::expected<PairPlan, std::string>
plan_pair(const PlanContext &ctx, const state::PairState &existing,
          std::span<const notion::PageRecord> notion_items,
          std::span<const reminders::ReminderRecord> reminders_items) {
  if (auto links_ok = validate_existing_links(existing.links); !links_ok) {
    return std::unexpected(links_ok.error());
  }

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

    const Source authoritative_source =
        authoritative_source_for_match(ctx, n, r);
    const Item &truth =
        (authoritative_source == Source::Notion) ? n.item : r.item;
    const Item &other =
        (authoritative_source == Source::Notion) ? r.item : n.item;
    if (items_equal_for_authority(truth, other, n.extras,
                                  authoritative_source)) {
      return;
    }

    Op op;
    if (authoritative_source == Source::Notion) {
      op.kind = Op::Kind::UpdateReminder;
      op.source_id = n.id;
      op.target_id = r.external_id;
      op.desired = n.item;
      op.current = r.item;
      op.description = "update reminder \"" + op.desired.title +
                       "\" (from " + source_name(authoritative_source) + ")";
    } else {
      op.kind = Op::Kind::UpdateNotionPage;
      op.source_id = r.external_id;
      op.target_id = n.id;
      op.desired = r.item;
      op.current = n.item;
      op.current_notion_extras = n.extras;
      op.description = "update page \"" + op.desired.title +
                       "\" (from " + source_name(authoritative_source) + ")";
    }
    plan.ops.push_back(std::move(op));
  };

  for (const auto &link : existing.links) {
    auto ni = notion_by_id.find(link.notion_page_id);
    auto ri = reminders_by_id.find(link.reminders_external_id);
    if (ni == notion_by_id.end() || ri == reminders_by_id.end()) {
      continue;
    }
    if (matched_notion_ids.contains(link.notion_page_id) ||
        matched_reminders_ids.contains(link.reminders_external_id)) {
      continue;
    }

    record_match(*ni->second, *ri->second);
    matched_notion_ids.insert(link.notion_page_id);
    matched_reminders_ids.insert(link.reminders_external_id);
  }

  std::map<std::string, std::vector<const notion::PageRecord *>> notion_by_title;
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
         "  reminders=" + plan.ctx.reminders_list_name +
         "  (matched=most-recent, tie-breaker=";
  out += source_name(plan.ctx.truth);
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
      std::optional<std::string> status =
          op.desired.done ? plan.ctx.notion_done_status_name
                          : plan.ctx.notion_not_done_status_name;
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

        bool temporarily_uncompleted = false;
        if ((title_arg || notes_arg) && op.current.done) {
          auto e = reminders::uncomplete(reminders_path,
                                         plan.ctx.reminders_list_name,
                                         op.target_id);
          if (!e) {
            return std::unexpected("pre-edit uncomplete failed: " + e.error());
          }
          temporarily_uncompleted = true;
        }

        if (title_arg || notes_arg) {
          auto e = reminders::edit(reminders_path, plan.ctx.reminders_list_name,
                                   op.target_id, title_arg, notes_arg);
          if (!e) {
            return std::unexpected("edit reminder failed: " + e.error());
          }
        }

        if (temporarily_uncompleted) {
          if (op.desired.done) {
            auto e = reminders::complete(reminders_path,
                                         plan.ctx.reminders_list_name,
                                         op.target_id);
            if (!e) {
              return std::unexpected("post-edit complete failed: " + e.error());
            }
          }
        } else if (op.current.done != op.desired.done) {
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
      notion::PropertyFields fields;
      std::optional<std::string> body_arg;

      if (op.current.title != op.desired.title) {
        fields.title = op.desired.title;
      }
      if (op.current.notes != op.desired.notes) {
        body_arg = op.desired.notes;
      }
      if (op.current.due_iso != op.desired.due_iso) {
        if (op.desired.due_iso) {
          fields.due_iso = *op.desired.due_iso;
        } else {
          fields.clear_due_date = true;
        }
      }
      const bool notion_is_done =
          op.current_notion_extras.raw_status.has_value() &&
          is_done_status(*op.current_notion_extras.raw_status);
      if (op.desired.done) {
        if (!notion_is_done) {
          fields.status_name = plan.ctx.notion_done_status_name;
        }
      } else {
        if (notion_is_done) {
          fields.status_name = plan.ctx.notion_not_done_status_name;
        }
      }

      auto e = notion::update_page(ntn_path, op.target_id, fields, body_arg);
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
