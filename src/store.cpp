#include "store.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <unordered_set>

#include "../third_party/sqlite/sqlite3.h"

namespace upc {

namespace {

struct statement {
    sqlite3_stmt* s = nullptr;
    statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("SQLite prepare failed: ") + sqlite3_errmsg(db));
    }
    ~statement() { sqlite3_finalize(s); }
};

std::string column_text(sqlite3_stmt* s, int col) {
    const auto* p = sqlite3_column_text(s, col);
    return p ? reinterpret_cast<const char*>(p) : "";
}

} // namespace

void store::open(const std::string& utf8_path) {
    std::lock_guard lock(m_lock);
    if (m_db) throw std::runtime_error("database already open");
    if (sqlite3_open_v2(utf8_path.c_str(), &m_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        std::string err = m_db ? sqlite3_errmsg(m_db) : "out of memory";
        sqlite3_close(m_db);
        m_db = nullptr;
        throw std::runtime_error("cannot open " + utf8_path + ": " + err);
    }
    try {
        sqlite3_busy_timeout(m_db, 2000);
        // Rollback journal, not WAL: no -wal / -shm files next to the database, so the .db alone is always the whole
        // database. Reads come from the in-memory copy, so WAL's concurrent reads would buy nothing. FULL: in this
        // mode NORMAL could corrupt the file on a power loss.
        exec("PRAGMA journal_mode=DELETE");
        exec("PRAGMA synchronous=FULL");
        exec("CREATE TABLE IF NOT EXISTS records ("
             "id INTEGER PRIMARY KEY,"
             "key TEXT NOT NULL UNIQUE,"
             "first_played INTEGER NOT NULL DEFAULT 0,"
             "last_played INTEGER NOT NULL DEFAULT 0,"
             "added INTEGER NOT NULL DEFAULT 0,"
             "playcount INTEGER NOT NULL DEFAULT 0,"
             "rating INTEGER NOT NULL DEFAULT 0,"
             "loved INTEGER NOT NULL DEFAULT 0,"
             "artist TEXT NOT NULL DEFAULT '',"
             "title TEXT NOT NULL DEFAULT '',"
             "album TEXT NOT NULL DEFAULT '',"
             "path TEXT NOT NULL DEFAULT '',"
             "subsong INTEGER NOT NULL DEFAULT 0)");
        exec("CREATE TABLE IF NOT EXISTS meta (name TEXT PRIMARY KEY NOT NULL, value TEXT NOT NULL)");
        int version = 0;
        {
            statement v(m_db, "PRAGMA user_version");
            if (sqlite3_step(v.s) == SQLITE_ROW) version = sqlite3_column_int(v.s, 0);
        }
        if (version == 1) exec("ALTER TABLE records ADD COLUMN subsong INTEGER NOT NULL DEFAULT 0");
        if (version < 3) {
            bool has_id = false;
            {
                statement c(m_db, "SELECT COUNT(*) FROM pragma_table_info('records') WHERE name = 'id'");
                has_id = sqlite3_step(c.s) == SQLITE_ROW && sqlite3_column_int(c.s, 0) > 0;
            }
            if (!has_id) {
                // Older databases keyed rows by the text key alone. History tables refer to a record by this
                // integer id, so a re-key or tag edit never has to rewrite them.
                bool has_rows = false;
                {
                    statement c(m_db, "SELECT 1 FROM records LIMIT 1");
                    has_rows = sqlite3_step(c.s) == SQLITE_ROW;
                }
                if (has_rows) {
                    const std::string copy = utf8_path + "-before-ids.bak";
                    std::remove(copy.c_str());
                    statement b(m_db, "VACUUM INTO ?");
                    sqlite3_bind_text(b.s, 1, copy.c_str(), -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(b.s) != SQLITE_DONE)
                        throw std::runtime_error(std::string("backup before upgrade failed: ") + sqlite3_errmsg(m_db));
                }
                exec("BEGIN");
                try {
                    exec("CREATE TABLE records_new (id INTEGER PRIMARY KEY, key TEXT NOT NULL UNIQUE,"
                         "first_played INTEGER NOT NULL DEFAULT 0, last_played INTEGER NOT NULL DEFAULT 0,"
                         "added INTEGER NOT NULL DEFAULT 0, playcount INTEGER NOT NULL DEFAULT 0,"
                         "rating INTEGER NOT NULL DEFAULT 0, loved INTEGER NOT NULL DEFAULT 0,"
                         "artist TEXT NOT NULL DEFAULT '', title TEXT NOT NULL DEFAULT '',"
                         "album TEXT NOT NULL DEFAULT '', path TEXT NOT NULL DEFAULT '',"
                         "subsong INTEGER NOT NULL DEFAULT 0)");
                    exec("INSERT INTO records_new (key,first_played,last_played,added,playcount,rating,loved,artist,"
                         "title,album,path,subsong) SELECT key,first_played,last_played,added,playcount,rating,loved,"
                         "artist,title,album,path,subsong FROM records");
                    exec("DROP TABLE records");
                    exec("ALTER TABLE records_new RENAME TO records");
                    exec("COMMIT");
                } catch (...) {
                    sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
                    throw;
                }
            }
            exec("PRAGMA user_version=3");
        }
        // One row per logged play. Refers to the record by id, so a re-key never rewrites history.
        exec("CREATE TABLE IF NOT EXISTS plays (rec INTEGER NOT NULL, played_at INTEGER NOT NULL)");
        exec("CREATE INDEX IF NOT EXISTS plays_by_rec ON plays (rec, played_at)");
        exec("CREATE TABLE IF NOT EXISTS skips (rec INTEGER NOT NULL, skipped_at INTEGER NOT NULL)");
        exec("CREATE INDEX IF NOT EXISTS skips_by_rec ON skips (rec, skipped_at)");
        exec("DROP TABLE IF EXISTS lastfm_plays"); // the history download of 0.6 / 0.7, replaced by per-track numbers
        exec("CREATE TABLE IF NOT EXISTS lastfm_tracks (rec INTEGER PRIMARY KEY, playcount INTEGER NOT NULL,"
             "loved INTEGER NOT NULL DEFAULT 0, fetched_at INTEGER NOT NULL)");
        exec("CREATE TABLE IF NOT EXISTS listens (mkey TEXT NOT NULL, played_at INTEGER NOT NULL,"
             "artist TEXT NOT NULL, track TEXT NOT NULL, release TEXT NOT NULL DEFAULT '', mbid TEXT NOT NULL DEFAULT '',"
             "PRIMARY KEY (mkey, played_at)) WITHOUT ROWID");
        exec("CREATE TABLE IF NOT EXISTS lastfm_track_plays (rec INTEGER NOT NULL, played_at INTEGER NOT NULL,"
             "PRIMARY KEY (rec, played_at)) WITHOUT ROWID");

        statement q(m_db, "SELECT key,first_played,last_played,added,playcount,rating,loved,artist,title,album,path,"
                          "subsong FROM records");
        int rc;
        while ((rc = sqlite3_step(q.s)) == SQLITE_ROW) {
            record r;
            r.first_played = sqlite3_column_int64(q.s, 1);
            r.last_played = sqlite3_column_int64(q.s, 2);
            r.added = sqlite3_column_int64(q.s, 3);
            r.playcount = static_cast<uint32_t>(sqlite3_column_int64(q.s, 4));
            r.rating = static_cast<uint32_t>(sqlite3_column_int64(q.s, 5));
            r.loved = sqlite3_column_int(q.s, 6) != 0;
            r.artist = column_text(q.s, 7);
            r.title = column_text(q.s, 8);
            r.album = column_text(q.s, 9);
            r.path = column_text(q.s, 10);
            r.subsong = static_cast<uint32_t>(sqlite3_column_int64(q.s, 11));
            m_records.emplace(column_text(q.s, 0), std::move(r));
        }
        if (rc != SQLITE_DONE) throw std::runtime_error(std::string("SQLite read failed: ") + sqlite3_errmsg(m_db));
        load_plays();
        load_skips();
        load_lastfm();
        load_listens();
    } catch (...) {
        sqlite3_close(m_db);
        m_db = nullptr;
        m_records.clear();
        m_plays.clear();
        m_skips.clear();
        m_lastfm.clear();
        m_listens.clear();
        m_listens_mbid.clear();
        throw;
    }
}

void store::close() {
    std::lock_guard lock(m_lock);
    if (m_db) sqlite3_close(m_db);
    m_db = nullptr;
    m_records.clear();
    m_plays.clear();
    m_skips.clear();
    m_lastfm.clear();
    m_listens.clear();
    m_listens_mbid.clear();
    m_listens_total = 0;
    m_listens_latest = 0;
}

bool store::is_open() const {
    std::lock_guard lock(m_lock);
    return m_db != nullptr;
}

std::optional<record> store::get(const std::string& key) const {
    std::lock_guard lock(m_lock);
    auto it = m_records.find(key);
    if (it == m_records.end()) return std::nullopt;
    return it->second;
}

std::optional<record> store::get_stats(const std::string& key) const {
    std::lock_guard lock(m_lock);
    auto it = m_records.find(key);
    if (it == m_records.end()) return std::nullopt;
    const record& r = it->second;
    record out;
    out.first_played = r.first_played;
    out.last_played = r.last_played;
    out.added = r.added;
    out.playcount = r.playcount;
    out.rating = r.rating;
    out.loved = r.loved;
    return out;
}

size_t store::size() const {
    std::lock_guard lock(m_lock);
    return m_records.size();
}


void store::put_many(const std::vector<std::pair<std::string, record>>& recs, const std::vector<std::string>& removes,
                     const std::vector<std::pair<std::string, std::string>>& moves,
                     const std::vector<std::pair<std::string, int64_t>>& new_plays,
                     const std::vector<std::pair<std::string, int64_t>>& new_skips) {
    std::lock_guard lock(m_lock);
    std::unordered_set<std::string> put_keys;
    for (auto& r : recs) put_keys.insert(r.first);
    std::unordered_set<std::string> gone;
    for (auto& key : removes)
        if (!put_keys.count(key)) gone.insert(key);

    auto step = [&](sqlite3_stmt* q) {
        if (sqlite3_step(q) != SQLITE_DONE)
            throw std::runtime_error(std::string("SQLite write failed: ") + sqlite3_errmsg(m_db));
        sqlite3_reset(q);
    };
    exec("BEGIN");
    try {
        // Prepared once per batch: a library-sized batch is tens of thousands of rows.
        // An upsert, not INSERT OR REPLACE: a replace would delete the row and give it a new id.
        statement ins(m_db, "INSERT INTO records "
                            "(key,first_played,last_played,added,playcount,rating,loved,artist,title,album,path,subsong) "
                            "VALUES (?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(key) DO UPDATE SET "
                            "first_played=excluded.first_played,last_played=excluded.last_played,"
                            "added=excluded.added,playcount=excluded.playcount,rating=excluded.rating,"
                            "loved=excluded.loved,artist=excluded.artist,title=excluded.title,"
                            "album=excluded.album,path=excluded.path,subsong=excluded.subsong");
        for (auto& [key, rec] : recs) write_row(ins.s, key, rec);

        if (!moves.empty()) {
            // ?1 = old key, ?2 = new key. Only when the new record exists.
            statement move(m_db, "UPDATE plays SET rec = (SELECT id FROM records WHERE key = ?2) "
                                 "WHERE rec = (SELECT id FROM records WHERE key = ?1) "
                                 "AND EXISTS (SELECT 1 FROM records WHERE key = ?2)");
            statement copy(m_db, "INSERT INTO plays (rec, played_at) "
                                 "SELECT (SELECT id FROM records WHERE key = ?2), played_at FROM plays "
                                 "WHERE rec = (SELECT id FROM records WHERE key = ?1) "
                                 "AND EXISTS (SELECT 1 FROM records WHERE key = ?2)");
            statement move_lf(m_db, "UPDATE OR IGNORE lastfm_tracks SET rec = (SELECT id FROM records WHERE key = ?2) "
                                    "WHERE rec = (SELECT id FROM records WHERE key = ?1) "
                                    "AND EXISTS (SELECT 1 FROM records WHERE key = ?2)");
            statement move_lf_plays(m_db, "UPDATE OR IGNORE lastfm_track_plays SET rec = (SELECT id FROM records "
                                          "WHERE key = ?2) WHERE rec = (SELECT id FROM records WHERE key = ?1) "
                                          "AND EXISTS (SELECT 1 FROM records WHERE key = ?2)");
            statement move_sk(m_db, "UPDATE skips SET rec = (SELECT id FROM records WHERE key = ?2) "
                                    "WHERE rec = (SELECT id FROM records WHERE key = ?1) "
                                    "AND EXISTS (SELECT 1 FROM records WHERE key = ?2)");
            statement copy_sk(m_db, "INSERT INTO skips (rec, skipped_at) "
                                    "SELECT (SELECT id FROM records WHERE key = ?2), skipped_at FROM skips "
                                    "WHERE rec = (SELECT id FROM records WHERE key = ?1) "
                                    "AND EXISTS (SELECT 1 FROM records WHERE key = ?2)");
            for (auto& [from, to] : moves) {
                if (from == to || to.empty()) continue;
                const bool moving = gone.count(from) > 0; // the old record goes away: its data moves; else it is copied
                for (sqlite3_stmt* q : {move.s, move_lf.s, move_lf_plays.s, move_sk.s, copy.s, copy_sk.s}) {
                    if ((q == copy.s || q == copy_sk.s) == moving) continue;
                    // a copy leaves the Last.fm numbers with the old record
                    if (!moving && (q == move_lf.s || q == move_lf_plays.s)) continue;
                    sqlite3_bind_text(q, 1, from.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(q, 2, to.c_str(), -1, SQLITE_TRANSIENT);
                    step(q);
                }
            }
        }
        statement del_plays(m_db, "DELETE FROM plays WHERE rec = (SELECT id FROM records WHERE key = ?1)");
        statement del_skips(m_db, "DELETE FROM skips WHERE rec = (SELECT id FROM records WHERE key = ?1)");
        statement del_lf_plays(m_db, "DELETE FROM lastfm_track_plays WHERE rec = (SELECT id FROM records WHERE key = ?1)");
        statement del_lf(m_db, "DELETE FROM lastfm_tracks WHERE rec = (SELECT id FROM records WHERE key = ?1)");
        statement del(m_db, "DELETE FROM records WHERE key = ?1");
        for (auto& key : gone) {
            for (auto* q : {del_plays.s, del_skips.s, del_lf_plays.s, del_lf.s}) {
                sqlite3_bind_text(q, 1, key.c_str(), -1, SQLITE_TRANSIENT);
                step(q);
            }
            sqlite3_bind_text(del.s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
            step(del.s);
        }
        statement add_play(m_db, "INSERT INTO plays (rec, played_at) SELECT id, ?2 FROM records WHERE key = ?1");
        for (auto& [key, at] : new_plays) {
            sqlite3_bind_text(add_play.s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(add_play.s, 2, at);
            step(add_play.s);
        }
        statement add_skip(m_db, "INSERT INTO skips (rec, skipped_at) SELECT id, ?2 FROM records WHERE key = ?1");
        for (auto& [key, at] : new_skips) {
            sqlite3_bind_text(add_skip.s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(add_skip.s, 2, at);
            step(add_skip.s);
        }
        exec("COMMIT");
    } catch (...) {
        sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    for (auto& key : gone) m_records.erase(key);
    for (auto& [key, rec] : recs) m_records[key] = rec;
    if (!moves.empty()) {
        load_plays(); // rare: a re-key or a merge
        load_skips();
        load_lastfm();
        return;
    }
    for (auto& key : gone) {
        m_plays.erase(key);
        m_skips.erase(key);
        m_lastfm.erase(key);
    }
    for (auto& [key, at] : new_skips) {
        auto& times = m_skips[key];
        times.insert(std::upper_bound(times.begin(), times.end(), at), at);
    }
    for (auto& [key, at] : new_plays) {
        auto& times = m_plays[key];
        times.insert(std::upper_bound(times.begin(), times.end(), at), at);
    }
}

void store::add_if_missing(const std::string& key, const record& rec) {
    std::lock_guard lock(m_lock);
    if (m_records.count(key)) return;
    if (!m_db) throw std::runtime_error("database not open");
    statement ins(m_db, "INSERT OR IGNORE INTO records "
                        "(key,first_played,last_played,added,playcount,rating,loved,artist,title,album,path,subsong) "
                        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
    write_row(ins.s, key, rec);
    m_records.emplace(key, rec);
}

void store::load_skips() {
    m_skips.clear();
    statement q(m_db, "SELECT r.key, s.skipped_at FROM skips s JOIN records r ON r.id = s.rec ORDER BY r.key, s.skipped_at");
    int rc;
    while ((rc = sqlite3_step(q.s)) == SQLITE_ROW) m_skips[column_text(q.s, 0)].push_back(sqlite3_column_int64(q.s, 1));
    if (rc != SQLITE_DONE) throw std::runtime_error(std::string("SQLite read failed: ") + sqlite3_errmsg(m_db));
}

size_t store::skip_count(const std::string& key) const {
    std::lock_guard lock(m_lock);
    auto it = m_skips.find(key);
    return it == m_skips.end() ? 0 : it->second.size();
}

int64_t store::last_skip(const std::string& key) const {
    std::lock_guard lock(m_lock);
    auto it = m_skips.find(key);
    return it == m_skips.end() || it->second.empty() ? 0 : it->second.back();
}

void store::load_plays() {
    m_plays.clear();
    statement q(m_db, "SELECT r.key, p.played_at FROM plays p JOIN records r ON r.id = p.rec ORDER BY r.key, p.played_at");
    int rc;
    while ((rc = sqlite3_step(q.s)) == SQLITE_ROW) m_plays[column_text(q.s, 0)].push_back(sqlite3_column_int64(q.s, 1));
    if (rc != SQLITE_DONE) throw std::runtime_error(std::string("SQLite read failed: ") + sqlite3_errmsg(m_db));
}

void store::load_lastfm() {
    m_lastfm.clear();
    {
        statement q(m_db, "SELECT r.key, t.playcount, t.loved, t.fetched_at FROM lastfm_tracks t "
                          "JOIN records r ON r.id = t.rec");
        while (sqlite3_step(q.s) == SQLITE_ROW) {
            lastfm_track t;
            t.playcount = static_cast<uint32_t>(sqlite3_column_int64(q.s, 1));
            t.loved = sqlite3_column_int(q.s, 2) != 0;
            t.fetched_at = sqlite3_column_int64(q.s, 3);
            m_lastfm.emplace(column_text(q.s, 0), std::move(t));
        }
    }
    statement q(m_db, "SELECT r.key, p.played_at FROM lastfm_track_plays p JOIN records r ON r.id = p.rec "
                      "ORDER BY r.key, p.played_at");
    int rc;
    while ((rc = sqlite3_step(q.s)) == SQLITE_ROW)
        if (auto it = m_lastfm.find(column_text(q.s, 0)); it != m_lastfm.end())
            it->second.times.push_back(sqlite3_column_int64(q.s, 1));
    if (rc != SQLITE_DONE) throw std::runtime_error(std::string("SQLite read failed: ") + sqlite3_errmsg(m_db));
}

namespace {

std::string fold_name(const std::string& in) {
    std::string s;
    bool space = true;
    for (unsigned char c : in) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            space = true;
            continue;
        }
        if (space && !s.empty()) s += ' ';
        space = false;
        s += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    return s;
}

} // namespace

std::string match_key(const std::string& artist, const std::string& title) {
    return fold_name(artist) + '\x1f' + fold_name(title);
}

void store::put_lastfm_tracks(const std::vector<std::pair<std::string, lastfm_track>>& tracks) {
    std::lock_guard lock(m_lock);
    auto step = [&](sqlite3_stmt* q) {
        if (sqlite3_step(q) != SQLITE_DONE)
            throw std::runtime_error(std::string("SQLite write failed: ") + sqlite3_errmsg(m_db));
        sqlite3_reset(q);
    };
    exec("BEGIN");
    try {
        statement id(m_db, "SELECT id FROM records WHERE key = ?");
        statement ins(m_db, "INSERT OR REPLACE INTO lastfm_tracks (rec,playcount,loved,fetched_at) VALUES (?,?,?,?)");
        statement del(m_db, "DELETE FROM lastfm_track_plays WHERE rec = ?");
        statement add(m_db, "INSERT OR IGNORE INTO lastfm_track_plays (rec, played_at) VALUES (?, ?)");
        for (const auto& [key, t] : tracks) {
            sqlite3_bind_text(id.s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
            const int64_t rec = sqlite3_step(id.s) == SQLITE_ROW ? sqlite3_column_int64(id.s, 0) : -1;
            sqlite3_reset(id.s);
            if (rec < 0) throw std::runtime_error("no record for this track");
            sqlite3_bind_int64(ins.s, 1, rec);
            sqlite3_bind_int64(ins.s, 2, t.playcount);
            sqlite3_bind_int(ins.s, 3, t.loved ? 1 : 0);
            sqlite3_bind_int64(ins.s, 4, t.fetched_at);
            step(ins.s);
            sqlite3_bind_int64(del.s, 1, rec);
            step(del.s);
            for (const auto at : t.times) {
                sqlite3_bind_int64(add.s, 1, rec);
                sqlite3_bind_int64(add.s, 2, at);
                step(add.s);
            }
        }
        exec("COMMIT");
    } catch (...) {
        sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    for (const auto& [key, t] : tracks) {
        auto copy = t;
        std::sort(copy.times.begin(), copy.times.end());
        copy.times.erase(std::unique(copy.times.begin(), copy.times.end()), copy.times.end());
        m_lastfm[key] = std::move(copy);
    }
}

std::optional<lastfm_track> store::get_lastfm_track(const std::string& key) const {
    std::lock_guard lock(m_lock);
    auto it = m_lastfm.find(key);
    if (it == m_lastfm.end()) return std::nullopt;
    return it->second;
}

std::optional<lastfm_track> store::get_lastfm_summary(const std::string& key) const {
    std::lock_guard lock(m_lock);
    auto it = m_lastfm.find(key);
    if (it == m_lastfm.end()) return std::nullopt;
    lastfm_track out;
    out.playcount = it->second.playcount;
    out.loved = it->second.loved;
    out.fetched_at = it->second.fetched_at;
    return out;
}

void store::forget_lastfm(const std::vector<std::string>& keys) {
    std::lock_guard lock(m_lock);
    exec("BEGIN");
    try {
        statement a(m_db, "DELETE FROM lastfm_track_plays WHERE rec = (SELECT id FROM records WHERE key = ?1)");
        statement b(m_db, "DELETE FROM lastfm_tracks WHERE rec = (SELECT id FROM records WHERE key = ?1)");
        for (auto& key : keys)
            for (auto* q : {a.s, b.s}) {
                sqlite3_bind_text(q, 1, key.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(q) != SQLITE_DONE)
                    throw std::runtime_error(std::string("SQLite write failed: ") + sqlite3_errmsg(m_db));
                sqlite3_reset(q);
            }
        exec("COMMIT");
    } catch (...) {
        sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    for (auto& key : keys) m_lastfm.erase(key);
}

void store::load_listens() {
    m_listens.clear();
    m_listens_mbid.clear();
    m_listens_total = 0;
    m_listens_latest = 0;
    statement q(m_db, "SELECT mkey, played_at, mbid FROM listens ORDER BY mkey, played_at");
    int rc;
    while ((rc = sqlite3_step(q.s)) == SQLITE_ROW) {
        const int64_t at = sqlite3_column_int64(q.s, 1);
        m_listens[column_text(q.s, 0)].push_back(at);
        if (const auto mbid = column_text(q.s, 2); !mbid.empty()) m_listens_mbid[mbid].push_back(at);
        ++m_listens_total;
        m_listens_latest = (std::max)(m_listens_latest, at);
    }
    if (rc != SQLITE_DONE) throw std::runtime_error(std::string("SQLite read failed: ") + sqlite3_errmsg(m_db));
    for (auto& [mbid, times] : m_listens_mbid) std::sort(times.begin(), times.end());
}

size_t store::count_new_listens(const std::vector<lastfm_row>& rows) const {
    std::lock_guard lock(m_lock);
    std::unordered_map<std::string, std::unordered_set<int64_t>> seen; // also catches repeats within `rows`
    size_t fresh = 0;
    for (auto& r : rows) {
        const auto key = match_key(r.artist, r.track);
        auto it = m_listens.find(key);
        if (it != m_listens.end() && std::binary_search(it->second.begin(), it->second.end(), r.at)) continue;
        if (seen[key].insert(r.at).second) ++fresh;
    }
    return fresh;
}

size_t store::add_listens(const std::vector<lastfm_row>& rows) {
    std::lock_guard lock(m_lock);
    size_t fresh = 0;
    std::vector<std::pair<std::string, int64_t>> added, added_mbid;
    std::vector<std::string> mbids;
    exec("BEGIN");
    try {
        statement ins(m_db, "INSERT OR IGNORE INTO listens (mkey,played_at,artist,track,release,mbid) "
                            "VALUES (?,?,?,?,?,?)");
        for (auto& r : rows) {
            const auto key = match_key(r.artist, r.track);
            sqlite3_bind_text(ins.s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(ins.s, 2, r.at);
            sqlite3_bind_text(ins.s, 3, r.artist.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins.s, 4, r.track.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins.s, 5, r.album.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins.s, 6, r.mbid.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(ins.s) != SQLITE_DONE)
                throw std::runtime_error(std::string("SQLite write failed: ") + sqlite3_errmsg(m_db));
            sqlite3_reset(ins.s);
            if (sqlite3_changes(m_db) > 0) {
                ++fresh;
                added.emplace_back(key, r.at);
                if (!r.mbid.empty()) added_mbid.emplace_back(r.mbid, r.at);
            }
        }
        exec("COMMIT");
    } catch (...) {
        sqlite3_exec(m_db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
    for (auto& [key, at] : added) {
        auto& times = m_listens[key];
        times.insert(std::upper_bound(times.begin(), times.end(), at), at);
        m_listens_latest = (std::max)(m_listens_latest, at);
    }
    for (auto& [mbid, at] : added_mbid) {
        auto& times = m_listens_mbid[mbid];
        times.insert(std::upper_bound(times.begin(), times.end(), at), at);
    }
    m_listens_total += fresh;
    return fresh;
}

std::vector<int64_t> store::listen_times(const std::vector<std::string>& mkeys, const std::string& mbid) const {
    std::lock_guard lock(m_lock);
    std::vector<int64_t> out;
    for (auto& key : mkeys)
        if (auto it = m_listens.find(key); it != m_listens.end()) out.insert(out.end(), it->second.begin(), it->second.end());
    if (!mbid.empty())
        if (auto it = m_listens_mbid.find(mbid); it != m_listens_mbid.end())
            out.insert(out.end(), it->second.begin(), it->second.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

int64_t store::listens_latest() const {
    std::lock_guard lock(m_lock);
    return m_listens_latest;
}

size_t store::listens_total() const {
    std::lock_guard lock(m_lock);
    return m_listens_total;
}

size_t store::lastfm_track_count() const {
    std::lock_guard lock(m_lock);
    return m_lastfm.size();
}

std::vector<int64_t> store::play_times(const std::string& key) const {
    std::lock_guard lock(m_lock);
    auto it = m_plays.find(key);
    return it == m_plays.end() ? std::vector<int64_t>{} : it->second;
}

size_t store::plays_since(const std::string& key, int64_t since) const {
    std::lock_guard lock(m_lock);
    auto it = m_plays.find(key);
    if (it == m_plays.end()) return 0;
    return static_cast<size_t>(it->second.end() - std::lower_bound(it->second.begin(), it->second.end(), since));
}

std::vector<std::pair<std::string, record>> store::all() const {
    std::lock_guard lock(m_lock);
    return {m_records.begin(), m_records.end()};
}

std::string store::get_meta(const std::string& name) {
    std::lock_guard lock(m_lock);
    if (!m_db) throw std::runtime_error("database not open");
    statement q(m_db, "SELECT value FROM meta WHERE name = ?");
    sqlite3_bind_text(q.s, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(q.s) == SQLITE_ROW ? column_text(q.s, 0) : "";
}

void store::set_meta(const std::string& name, const std::string& value) {
    std::lock_guard lock(m_lock);
    if (!m_db) throw std::runtime_error("database not open");
    statement q(m_db, "INSERT OR REPLACE INTO meta (name, value) VALUES (?, ?)");
    sqlite3_bind_text(q.s, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q.s, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q.s) != SQLITE_DONE)
        throw std::runtime_error(std::string("SQLite write failed: ") + sqlite3_errmsg(m_db));
}

void store::backup_to(const std::string& utf8_path) {
    std::lock_guard lock(m_lock);
    if (!m_db) throw std::runtime_error("database not open");
    statement q(m_db, "VACUUM INTO ?");
    sqlite3_bind_text(q.s, 1, utf8_path.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q.s) != SQLITE_DONE)
        throw std::runtime_error(std::string("backup failed: ") + sqlite3_errmsg(m_db));
}

void store::exec(const char* sql) {
    if (!m_db) throw std::runtime_error("database not open");
    char* err = nullptr;
    if (sqlite3_exec(m_db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("SQLite: " + msg);
    }
}

void store::write_row(sqlite3_stmt* s, const std::string& key, const record& r) {
    sqlite3_bind_text(s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 2, r.first_played);
    sqlite3_bind_int64(s, 3, r.last_played);
    sqlite3_bind_int64(s, 4, r.added);
    sqlite3_bind_int64(s, 5, r.playcount);
    sqlite3_bind_int64(s, 6, r.rating);
    sqlite3_bind_int(s, 7, r.loved ? 1 : 0);
    sqlite3_bind_text(s, 8, r.artist.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 9, r.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 10, r.album.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 11, r.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 12, r.subsong);
    if (sqlite3_step(s) != SQLITE_DONE)
        throw std::runtime_error(std::string("SQLite write failed: ") + sqlite3_errmsg(m_db));
    sqlite3_reset(s);
}

} // namespace upc
