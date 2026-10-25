#include "lastfm_core.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "../third_party/sqlite/sqlite3.h"

namespace upc {

namespace {

// SQLite already ships a JSON reader, so the replies are read with it instead of adding a JSON library.
struct statement {
    sqlite3_stmt* s = nullptr;
    statement(sqlite3* db, const std::string& sql) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) s = nullptr;
    }
    ~statement() { sqlite3_finalize(s); }
};

struct memory_db {
    sqlite3* db = nullptr;
    memory_db() {
        if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
            sqlite3_close(db);
            db = nullptr;
        }
    }
    ~memory_db() { sqlite3_close(db); }
};

const char unreadable[] = "Last.fm sent a reply that can't be read";

std::string text(sqlite3_stmt* s, int col) {
    const auto* p = sqlite3_column_text(s, col);
    return p ? reinterpret_cast<const char*>(p) : "";
}

std::string percent_encode(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            sprintf_s(buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// Last.fm's error reply: {"error":6,"message":"..."}. False if `json` isn't one.
bool read_error(sqlite3* db, const std::string& json, int& code, std::string& message, bool& valid) {
    statement q(db, "SELECT json_valid(?1), json_extract(?1,'$.error'), json_extract(?1,'$.message')");
    valid = false;
    if (!q.s) return false;
    sqlite3_bind_text(q.s, 1, json.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q.s) != SQLITE_ROW || !sqlite3_column_int(q.s, 0)) return false;
    valid = true;
    if (sqlite3_column_type(q.s, 1) == SQLITE_NULL) return false;
    code = sqlite3_column_int(q.s, 1);
    message = text(q.s, 2);
    if (message.empty()) message = "Last.fm error " + std::to_string(code);
    return true;
}

// `root`: "lovedtracks" or "trackscrobbles". `artist_path`: where the artist's name sits in one track.
track_page parse_tracks(const std::string& json, const std::string& root, const std::string& artist_path,
                        bool dated_only) {
    track_page out;
    const std::string list = "$." + root + ".track";
    memory_db mem;
    if (!mem.db) {
        out.error = unreadable;
        return out;
    }
    bool valid = false;
    if (read_error(mem.db, json, out.error_code, out.error, valid)) return out;
    if (!valid) {
        out.error = unreadable;
        return out;
    }
    statement head(mem.db, "SELECT json_extract(?1,'$." + root + ".\"@attr\".totalPages'),"
                           "json_extract(?1,'$." + root + ".\"@attr\".total'), json_type(?1,'" + list + "')");
    if (!head.s) {
        out.error = unreadable;
        return out;
    }
    sqlite3_bind_text(head.s, 1, json.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(head.s) != SQLITE_ROW) {
        out.error = unreadable;
        return out;
    }
    out.pages = (std::max)(1, sqlite3_column_int(head.s, 0));
    out.total = sqlite3_column_int(head.s, 1);
    if (sqlite3_column_type(head.s, 2) == SQLITE_NULL) {
        // no track list: fine for a user without any, anything else is not a reply we know
        if (sqlite3_column_type(head.s, 0) == SQLITE_NULL) out.error = unreadable;
        return out;
    }
    // One track comes as an object, several as an array.
    const bool one = text(head.s, 2) == "object";
    const std::string source = one ? "json_array(json(json_extract(?1,'" + list + "')))" : "?1,'" + list + "'";
    statement rows(mem.db, "SELECT json_extract(value,'" + artist_path + "'), json_extract(value,'$.name'),"
                           "json_extract(value,'$.mbid'), json_extract(value,'$.date.uts') FROM json_each(" + source + ")");
    if (!rows.s) {
        out.error = unreadable;
        return out;
    }
    sqlite3_bind_text(rows.s, 1, json.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(rows.s) == SQLITE_ROW) {
        lastfm_row r;
        r.at = sqlite3_column_int64(rows.s, 3);
        if (dated_only && r.at <= 0) continue; // playing right now: no date yet
        r.artist = text(rows.s, 0);
        r.track = text(rows.s, 1);
        r.mbid = text(rows.s, 2);
        out.items.push_back(std::move(r));
    }
    return out;
}

} // namespace

track_info parse_track_info(const std::string& json) {
    track_info out;
    memory_db mem;
    if (!mem.db) {
        out.error = unreadable;
        return out;
    }
    bool valid = false;
    if (read_error(mem.db, json, out.error_code, out.error, valid)) {
        if (out.error_code == 6) { // Last.fm doesn't know this track
            out.found = false;
            out.error_code = 0;
            out.error.clear();
        }
        return out;
    }
    statement q(mem.db, "SELECT json_type(?1,'$.track'), json_extract(?1,'$.track.userplaycount'),"
                        "json_extract(?1,'$.track.userloved')");
    if (!valid || !q.s) {
        out.error = unreadable;
        return out;
    }
    sqlite3_bind_text(q.s, 1, json.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(q.s) != SQLITE_ROW || sqlite3_column_type(q.s, 0) == SQLITE_NULL) {
        out.error = unreadable;
        return out;
    }
    out.playcount = static_cast<uint32_t>((std::max)(0, sqlite3_column_int(q.s, 1)));
    out.loved = sqlite3_column_int(q.s, 2) != 0;
    return out;
}

track_page parse_track_scrobbles(const std::string& json) {
    return parse_tracks(json, "trackscrobbles", "$.artist.\"#text\"", true);
}

track_page parse_loved_tracks(const std::string& json) {
    return parse_tracks(json, "lovedtracks", "$.artist.name", false);
}

bool is_temporary_error(int error_code) {
    return error_code == 8 || error_code == 11 || error_code == 16 || error_code == 29;
}

bool is_fatal_error(int error_code) { return error_code == 4 || error_code == 9 || error_code == 10 || error_code == 26; }

std::string track_info_url(const std::string& user, const std::string& api_key, const std::string& artist,
                           const std::string& track, bool autocorrect) {
    return std::string("https://ws.audioscrobbler.com/2.0/?method=track.getinfo&format=json&autocorrect=") +
           (autocorrect ? "1" : "0") + "&username=" + percent_encode(user) + "&api_key=" + percent_encode(api_key) + "&artist=" + percent_encode(artist) +
           "&track=" + percent_encode(track);
}

std::string track_scrobbles_url(const std::string& user, const std::string& api_key, const std::string& artist,
                                const std::string& track, int page, bool autocorrect) {
    return std::string("https://ws.audioscrobbler.com/2.0/?method=user.gettrackscrobbles&format=json&autocorrect=") +
           (autocorrect ? "1" : "0") + "&limit=200&user=" + percent_encode(user) + "&api_key=" + percent_encode(api_key) + "&artist=" + percent_encode(artist) +
           "&track=" + percent_encode(track) + "&page=" + std::to_string(page);
}

std::string loved_tracks_url(const std::string& user, const std::string& api_key, int page) {
    return "https://ws.audioscrobbler.com/2.0/?method=user.getlovedtracks&format=json&limit=200&user=" +
           percent_encode(user) + "&api_key=" + percent_encode(api_key) + "&page=" + std::to_string(page);
}

} // namespace upc
