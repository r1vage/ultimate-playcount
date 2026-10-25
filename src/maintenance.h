#pragma once
// Re-keying and cleanup decisions. No foobar2000 dependency.
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "store.h"

namespace upc {

// Earlier of two unix times, where 0 means "never".
int64_t earliest(int64_t a, int64_t b);

// Combines records of plays that really happened separately (two old keys becoming one, or a track's data
// before and after a tag edit): play counts add up. Dates: earliest first played / added, latest last played.
record combine(record a, const record& b);

struct change_set {
    std::vector<std::pair<std::string, record>> puts;
    std::vector<std::string> removes;
};

// Moves records to new keys. `moves` holds (old key, new key) for every known track. Several old keys landing
// on one new key are combined; one old key splitting into several new keys is copied to each (it can't be
// split). Records with no known track keep their old key. Existing records under a new key are kept and
// combined with what arrives, unless that key is itself moving away.
change_set rekey(const std::vector<std::pair<std::string, record>>& records,
                 const std::vector<std::pair<std::string, std::string>>& moves);

struct location {
    std::string path; // as stored in records (native path)
    uint32_t subsong = 0;
    bool operator<(const location& o) const { return path != o.path ? path < o.path : subsong < o.subsong; }
};

struct cleanup_scan {
    // Records whose file is still a known track but under another key (its tags changed): old key -> current key.
    std::vector<std::pair<std::string, std::string>> reattach;
    // Records with no known track at all.
    std::vector<std::string> orphans;
};

// `live` = current key of every known track, by its location.
cleanup_scan scan_for_cleanup(const std::vector<std::pair<std::string, record>>& records,
                              const std::map<location, std::string>& live);

// Orphans to remove automatically: no rating or loved mark, no activity (last played / added) for `days`, and
// `file_gone` confirms the file is really gone. Returns nothing if that would be more than a quarter of all
// records: that looks like a missing drive, not deleted files.
std::vector<std::string> pick_auto_clean(const std::vector<std::pair<std::string, record>>& orphans,
                                         size_t total_records, int64_t now, int days,
                                         const std::function<bool(const record&)>& file_gone);

// Backup files are named "ultimate_playcount-<kind>-YYYYMMDD-HHMMSS.db" (kind: auto, before-import, ...).
// Returns the names to delete so that at most `keep` (at least 1) of each kind remain, the newest ones.
// Names that don't follow the pattern are never returned.
std::vector<std::string> backups_to_prune(const std::vector<std::string>& names, size_t keep);

} // namespace upc
