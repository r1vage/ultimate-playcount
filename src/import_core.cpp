#include "import_core.h"
#include "maintenance.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include "../third_party/sqlite/sqlite3.h"

namespace upc {

namespace {

uint32_t read_u32(const unsigned char* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Playcount 2003's index GUID; the table without _data maps "<subsong>+<path>" to the record's hash.
constexpr char pc2003_map[] = "metadb_index_BE36C585_58CE_4465_9825_F2CA30CCEEED";

} // namespace

record merge(record existing, const record& imported) {
    existing.playcount = std::max(existing.playcount, imported.playcount);
    existing.first_played = earliest(existing.first_played, imported.first_played);
    existing.added = earliest(existing.added, imported.added);
    existing.last_played = std::max(existing.last_played, imported.last_played);
    if (!existing.rating) existing.rating = imported.rating;
    existing.loved = existing.loved || imported.loved;
    return existing;
}

record replace_stats(record existing, const record& imported) {
    existing.playcount = imported.playcount;
    existing.first_played = imported.first_played;
    existing.last_played = imported.last_played;
    if (imported.added) existing.added = imported.added;
    if (imported.rating) existing.rating = imported.rating;
    if (imported.loved) existing.loved = true;
    return existing;
}

bool same_stats(const record& a, const record& b) {
    return a.playcount == b.playcount && a.first_played == b.first_played && a.added == b.added &&
           a.last_played == b.last_played && a.rating == b.rating && a.loved == b.loved;
}

int64_t parse_local_datetime(const std::string& text) {
    std::tm tm{};
    char tail;
    if (sscanf_s(text.c_str(), "%4d-%2d-%2d %2d:%2d:%2d%c", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
                 &tm.tm_min, &tm.tm_sec, &tail, 1u) != 6)
        return 0;
    if (tm.tm_year < 1971 || tm.tm_mon < 1 || tm.tm_mon > 12 || tm.tm_mday < 1 || tm.tm_mday > 31) return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    const auto t = std::mktime(&tm);
    return t > 0 ? static_cast<int64_t>(t) : 0;
}

std::vector<int64_t> parse_played_times_ms(const std::string& text) {
    std::vector<int64_t> out;
    const auto open = text.find('[');
    if (open == std::string::npos || text.find_first_not_of(" \t\r\n") != open) return out;
    const char* p = text.c_str() + open + 1;
    while (*p && *p != ']') {
        char* end = nullptr;
        const long long ms = std::strtoll(p, &end, 10);
        if (end == p) { // not a number: skip to the next item
            while (*p && *p != ',' && *p != ']') ++p;
        } else {
            if (ms > 0) out.push_back(ms / 1000);
            p = end;
            while (*p && *p != ',' && *p != ']') ++p; // a fraction or trailing text
        }
        if (*p == ',') ++p;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

pc2003_data read_playcount2003(const std::string& metadb_path) {
    sqlite3* db = nullptr;
    // Read-only: we never write to foobar2000's own database.
    std::string uri = "file:";
    for (char c : metadb_path) {
        if (c == '\\') uri += '/';
        else if (c == '%' || c == '?' || c == '#') uri += c == '%' ? "%25" : c == '?' ? "%3f" : "%23";
        else uri += c;
    }
    uri += "?mode=ro";
    if (sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) != SQLITE_OK) {
        std::string err = db ? sqlite3_errmsg(db) : "out of memory";
        sqlite3_close(db);
        throw std::runtime_error("cannot open " + metadb_path + ": " + err);
    }
    struct closer {
        sqlite3* db;
        ~closer() { sqlite3_close(db); }
    } close_db{db};
    sqlite3_busy_timeout(db, 5000);

    auto query = [&](const std::string& sql, auto&& row) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("Playcount 2003 data not found: ") + sqlite3_errmsg(db));
        int rc;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) row(s);
        sqlite3_finalize(s);
        if (rc != SQLITE_DONE) throw std::runtime_error(std::string("reading failed: ") + sqlite3_errmsg(db));
    };

    const std::string map = std::string("\"") + pc2003_map + "\"", data = std::string("\"") + pc2003_map + "_data\"";
    pc2003_data out;
    query("SELECT COUNT(*), SUM(NOT EXISTS (SELECT 1 FROM " + map + " m WHERE m.key = d.key)) FROM " + data + " d",
          [&](sqlite3_stmt* s) {
              out.records = static_cast<size_t>(sqlite3_column_int64(s, 0));
              out.orphans = static_cast<size_t>(sqlite3_column_int64(s, 1));
          });
    query("SELECT m.filename, d.value FROM " + map + " m JOIN " + data + " d ON d.key = m.key", [&](sqlite3_stmt* s) {
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(s, 0));
        const auto* blob = static_cast<const unsigned char*>(sqlite3_column_blob(s, 1));
        if (!name || !blob || sqlite3_column_bytes(s, 1) < 24) return;
        const char* plus = std::strchr(name, '+');
        if (!plus) return;
        pc2003_row r;
        r.subsong = static_cast<uint32_t>(std::strtoul(name, nullptr, 10));
        r.path = plus + 1;
        // 6 little-endian uint32: first_played, last_played, loved, playcount, rating, added
        r.rec.first_played = read_u32(blob);
        r.rec.last_played = read_u32(blob + 4);
        r.rec.loved = read_u32(blob + 8) != 0;
        r.rec.playcount = read_u32(blob + 12);
        const uint32_t rating = read_u32(blob + 16);
        r.rec.rating = rating <= 5 ? rating : 0;
        r.rec.added = read_u32(blob + 20);
        out.rows.push_back(std::move(r));
    });
    return out;
}

} // namespace upc
