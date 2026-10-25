#pragma once
// Import helpers with no foobar2000 dependency: merging, Playcount 2003's stored format, date parsing.
#include <cstdint>
#include <string>
#include <vector>

#include "store.h"

namespace upc {

// Combines imported data into an existing record without double counting, so importing twice changes nothing:
// higher play count, earliest first played / added, latest last played, existing rating wins, loved if either.
record merge(record existing, const record& imported);

// Another plugin's local data replacing ours, the last import wins: play count and first / last played become the
// imported ones, nothing is added up or compared. Added date, rating and loved mark are taken when the import has
// one; an empty one leaves ours.
record replace_stats(record existing, const record& imported);

// Same statistics (labels not compared).
bool same_stats(const record& a, const record& b);

// "2026-10-16 13:05:09" in local time -> unix time; 0 if not a valid date.
int64_t parse_local_datetime(const std::string& text);

// Play times as Enhanced Playback Statistics' %played_times_js% gives them: a JSON array of Unix times in
// milliseconds, e.g. "[1760620000000,1760700000000]". Returns Unix seconds, sorted, without duplicates; items
// that aren't positive numbers are skipped, anything that isn't an array gives nothing.
std::vector<int64_t> parse_played_times_ms(const std::string& text);

struct pc2003_row {
    std::string path; // as foobar2000 stores it, e.g. "file://E:\..."
    uint32_t subsong = 0;
    record rec;
};

struct pc2003_data {
    std::vector<pc2003_row> rows; // one per known file
    size_t records = 0;           // stored records in total (several files can share one)
    size_t orphans = 0;           // records no file points to: can't be matched to a track
};

// Reads Playcount 2003's data from foobar2000's metadb.sqlite, read-only. Throws std::runtime_error.
pc2003_data read_playcount2003(const std::string& metadb_path);

} // namespace upc
