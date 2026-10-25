#include "listenbrainz_core.h"

#include <cctype>
#include <cstdio>

#include "../third_party/sqlite/sqlite3.h"

namespace upc {

namespace {

struct statement {
    sqlite3_stmt* s = nullptr;
    statement(sqlite3* db, const std::string& sql) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) s = nullptr;
    }
    ~statement() { sqlite3_finalize(s); }
};

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

} // namespace

listens_page parse_listens(const std::string& json) {
    listens_page out;
    const char unreadable[] = "ListenBrainz sent a reply that can't be read";
    sqlite3* db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        sqlite3_close(db);
        out.error = unreadable;
        return out;
    }
    {
        // An error reply looks like {"code":404,"error":"Cannot find user: x"}.
        statement head(db, "SELECT json_valid(?1), json_extract(?1,'$.error'), json_extract(?1,'$.code'),"
                           "json_type(?1,'$.payload.listens')");
        if (!head.s) {
            out.error = unreadable;
        } else {
            sqlite3_bind_text(head.s, 1, json.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(head.s) != SQLITE_ROW || !sqlite3_column_int(head.s, 0)) {
                out.error = unreadable;
            } else if (sqlite3_column_type(head.s, 1) != SQLITE_NULL) {
                out.error = text(head.s, 1);
                out.error_code = sqlite3_column_int(head.s, 2);
                if (out.error.empty()) out.error = "ListenBrainz error " + std::to_string(out.error_code);
            } else if (text(head.s, 3) != "array") {
                out.error = unreadable;
            } else {
                statement rows(db, "SELECT json_extract(value,'$.track_metadata.artist_name'),"
                                   "json_extract(value,'$.track_metadata.track_name'),"
                                   "json_extract(value,'$.track_metadata.release_name'),"
                                   "COALESCE(json_extract(value,'$.track_metadata.mbid_mapping.recording_mbid'),"
                                   "json_extract(value,'$.track_metadata.additional_info.recording_mbid'),''),"
                                   "json_extract(value,'$.listened_at') FROM json_each(?1,'$.payload.listens')");
                if (!rows.s) {
                    out.error = unreadable;
                } else {
                    sqlite3_bind_text(rows.s, 1, json.c_str(), -1, SQLITE_TRANSIENT);
                    while (sqlite3_step(rows.s) == SQLITE_ROW) {
                        lastfm_row r;
                        r.at = sqlite3_column_int64(rows.s, 4);
                        if (r.at <= 0) continue; // the track playing right now has no date
                        r.artist = text(rows.s, 0);
                        r.track = text(rows.s, 1);
                        r.album = text(rows.s, 2);
                        r.mbid = text(rows.s, 3);
                        if (r.artist.empty() && r.track.empty()) continue;
                        out.items.push_back(std::move(r));
                    }
                }
            }
        }
    }
    sqlite3_close(db);
    return out;
}

std::string listens_url(const std::string& user, int count, int64_t max_ts, int64_t min_ts) {
    std::string url = "https://api.listenbrainz.org/1/user/" + percent_encode(user) + "/listens?count=" +
                      std::to_string(count);
    if (max_ts > 0) url += "&max_ts=" + std::to_string(max_ts);
    else if (min_ts > 0) url += "&min_ts=" + std::to_string(min_ts);
    return url;
}

bool next_page(bool forward, int64_t oldest, int64_t newest, int64_t& max_ts, int64_t& min_ts) {
    if (forward) {
        if (newest - 1 <= min_ts) return false;
        min_ts = newest - 1;
    } else {
        if (max_ts && oldest + 1 >= max_ts) return false;
        max_ts = oldest + 1;
    }
    return true;
}

bool is_temporary_listenbrainz_error(int error_code) {
    return error_code == 429 || error_code == 500 || error_code == 502 || error_code == 503 || error_code == 504;
}

} // namespace upc
