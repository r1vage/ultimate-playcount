#include "maintenance.h"

#include <algorithm>
#include <unordered_map>

namespace upc {

int64_t earliest(int64_t a, int64_t b) { return !a ? b : !b ? a : std::min(a, b); }

record combine(record a, const record& b) {
    a.playcount += b.playcount;
    a.first_played = earliest(a.first_played, b.first_played);
    a.added = earliest(a.added, b.added);
    a.last_played = std::max(a.last_played, b.last_played);
    if (!a.rating) a.rating = b.rating;
    a.loved = a.loved || b.loved;
    if (a.title.empty() && a.path.empty()) { // empty base: take the label too
        a.artist = b.artist;
        a.title = b.title;
        a.album = b.album;
        a.path = b.path;
        a.subsong = b.subsong;
    }
    return a;
}

change_set rekey(const std::vector<std::pair<std::string, record>>& records,
                 const std::vector<std::pair<std::string, std::string>>& moves) {
    std::unordered_map<std::string, const record*> by_key;
    for (auto& [key, rec] : records) by_key.emplace(key, &rec);

    // old key -> distinct new keys, only for keys that exist and actually change somewhere
    std::map<std::string, std::set<std::string>> targets_of;
    for (auto& [from, to] : moves)
        if (by_key.count(from)) targets_of[from].insert(to);
    for (auto it = targets_of.begin(); it != targets_of.end();)
        it = (it->second.size() == 1 && *it->second.begin() == it->first) ? targets_of.erase(it) : std::next(it);

    std::map<std::string, record> result;
    for (auto& [from, tos] : targets_of)
        for (auto& to : tos) {
            auto [it, fresh] = result.try_emplace(to);
            // Keep data already under the new key, unless that record is itself moving away.
            if (fresh && to != from && by_key.count(to) && !targets_of.count(to)) it->second = *by_key[to];
            it->second = combine(it->second, *by_key[from]);
        }

    change_set out;
    for (auto& [from, tos] : targets_of)
        if (!result.count(from)) out.removes.push_back(from);
    out.puts.assign(result.begin(), result.end());
    return out;
}

cleanup_scan scan_for_cleanup(const std::vector<std::pair<std::string, record>>& records,
                              const std::map<location, std::string>& live) {
    std::set<std::string> live_keys;
    for (auto& [loc, key] : live) live_keys.insert(key);
    cleanup_scan out;
    for (auto& [key, rec] : records) {
        if (live_keys.count(key)) continue;
        auto it = live.find({rec.path, rec.subsong});
        if (it != live.end()) out.reattach.emplace_back(key, it->second);
        else out.orphans.push_back(key);
    }
    return out;
}

std::vector<std::string> pick_auto_clean(const std::vector<std::pair<std::string, record>>& orphans,
                                         size_t total_records, int64_t now, int days,
                                         const std::function<bool(const record&)>& file_gone) {
    std::vector<std::string> out;
    if (days <= 0) return out;
    const int64_t cutoff = now - static_cast<int64_t>(days) * 86400;
    for (auto& [key, rec] : orphans)
        if (!rec.rating && !rec.loved && std::max(rec.last_played, rec.added) < cutoff && file_gone(rec))
            out.push_back(key);
    if (out.size() * 4 > total_records) out.clear();
    return out;
}

std::vector<std::string> backups_to_prune(const std::vector<std::string>& names, size_t keep) {
    static const std::string prefix = "ultimate_playcount-", suffix = ".db";
    constexpr size_t stamp_len = 15; // YYYYMMDD-HHMMSS
    std::map<std::string, std::vector<std::string>> by_kind; // kind -> names (sortable by their stamp)
    for (auto& n : names) {
        if (n.size() < prefix.size() + 1 + stamp_len + suffix.size() + 1 || n.compare(0, prefix.size(), prefix) ||
            n.compare(n.size() - suffix.size(), suffix.size(), suffix))
            continue;
        const size_t stamp_at = n.size() - suffix.size() - stamp_len;
        if (n[stamp_at - 1] != '-' || n[stamp_at + 8] != '-') continue;
        by_kind[n.substr(prefix.size(), stamp_at - 1 - prefix.size())].push_back(n);
    }
    std::vector<std::string> out;
    keep = (std::max)(keep, size_t{1});
    for (auto& [kind, list] : by_kind) {
        if (list.size() <= keep) continue;
        std::sort(list.begin(), list.end()); // same kind and prefix: name order is time order
        out.insert(out.end(), list.begin(), list.end() - keep);
    }
    return out;
}

} // namespace upc
