#pragma once
// The play statistics database: an SQLite file with an in-memory copy for fast field lookups.
// No foobar2000 dependency. Thread-safe.
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace upc {

struct record {
    int64_t first_played = 0, last_played = 0, added = 0; // unix time, 0 = never
    uint32_t playcount = 0;
    uint32_t rating = 0; // 0 = none, 1..5
    bool loved = false;
    // Readable label, refreshed on every write, so a cleanup tool can show records whose track is gone.
    std::string artist, title, album, path;
    uint32_t subsong = 0; // with path: which track of a multi-track file (cue sheet)
};

// One loved track as Last.fm lists it.
struct lastfm_row {
    std::string artist, track, album, mbid;
    int64_t at = 0; // unix time, 0 = not given
};

// For comparing names: ASCII upper case folded, spaces collapsed. Nothing cleverer.
std::string match_key(const std::string& artist, const std::string& title);

// What Last.fm itself reports for one track of the user.
struct lastfm_track {
    uint32_t playcount = 0;
    bool loved = false;
    int64_t fetched_at = 0;     // unix time of the request
    std::vector<int64_t> times; // dates of the plays, oldest first; can be fewer than playcount
};

class store {
public:
    store() = default;
    store(const store&) = delete;
    store& operator=(const store&) = delete;
    ~store() { close(); }

    // Opens or creates the database and loads it. Throws std::runtime_error on failure.
    void open(const std::string& utf8_path);
    void close();
    bool is_open() const;

    std::optional<record> get(const std::string& key) const;
    // Numbers only, labels left empty: cheap enough for display fields evaluated per row.
    std::optional<record> get_stats(const std::string& key) const;
    // Writes `recs`, then moves play history, then removes, all in one transaction. Throws std::runtime_error on
    // failure.
    // `moves` (old key, new key): the history of the old record goes to the new one (moved if the old record is
    // removed, copied if it stays). History of removed records that aren't moved is deleted.
    // `new_plays` (key, unix time): plays to add to the history, in the same transaction as the record.
    // A key in both `recs` and `removes` is kept.
    void put_many(const std::vector<std::pair<std::string, record>>& recs, const std::vector<std::string>& removes = {},
                  const std::vector<std::pair<std::string, std::string>>& moves = {},
                  const std::vector<std::pair<std::string, int64_t>>& new_plays = {},
                  const std::vector<std::pair<std::string, int64_t>>& new_skips = {});
    // Writes `rec` unless the key already has a record, as one step: safe against another thread writing the same
    // key at the same time. Throws std::runtime_error on failure.
    void add_if_missing(const std::string& key, const record& rec);
    // Unix times of every logged play of the record, oldest first. Empty if none.
    std::vector<int64_t> play_times(const std::string& key) const;
    // Skips (a track left before it counted as played): how many, and when the last one was (0 = never).
    size_t skip_count(const std::string& key) const;
    int64_t last_skip(const std::string& key) const;
    // Plays at or after `since` (unix time).
    size_t plays_since(const std::string& key, int64_t since) const;

    // Last.fm's own numbers for a track, kept apart from the plays counted here. The record of `key` must exist.
    // Replaces what was stored for the track. Throws std::runtime_error on failure.
    void put_lastfm_track(const std::string& key, const lastfm_track& t) { put_lastfm_tracks({{key, t}}); }
    // Several at once, in one transaction.
    void put_lastfm_tracks(const std::vector<std::pair<std::string, lastfm_track>>& tracks);
    std::optional<lastfm_track> get_lastfm_track(const std::string& key) const;
    // Same without the dates, cheap enough for display fields evaluated per row.
    std::optional<lastfm_track> get_lastfm_summary(const std::string& key) const;
    void forget_lastfm(const std::vector<std::string>& keys);
    size_t lastfm_track_count() const;

    // ListenBrainz offers no per-track number, so its listens are kept as they come (`album` = release name) and
    // tied to a track by MusicBrainz recording id or by exact artist and title (match_key). Apart from the local
    // plays and from Last.fm. Returns how many of `rows` were new (same name key and time = already there).
    size_t add_listens(const std::vector<lastfm_row>& rows);
    size_t count_new_listens(const std::vector<lastfm_row>& rows) const;
    // Unix times of the listens that belong to a track: those under any of its match keys and those carrying its
    // recording id. Oldest first, a listen found twice is counted once.
    std::vector<int64_t> listen_times(const std::vector<std::string>& mkeys, const std::string& mbid = {}) const;
    int64_t listens_latest() const; // 0 = nothing stored
    size_t listens_total() const;
    std::vector<std::pair<std::string, record>> all() const;
    // Small named values kept with the data (e.g. the key pattern the records were made with).
    std::string get_meta(const std::string& name);
    void set_meta(const std::string& name, const std::string& value);
    size_t size() const;
    // Consistent copy of the database to a new file. Throws std::runtime_error.
    void backup_to(const std::string& utf8_path);

private:
    void exec(const char* sql);
    void write_row(sqlite3_stmt* insert, const std::string& key, const record& rec);
    void load_plays();
    void load_skips();
    void load_lastfm();
    void load_listens();

    mutable std::mutex m_lock;
    sqlite3* m_db = nullptr;
    std::unordered_map<std::string, record> m_records;
    std::unordered_map<std::string, std::vector<int64_t>> m_plays; // by record key, each sorted
    std::unordered_map<std::string, std::vector<int64_t>> m_skips; // by record key, each sorted
    std::unordered_map<std::string, lastfm_track> m_lastfm; // by record key
    std::unordered_map<std::string, std::vector<int64_t>> m_listens;      // ListenBrainz, by match key, each sorted
    std::unordered_map<std::string, std::vector<int64_t>> m_listens_mbid; // by recording id, each sorted
    size_t m_listens_total = 0;
    int64_t m_listens_latest = 0;
};

} // namespace upc
