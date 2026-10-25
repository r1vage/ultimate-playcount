// Unit tests for the foobar2000-independent core. Exit code 0 = all passed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "../src/import_core.h"
#include "../src/lastfm_core.h"
#include "../src/listenbrainz_core.h"
#include "../src/maintenance.h"
#include "../src/play_counter.h"
#include "../third_party/sqlite/sqlite3.h"
#include "../src/store.h"
#include "../src/time_format.h"

static int g_failed = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failed;                                                                                            \
        }                                                                                                          \
    } while (0)

using upc::play_counter;

// A steady clock for the tests: each tick is one real second later unless said otherwise.
static double g_clock = 1000;
static bool tick(play_counter& c, double position, double seconds_passed = 1) {
    return c.on_time(position, g_clock += seconds_passed);
}

// Plays from `from` to `to` (exclusive of from) with 1 s ticks; returns how many times a play was logged.
static int play(play_counter& c, int from, int to) {
    int logs = 0;
    for (int t = from + 1; t <= to; ++t) logs += tick(c, t);
    return logs;
}

static void test_threshold() {
    using upc::count_mode;
    CHECK(upc::threshold_for(count_mode::percent, 50, 240) == 120);
    CHECK(upc::threshold_for(count_mode::percent, 100, 240) == 240);
    CHECK(upc::threshold_for(count_mode::percent, 250, 240) == 240); // clamped to 100 %
    CHECK(upc::threshold_for(count_mode::percent, 0, 200) == 2);     // clamped to 1 %
    CHECK(upc::threshold_for(count_mode::seconds, 60, 240) == 60);
    CHECK(upc::threshold_for(count_mode::seconds, 240, 180) == 180); // short track: whole track
    CHECK(upc::threshold_for(count_mode::seconds, 0, 240) == 60);
    CHECK(std::isinf(upc::threshold_for(count_mode::track_end, 0, 240)));
    for (auto m : {count_mode::percent, count_mode::seconds, count_mode::track_end})
        CHECK(upc::threshold_for(m, 50, 0) == 60); // unknown length (stream)
}

static void test_track_end_mode() {
    const double length = 179.6;
    play_counter c;
    c.start(upc::threshold_for(upc::count_mode::track_end, 0, length), length);
    CHECK(play(c, 0, 179) == 0); // listening time alone never counts
    CHECK(c.finish());           // reaching the end does
    CHECK(!c.finish());          // once

    c.start(upc::threshold_for(upc::count_mode::track_end, 0, length), length);
    play(c, 0, 90);
    CHECK(!c.finish()); // skipped away mid-track
}

static void test_basic_logging() {
    play_counter c;
    c.start(120, 240);
    CHECK(play(c, 0, 119) == 0);
    CHECK(tick(c, 120)); // >= threshold, exactly once
    CHECK(play(c, 120, 240) == 0);
    CHECK(c.logged());
}

static void test_refresh_does_not_reset() {
    // The bug being fixed: info refreshes mid-song. The counter has no refresh input at all, and the
    // integration never calls start() on refresh; what matters is that time keeps accumulating.
    play_counter c;
    c.start(100, 200);
    CHECK(play(c, 0, 50) == 0);
    // (refresh happens here: nothing is called)
    CHECK(play(c, 50, 100) == 1);
}

// A skip: left before the end, not counted as played, after at least a second.
static void test_skips() {
    upc::play_counter c;
    c.start(100, 200, 0, 0); // counts after 100 s of a 200 s track
    for (int t = 1; t <= 10; ++t) CHECK(!c.on_time(t, t));
    CHECK(!c.finish() && c.skipped()); // 10 s heard, left at 10 s
    c.stop();
    CHECK(!c.skipped()); // not active any more

    c.start(100, 200, 0, 0);
    c.on_time(0.5, 0.5);
    CHECK(!c.finish() && !c.skipped()); // under a second: tapping through a list
    c.stop();

    c.start(100, 200, 0, 0);
    for (int t = 1; t <= 100; ++t) c.on_time(t, t);
    CHECK(c.logged() && !c.finish() && !c.skipped()); // already counted as played
    c.stop();

    c.start(100, 200, 0, 0);
    c.on_seek(190);
    for (int t = 191; t <= 200; ++t) c.on_time(t, t - 190);
    CHECK(!c.finish() && !c.skipped()); // heard only 10 s but played to the end: no skip
    c.stop();

    c.start(60, 0, 0, 0); // a stream without a length
    for (int t = 1; t <= 5; ++t) c.on_time(t, t);
    CHECK(!c.finish() && c.skipped());
    c.stop();
}

static void test_backward_seek_adds_nothing() {
    play_counter c;
    c.start(100, 200);
    play(c, 0, 60);
    c.on_seek(10); // go back 50 s
    CHECK(c.listened() == 60);
    CHECK(play(c, 10, 49) == 0); // 60 + 39 = 99
    CHECK(tick(c, 50));        // 100: re-listening counts as listening
}

static void test_forward_seek_not_credited() {
    play_counter c;
    c.start(100, 300);
    play(c, 0, 30);
    c.on_seek(250);
    CHECK(c.listened() == 30);
    CHECK(play(c, 250, 300) == 0); // 30 + 50 = 80 < 100
    CHECK(!c.finish());
}

static void test_unreported_jump_capped_by_real_time() {
    play_counter c;
    c.start(100, 300);
    play(c, 0, 10);
    tick(c, 200); // jumps 190 s in one real second, no seek event: credit at most the real time
    CHECK(c.listened() > 10 && c.listened() <= 11.5 + 1e-9);
}

static void test_busy_foobar_still_counts() {
    // foobar2000 busy (tags being written, slow panel): time updates bunch up and jump 5 s every 5 s.
    play_counter c;
    c.start(100, 300, 0, g_clock);
    int logs = 0;
    for (int t = 5; t <= 100; t += 5) logs += tick(c, t, 5);
    CHECK(logs == 1); // all 100 s heard, so it counts
    CHECK(c.last_step() == 5);
}

static void test_backward_tick_without_seek() {
    play_counter c;
    c.start(100, 300);
    play(c, 0, 50);
    tick(c, 5); // e.g. a restart of the same position, no seek event
    CHECK(c.listened() == 50);
    CHECK(play(c, 5, 54) == 0);
    CHECK(tick(c, 55));
}

static void test_stop_and_new_track_reset() {
    play_counter c;
    c.start(100, 200);
    play(c, 0, 90);
    c.stop();
    CHECK(!tick(c, 91)); // inactive after stop
    c.start(100, 200);    // next track (or the same one again)
    CHECK(c.listened() == 0);
    CHECK(play(c, 0, 99) == 0);
    CHECK(tick(c, 100));
}

static void test_threshold_equals_length() {
    // Last tick of a 179.6 s track is at 179; finish() credits the rest.
    play_counter c;
    c.start(179.6, 179.6);
    CHECK(play(c, 0, 179) == 0);
    CHECK(c.finish());
    CHECK(!c.finish()); // still once
}

static void test_finish_far_from_end() {
    play_counter c;
    c.start(100, 200);
    play(c, 0, 90);
    CHECK(!c.finish()); // skipped away at 90 s: no credit for the rest
}

static void test_fractional_ticks() {
    play_counter c;
    c.start(1.0, 10);
    double t = 0;
    int logs = 0;
    for (int i = 0; i < 10; ++i) logs += tick(c, t += 0.1, 0.1);
    CHECK(logs == 1);
}

static void test_resume_mid_track() {
    // Playback starts at 120 s (resume): first tick at 121 adds 1 s, not 121 s.
    play_counter c;
    c.start(60, 300, 120);
    CHECK(play(c, 120, 179) == 0);
    CHECK(tick(c, 180));
}

static void test_reannounce_mid_song() {
    play_counter c;
    c.start(100, 200);
    play(c, 0, 80);
    CHECK(c.continues_at(80.4)); // re-announced mid-song: caller keeps the count
    CHECK(play(c, 80, 99) == 0);
    CHECK(tick(c, 100));
    CHECK(!c.continues_at(0.2)); // repeat-track restart from the top is a new play
    c.on_seek(150);
    CHECK(!c.continues_at(20)); // somewhere else entirely
    c.stop();
    CHECK(!c.continues_at(150)); // nothing playing
}

// The hook's sequence on a mid-song refresh, with or without a stop first.
static void test_refresh_sequence() {
    play_counter c;
    c.start(100, 200);
    play(c, 0, 80);
    CHECK(!play_counter::stop_ends_track(true)); // stop(starting another): wait for the new track
    CHECK(c.reannounced(true, 80.4));            // same file, same spot: keep the count
    CHECK(play(c, 80, 99) == 0);
    CHECK(tick(c, 100));
    CHECK(!c.reannounced(false, 80.4)); // another file is a new track
    CHECK(play_counter::stop_ends_track(false)); // user stop / end of playlist ends the track
}

static void test_time_format() {
    CHECK(upc::format_ago(12) == "12s");
    CHECK(upc::format_ago(250) == "4m 10s");
    CHECK(upc::format_ago(2 * 3600 + 7 * 60) == "2h 7m");
    CHECK(upc::format_ago(5 * 86400 + 4 * 3600) == "5d 4h");
    CHECK(upc::format_ago(23 * 86400) == "3wk 2d");
    const int64_t now = 1760000000;
    CHECK(upc::format_ago2(now - 60, now) == "Today" || upc::format_ago2(now - 60, now) == "Yesterday");
    CHECK(upc::format_ago2(now - 3 * 86400, now) == "3d");
    CHECK(upc::format_ago2(now - 10 * 86400, now) == "1w 3d");
    CHECK(upc::format_ago2(now - 400 * 86400, now) == "1y 1m 5d");
    CHECK(upc::format_date(0).size() == 19);
}

static void test_store() {
    const auto path = std::filesystem::temp_directory_path() / "upc_core_tests.db";
    std::error_code ec;
    for (auto ext : {"", "-wal", "-shm"}) std::filesystem::remove(path.string() + ext, ec);
    {
        upc::store s;
        s.open(path.string());
        CHECK(s.size() == 0);
        CHECK(!s.get("a|b"));
        upc::record r;
        r.playcount = 3;
        r.rating = 4;
        r.loved = true;
        r.first_played = 1700000000;
        r.title = "T\xC3\xA9st"; // UTF-8 survives
        r.path = "E:\\rivage\\Music\\x.flac";
        s.put_many({{"a|b", r}});
        s.put_many({{"k1", upc::record{}}, {"k2", r}});
        CHECK(s.size() == 3);
        // Rollback journal: no -wal / -shm files next to the database, even while it is open.
        CHECK(!std::filesystem::exists(path.string() + "-wal") && !std::filesystem::exists(path.string() + "-shm"));
    }
    {
        // A database left in WAL mode by an older version is switched over when opened.
        sqlite3* db = nullptr;
        sqlite3_open(path.string().c_str(), &db);
        sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
        sqlite3_close(db);
        upc::store s;
        s.open(path.string());
        s.put_many({{"k3", upc::record{}}});
        CHECK(s.size() == 4 && !std::filesystem::exists(path.string() + "-wal"));
        s.put_many({}, {"k3"});
    }
    {
        upc::store s;
        s.open(path.string()); // reload from disk
        CHECK(s.size() == 3);
        auto r = s.get("a|b");
        CHECK(r && r->playcount == 3 && r->rating == 4 && r->loved && r->first_played == 1700000000);
        CHECK(r && r->title == "T\xC3\xA9st" && r->path == "E:\\rivage\\Music\\x.flac");
        const auto st = s.get_stats("a|b");
        CHECK(st && st->playcount == 3 && st->rating == 4 && st->loved && st->title.empty());
        const auto copy = path.string() + ".backup";
        std::filesystem::remove(copy, ec);
        s.backup_to(copy);
        upc::store b;
        b.open(copy);
        CHECK(b.size() == 3);
        b.close();
        for (auto ext : {"", "-wal", "-shm"}) std::filesystem::remove(copy + ext, ec);
    }
    for (auto ext : {"", "-wal", "-shm"}) std::filesystem::remove(path.string() + ext, ec);
}

static void test_merge() {
    upc::record ours;
    ours.playcount = 3;
    ours.first_played = 2000;
    ours.last_played = 9000;
    ours.rating = 4;
    upc::record old;
    old.playcount = 10;
    old.first_played = 1000;
    old.last_played = 5000;
    old.added = 500;
    old.rating = 2;
    old.loved = true;
    auto m = upc::merge(ours, old);
    CHECK(m.playcount == 10 && m.first_played == 1000 && m.last_played == 9000 && m.added == 500);
    CHECK(m.rating == 4 && m.loved); // existing rating wins
    CHECK(upc::same_stats(upc::merge(m, old), m)); // importing twice changes nothing
    CHECK(upc::merge(upc::record{}, old).rating == 2);
    CHECK(!upc::same_stats(m, ours));

    // Last import wins: count and dates replaced, never added up or compared.
    auto r = upc::replace_stats(ours, old);
    CHECK(r.playcount == 10 && r.first_played == 1000 && r.last_played == 5000 && r.added == 500);
    CHECK(r.rating == 2 && r.loved);
    upc::record low;
    low.playcount = 1;
    low.last_played = 4000;
    r = upc::replace_stats(r, low); // a lower count still replaces; empty rating, added and loved leave ours
    CHECK(r.playcount == 1 && r.first_played == 0 && r.last_played == 4000 && r.added == 500 && r.rating == 2 && r.loved);
    CHECK(upc::same_stats(upc::replace_stats(r, low), r)); // importing twice changes nothing
}

static void test_parse_datetime() {
    const int64_t t = upc::parse_local_datetime("2024-03-05 07:08:09");
    CHECK(t > 0 && upc::format_date(t) == "2024-03-05 07:08:09");
    CHECK(upc::parse_local_datetime("") == 0);
    CHECK(upc::parse_local_datetime("?") == 0);
    CHECK(upc::parse_local_datetime("2024-13-05 07:08:09") == 0);
    CHECK(upc::parse_local_datetime("2024-03-05 07:08:09x") == 0);
}

static void test_parse_played_times() {
    using v = std::vector<int64_t>;
    CHECK((upc::parse_played_times_ms("[1760620000000,1760700000999]") == v{1760620000, 1760700000}));
    CHECK((upc::parse_played_times_ms(" [ 3000 , 1000,3000 ] ") == v{1, 3})); // sorted, no duplicates
    CHECK((upc::parse_played_times_ms("[0,-5,\"x\",2000,]") == v{2}));         // junk skipped
    CHECK(upc::parse_played_times_ms("[]").empty());
    CHECK(upc::parse_played_times_ms("").empty());
    CHECK(upc::parse_played_times_ms("?").empty());
    CHECK(upc::parse_played_times_ms("N/A").empty());
    CHECK(upc::parse_played_times_ms("1000,2000").empty()); // not an array
    CHECK((upc::parse_played_times_ms("[5000") == v{5}));   // cut off: keep what is there
}

static void test_listenbrainz() {
    const std::string page = R"({"payload":{"count":3,"user_id":"u","latest_listen_ts":1760620000,"listens":[
        {"listened_at":1760620000,"track_metadata":{"artist_name":"Muse","track_name":"Hysteria","release_name":"Absolution",
          "additional_info":{"recording_mbid":"add-1"},"mbid_mapping":{"recording_mbid":"map-1"}}},
        {"listened_at":1760610000,"track_metadata":{"artist_name":"Arkells","track_name":"Leather Jacket",
          "additional_info":{"recording_mbid":"add-2"}}},
        {"listened_at":1760600000,"track_metadata":{"artist_name":"A","track_name":"T"}}]}})";
    auto r = upc::parse_listens(page);
    CHECK(r.error.empty() && r.items.size() == 3);
    CHECK(r.items[0].artist == "Muse" && r.items[0].track == "Hysteria" && r.items[0].album == "Absolution" &&
          r.items[0].at == 1760620000 && r.items[0].mbid == "map-1"); // ListenBrainz's own mapping wins
    CHECK(r.items[1].mbid == "add-2" && r.items[2].mbid.empty());
    r = upc::parse_listens(R"({"payload":{"count":0,"listens":[]}})");
    CHECK(r.error.empty() && r.items.empty());
    r = upc::parse_listens(R"({"code":404,"error":"Cannot find user: nobody"})");
    CHECK(r.error == "Cannot find user: nobody" && r.error_code == 404 && !upc::is_temporary_listenbrainz_error(404));
    r = upc::parse_listens(R"({"code":429,"error":"Rate limit exceeded"})");
    CHECK(upc::is_temporary_listenbrainz_error(r.error_code));
    CHECK(!upc::parse_listens("<html>").error.empty() && !upc::parse_listens("").error.empty() &&
          !upc::parse_listens("{}").error.empty());
    const auto url = upc::listens_url("a b", 1000, 500, 0);
    CHECK(url.find("/user/a%20b/listens?count=1000") != std::string::npos && url.find("&max_ts=500") != std::string::npos &&
          url.find("min_ts") == std::string::npos);
    CHECK(upc::listens_url("u", 1000, 0, 400).find("&min_ts=400") != std::string::npos);

    // Page cursor. Back from the newest: the next page asks for listens before oldest + 1, so a second split by
    // the page edge is read again rather than lost.
    int64_t max_ts = 0, min_ts = 0;
    CHECK(upc::next_page(false, 500, 900, max_ts, min_ts) && max_ts == 501 && min_ts == 0);
    CHECK(upc::next_page(false, 200, 500, max_ts, min_ts) && max_ts == 201);
    CHECK(!upc::next_page(false, 200, 200, max_ts, min_ts) && max_ts == 201); // stuck on one second: stop
    // Forward from the last sync: listens after newest - 1.
    max_ts = 0, min_ts = 99;
    CHECK(upc::next_page(true, 100, 400, max_ts, min_ts) && min_ts == 399 && max_ts == 0);
    CHECK(!upc::next_page(true, 400, 400, max_ts, min_ts) && min_ts == 399); // stuck on one second: stop

    const auto path = (std::filesystem::temp_directory_path() / "upc_listens.db").string();
    std::error_code ec;
    for (auto ext : {"", "-wal", "-shm"}) std::filesystem::remove(path + ext, ec);
    const std::vector<upc::lastfm_row> rows = {{"Muse", "Hysteria", "", "map-1", 300},
                                               {"Other Spelling", "Song", "", "map-1", 100}, // same recording id
                                               {"muse", "hysteria", "", "", 200},
                                               {"Muse", "Hysteria", "", "map-1", 300}, // repeated in one batch
                                               {"Arkells", "Leather Jacket", "", "", 50}};
    {
        upc::store s;
        s.open(path);
        CHECK(s.listens_total() == 0 && s.listens_latest() == 0 && s.listen_times({"x"}).empty());
        CHECK(s.count_new_listens(rows) == 4 && s.add_listens(rows) == 4);
        CHECK(s.add_listens(rows) == 0 && s.count_new_listens(rows) == 0); // again changes nothing
        const auto muse = upc::match_key("Muse", "Hysteria");
        CHECK((s.listen_times({muse}) == std::vector<int64_t>{200, 300}));
        CHECK((s.listen_times({muse}, "map-1") == std::vector<int64_t>{100, 200, 300})); // name and id, once each
        CHECK((s.listen_times({}, "map-1") == std::vector<int64_t>{100, 300}));
        CHECK(s.listens_total() == 4 && s.listens_latest() == 300);
    }
    {
        upc::store s;
        s.open(path); // loaded from disk, the id index too
        CHECK(s.listens_total() == 4 && (s.listen_times({}, "map-1") == std::vector<int64_t>{100, 300}));
        CHECK(s.add_listens({{"Muse", "Hysteria", "", "", 400}}) == 1 && s.listens_latest() == 400);
    }
    for (auto ext : {"", "-wal", "-shm"}) std::filesystem::remove(path + ext, ec);
}

static void put_u32(std::string& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b += static_cast<char>((v >> (8 * i)) & 0xff);
}

static void test_read_playcount2003() {
    // Fake metadb.sqlite with Playcount 2003's two tables, in a folder whose name needs URI escaping.
    const auto dir = std::filesystem::temp_directory_path() / "upc tests #1 %";
    std::filesystem::create_directories(dir);
    const auto path = dir / "metadb.sqlite";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    sqlite3* db = nullptr;
    CHECK(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
    sqlite3_exec(db,
                 "CREATE TABLE metadb_index_BE36C585_58CE_4465_9825_F2CA30CCEEED (key INTEGER NOT NULL, filename "
                 "TEXT NOT NULL UNIQUE PRIMARY KEY);"
                 "CREATE TABLE metadb_index_BE36C585_58CE_4465_9825_F2CA30CCEEED_data (key INTEGER NOT NULL PRIMARY "
                 "KEY UNIQUE, value BLOB);"
                 "INSERT INTO metadb_index_BE36C585_58CE_4465_9825_F2CA30CCEEED VALUES "
                 "(7, '0+file://E:\\rivage\\Music\\a.flac'), (7, '2+file://E:\\rivage\\Music\\b.cue'), "
                 "(-5, '0+file://E:\\x.mp3');",
                 nullptr, nullptr, nullptr);
    std::string blob;
    for (uint32_t v : {1000u, 5000u, 1u, 12u, 4u, 500u}) put_u32(blob, v);
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO metadb_index_BE36C585_58CE_4465_9825_F2CA30CCEEED_data VALUES (?, ?)", -1, &s,
                       nullptr);
    for (int64_t key : {7, 99}) { // 99: orphan record
        sqlite3_bind_int64(s, 1, key);
        sqlite3_bind_blob(s, 2, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);
        sqlite3_step(s);
        sqlite3_reset(s);
    }
    sqlite3_finalize(s);
    sqlite3_close(db);

    const auto data = upc::read_playcount2003(path.string());
    CHECK(data.records == 2 && data.orphans == 1);
    CHECK(data.rows.size() == 2); // the -5 mapping has no record
    for (auto& r : data.rows) {
        CHECK(r.rec.first_played == 1000 && r.rec.last_played == 5000 && r.rec.loved && r.rec.playcount == 12);
        CHECK(r.rec.rating == 4 && r.rec.added == 500);
        if (r.subsong == 2) CHECK(r.path == "file://E:\\rivage\\Music\\b.cue");
        else CHECK(r.subsong == 0 && r.path == "file://E:\\rivage\\Music\\a.flac");
    }
    bool threw = false;
    try {
        upc::read_playcount2003((dir / "missing.sqlite").string());
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    std::filesystem::remove_all(dir, ec);
}

static upc::record plays(uint32_t n, int64_t first = 0, int64_t last = 0) {
    upc::record r;
    r.playcount = n;
    r.first_played = first;
    r.last_played = last;
    return r;
}

static void test_lastfm_parse() {
    // track.getInfo for a user
    auto info = upc::parse_track_info(R"({"track":{"name":"T","userplaycount":"12","userloved":"1"}})");
    CHECK(info.error.empty() && info.found && info.playcount == 12 && info.loved);
    info = upc::parse_track_info(R"({"track":{"name":"T","userplaycount":"0","userloved":"0"}})");
    CHECK(info.error.empty() && info.playcount == 0 && !info.loved);
    info = upc::parse_track_info(R"({"track":{"name":"T"}})"); // no user numbers: none
    CHECK(info.error.empty() && info.playcount == 0);
    info = upc::parse_track_info(R"({"error":6,"message":"Track not found"})");
    CHECK(info.error.empty() && !info.found && info.playcount == 0); // Last.fm doesn't know it: not a failure
    info = upc::parse_track_info(R"({"error":10,"message":"Invalid API key"})");
    CHECK(info.error == "Invalid API key" && info.error_code == 10 && upc::is_fatal_error(10));
    info = upc::parse_track_info(R"({"error":29,"message":"Rate Limit Exceeded"})");
    CHECK(upc::is_temporary_error(info.error_code) && !upc::is_fatal_error(29));
    CHECK(!upc::parse_track_info("<html>nope").error.empty());
    CHECK(!upc::parse_track_info("").error.empty());
    CHECK(!upc::parse_track_info("{}").error.empty());

    // the user's scrobbles of one track: the one playing right now has no date
    const std::string many =
        R"({"trackscrobbles":{"track":[
          {"artist":{"mbid":"","#text":"Arkells"},"name":"Now","@attr":{"nowplaying":"true"}},
          {"artist":{"mbid":"","#text":"Arkells"},"name":"Leather Jacket","date":{"uts":"1760620000","#text":"x"}},
          {"artist":{"mbid":"","#text":"Arkells"},"name":"Leather Jacket","date":{"uts":"1760610000"}}
        ],"@attr":{"user":"u","totalPages":"3","page":"1","total":"500"}}})";
    auto r = upc::parse_track_scrobbles(many);
    CHECK(r.error.empty() && r.pages == 3 && r.total == 500 && r.items.size() == 2);
    CHECK(r.items[0].at == 1760620000 && r.items[1].at == 1760610000);
    // Last.fm sends a single play as an object, not an array
    r = upc::parse_track_scrobbles(
        R"({"trackscrobbles":{"track":{"artist":{"#text":"A"},"name":"T","date":{"uts":"5"}},"@attr":{"totalPages":"1","total":"1"}}})");
    CHECK(r.error.empty() && r.items.size() == 1 && r.items[0].at == 5);
    r = upc::parse_track_scrobbles(R"({"trackscrobbles":{"track":[],"@attr":{"totalPages":"0","total":"0"}}})");
    CHECK(r.error.empty() && r.items.empty() && r.pages == 1);
    CHECK(!upc::parse_track_scrobbles("{}").error.empty());
    CHECK(upc::parse_track_scrobbles(R"({"error":6,"message":"x"})").error_code == 6);

    // the user's loved tracks
    const std::string loved = R"({"lovedtracks":{"track":[
        {"name":"Hysteria","mbid":"m1","date":{"uts":"1700000000"},"artist":{"name":"Muse","mbid":""}},
        {"name":"Song","mbid":"","artist":{"name":"A"}}],"@attr":{"totalPages":"2","total":"300"}}})";
    r = upc::parse_loved_tracks(loved);
    CHECK(r.error.empty() && r.pages == 2 && r.items.size() == 2);
    CHECK(r.items[0].artist == "Muse" && r.items[0].track == "Hysteria" && r.items[0].at == 1700000000);
    CHECK(r.items[1].artist == "A" && r.items[1].at == 0); // no date: still loved
    r = upc::parse_loved_tracks(R"({"lovedtracks":{"track":{"name":"T","artist":{"name":"A"}},"@attr":{"totalPages":"1"}}})");
    CHECK(r.error.empty() && r.items.size() == 1);
    r = upc::parse_loved_tracks(R"({"lovedtracks":{"track":[],"@attr":{"totalPages":"0"}}})");
    CHECK(r.error.empty() && r.items.empty());

    const auto url = upc::track_info_url("a b&c", "k", "Sigur Rós", "Sæglópur", true);
    CHECK(url.find("username=a%20b%26c") != std::string::npos && url.find("artist=Sigur%20R%C3%B3s") != std::string::npos &&
          url.find("autocorrect=1") != std::string::npos);
    CHECK(upc::track_info_url("u", "k", "A", "T", false).find("autocorrect=0") != std::string::npos);
    CHECK(upc::track_scrobbles_url("u", "k", "A", "T", 2, true).find("&page=2") != std::string::npos);
    CHECK(upc::track_scrobbles_url("u", "k", "A", "T", 2, false).find("autocorrect=0") != std::string::npos);
    CHECK(upc::loved_tracks_url("u", "k", 3).find("method=user.getlovedtracks") != std::string::npos);
}

static void test_lastfm_store() {
    CHECK(upc::match_key("  The  Muse ", "Hysteria") == upc::match_key("the muse", "HYSTERIA"));
    CHECK(upc::match_key("a", "b c") != upc::match_key("a b", "c")); // the artist/title border is kept
    const auto path = (std::filesystem::temp_directory_path() / "upc_lastfm.db").string();
    std::error_code ec;
    for (auto ext : {"", "-wal", "-shm"}) std::filesystem::remove(path + ext, ec);
    upc::lastfm_track t;
    t.playcount = 5;
    t.loved = true;
    t.fetched_at = 1000;
    t.times = {300, 100, 200, 100}; // unsorted, a repeat
    {
        upc::store s;
        s.open(path);
        bool threw = false;
        try { s.put_lastfm_track("a", t); } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw && !s.get_lastfm_track("a")); // needs a record first
        s.put_many({{"a", plays(1)}, {"b", plays(1)}});
        s.add_if_missing("a", upc::record{}); // an existing record is left alone
        s.add_if_missing("e", plays(0));
        CHECK(s.get("a")->playcount == 1 && s.get("e"));
        threw = false;
        try { s.put_lastfm_tracks({{"a", t}, {"nobody", t}}); } catch (const std::runtime_error&) { threw = true; }
        CHECK(threw && !s.get_lastfm_track("a")); // one in the batch fails: none is stored
        s.put_lastfm_track("a", t);
        auto got = s.get_lastfm_track("a");
        CHECK(got && got->playcount == 5 && got->loved && got->fetched_at == 1000 &&
              (got->times == std::vector<int64_t>{100, 200, 300}));
        auto sum = s.get_lastfm_summary("a");
        CHECK(sum && sum->playcount == 5 && sum->times.empty() && !s.get_lastfm_summary("b"));
        t.playcount = 6;
        t.times = {400};
        s.put_lastfm_track("a", t); // replaces what was there
        CHECK(s.get_lastfm_track("a")->times == std::vector<int64_t>{400} && s.lastfm_track_count() == 1);
        s.put_lastfm_track("b", t);
        s.put_many({{"a", plays(2)}}); // rewriting the record keeps the numbers
        CHECK(s.get_lastfm_track("a") && s.lastfm_track_count() == 2);
    }
    {
        upc::store s;
        s.open(path); // loaded from disk
        CHECK(s.get_lastfm_track("a")->playcount == 6 && s.get_lastfm_track("a")->times == std::vector<int64_t>{400});
        CHECK(s.get("e")); // add_if_missing reached the file
        // a -> c is a move (a removed), b -> d a copy of the record (b stays; its numbers stay with it)
        s.put_many({{"c", plays(2)}, {"d", plays(1)}}, {"a"}, {{"a", "c"}, {"b", "d"}});
        CHECK(!s.get_lastfm_track("a") && s.get_lastfm_track("c") && s.get_lastfm_track("c")->playcount == 6);
        CHECK(s.get_lastfm_track("b") && !s.get_lastfm_track("d"));
        s.forget_lastfm({"b", "nobody"});
        CHECK(!s.get_lastfm_track("b"));
        s.put_many({}, {"c"}); // removed with its record
        CHECK(!s.get_lastfm_track("c"));
    }
    {
        upc::store s;
        s.open(path);
        CHECK(s.lastfm_track_count() == 0);
    }
    for (auto ext : {"", "-wal", "-shm"}) std::filesystem::remove(path + ext, ec);
}


static const upc::record* find(const upc::change_set& c, const std::string& key) {
    for (auto& [k, r] : c.puts)
        if (k == key) return &r;
    return nullptr;
}

static bool removed(const upc::change_set& c, const std::string& key) {
    return std::find(c.removes.begin(), c.removes.end(), key) != c.removes.end();
}

static void test_combine() {
    auto c = upc::combine(plays(3, 100, 900), plays(4, 50, 500));
    CHECK(c.playcount == 7 && c.first_played == 50 && c.last_played == 900);
}

static void test_rekey() {
    // A and B (same song on two albums) become one key S; C keeps its key; D has no known track.
    std::vector<std::pair<std::string, upc::record>> recs = {
        {"A", plays(2, 10, 20)}, {"B", plays(3, 5, 30)}, {"C", plays(1)}, {"D", plays(9)}};
    auto c = upc::rekey(recs, {{"A", "S"}, {"B", "S"}, {"C", "C"}});
    auto s = find(c, "S");
    CHECK(s && s->playcount == 5 && s->first_played == 5 && s->last_played == 30);
    CHECK(removed(c, "A") && removed(c, "B"));
    CHECK(!find(c, "C") && !removed(c, "C")); // unchanged: not touched
    CHECK(!find(c, "D") && !removed(c, "D")); // no track: kept as is
    CHECK(c.puts.size() == 1);

    // Split: one old key, two new keys -> copied to both; plays already under a new key are kept.
    recs = {{"X", plays(4)}, {"X2", plays(1)}};
    c = upc::rekey(recs, {{"X", "X1"}, {"X", "X2"}});
    CHECK(find(c, "X1") && find(c, "X1")->playcount == 4);
    CHECK(find(c, "X2") && find(c, "X2")->playcount == 5);
    CHECK(removed(c, "X") && !removed(c, "X2"));

    // Swap: P -> Q and Q -> P must not mix.
    recs = {{"P", plays(1)}, {"Q", plays(10)}};
    c = upc::rekey(recs, {{"P", "Q"}, {"Q", "P"}});
    CHECK(find(c, "Q")->playcount == 1 && find(c, "P")->playcount == 10 && c.removes.empty());

    // Partial: old key stays for one file, moves for another.
    recs = {{"K", plays(2)}};
    c = upc::rekey(recs, {{"K", "K"}, {"K", "K9"}});
    CHECK(find(c, "K")->playcount == 2 && find(c, "K9")->playcount == 2 && c.removes.empty());
}

static void test_scan_for_cleanup() {
    upc::record edited = plays(3), gone = plays(1), live = plays(2);
    edited.path = "E:\\m\\a.flac";
    gone.path = "E:\\m\\deleted.flac";
    live.path = "E:\\m\\b.cue";
    live.subsong = 2;
    std::vector<std::pair<std::string, upc::record>> recs = {{"old-a", edited}, {"gone", gone}, {"b2", live}};
    std::map<upc::location, std::string> tracks = {{{"E:\\m\\a.flac", 0}, "new-a"}, {{"E:\\m\\b.cue", 2}, "b2"}};
    auto s = upc::scan_for_cleanup(recs, tracks);
    CHECK(s.reattach.size() == 1 && s.reattach[0].first == "old-a" && s.reattach[0].second == "new-a");
    CHECK(s.orphans.size() == 1 && s.orphans[0] == "gone");
}

static void test_pick_auto_clean() {
    const int64_t now = 1800000000, day = 86400;
    upc::record old_plays = plays(5, 0, now - 400 * day), recent = plays(1, 0, now - 10 * day),
                rated = plays(0, 0, now - 400 * day), on_disk = plays(1, 0, now - 400 * day);
    rated.rating = 3;
    on_disk.path = "exists";
    std::vector<std::pair<std::string, upc::record>> orphans = {
        {"old", old_plays}, {"recent", recent}, {"rated", rated}, {"disk", on_disk}};
    auto gone = [](const upc::record& r) { return r.path != "exists"; };
    auto picked = upc::pick_auto_clean(orphans, 100, now, 365, gone);
    CHECK(picked.size() == 1 && picked[0] == "old");
    CHECK(upc::pick_auto_clean(orphans, 100, now, 0, gone).empty()); // off
    CHECK(upc::pick_auto_clean(orphans, 3, now, 365, gone).empty()); // 1 of 3 > a quarter: missing drive?
}

static void test_backups_to_prune() {
    const std::vector<std::string> names = {
        "ultimate_playcount-auto-20261014-090000.db",          "ultimate_playcount-auto-20261016-090000.db",
        "ultimate_playcount-auto-20261015-090000.db",          "ultimate_playcount-before-import-20261001-120000.db",
        "ultimate_playcount-before-import-20261016-193142.db", "ultimate_playcount.db",
        "notes.txt",                                           "ultimate_playcount-auto-garbage.db"};
    auto del = upc::backups_to_prune(names, 2);
    CHECK(del.size() == 1 && del[0] == "ultimate_playcount-auto-20261014-090000.db"); // oldest auto only
    del = upc::backups_to_prune(names, 1);
    CHECK(del.size() == 3); // 2 older autos + the older import; each kind keeps its newest
    CHECK(std::find(del.begin(), del.end(), "ultimate_playcount-before-import-20261016-193142.db") == del.end());
    CHECK(upc::backups_to_prune(names, 0).size() == 3); // keep at least one of each
    CHECK(upc::backups_to_prune(names, 10).empty());
}

static void test_store_v2() {
    const auto path = (std::filesystem::temp_directory_path() / "upc_v1.db").string();
    std::error_code ec;
    for (auto ext : {"", "-wal", "-shm"}) std::filesystem::remove(path + ext, ec);
    { // a database as version 0.1.0 wrote it
        sqlite3* db = nullptr;
        sqlite3_open(path.c_str(), &db);
        sqlite3_exec(db,
                     "CREATE TABLE records (key TEXT PRIMARY KEY NOT NULL, first_played INTEGER NOT NULL DEFAULT 0,"
                     "last_played INTEGER NOT NULL DEFAULT 0, added INTEGER NOT NULL DEFAULT 0, playcount INTEGER NOT "
                     "NULL DEFAULT 0, rating INTEGER NOT NULL DEFAULT 0, loved INTEGER NOT NULL DEFAULT 0, artist TEXT "
                     "NOT NULL DEFAULT '', title TEXT NOT NULL DEFAULT '', album TEXT NOT NULL DEFAULT '', path TEXT "
                     "NOT NULL DEFAULT ''); INSERT INTO records (key, playcount) VALUES ('old', 7); PRAGMA "
                     "user_version=1;",
                     nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    {
        upc::store s;
        s.open(path);
        CHECK(s.get("old") && s.get("old")->playcount == 7 && s.get("old")->subsong == 0);
        CHECK(s.get_meta("key_pattern").empty());
        s.set_meta("key_pattern", "%title%");
        upc::record r = plays(2);
        r.subsong = 3;
        s.put_many({{"new", r}}, {"old"});
        CHECK(!s.get("old") && s.size() == 1);
    }
    {
        upc::store s;
        s.open(path);
        CHECK(s.get_meta("key_pattern") == "%title%");
        CHECK(!s.get("old") && s.get("new") && s.get("new")->subsong == 3 && s.get("new")->playcount == 2);
    }
    for (auto ext : {"", "-wal", "-shm"}) std::filesystem::remove(path + ext, ec);
}

static int64_t row_id(const std::string& path, const char* key) {
    sqlite3* db = nullptr;
    sqlite3_open(path.c_str(), &db);
    sqlite3_stmt* q = nullptr;
    sqlite3_prepare_v2(db, "SELECT id FROM records WHERE key = ?", -1, &q, nullptr);
    sqlite3_bind_text(q, 1, key, -1, SQLITE_TRANSIENT);
    int64_t id = sqlite3_step(q) == SQLITE_ROW ? sqlite3_column_int64(q, 0) : -1;
    sqlite3_finalize(q);
    sqlite3_close(db);
    return id;
}

// Records get an integer id on upgrade, and rewriting a record keeps it.
static void test_store_ids() {
    const auto path = (std::filesystem::temp_directory_path() / "upc_ids.db").string();
    std::error_code ec;
    for (auto ext : {"", "-wal", "-shm", "-before-ids.bak"}) std::filesystem::remove(path + ext, ec);
    { // a version 2 database: text key only
        sqlite3* db = nullptr;
        sqlite3_open(path.c_str(), &db);
        sqlite3_exec(db,
                     "CREATE TABLE records (key TEXT PRIMARY KEY NOT NULL, first_played INTEGER NOT NULL DEFAULT 0,"
                     "last_played INTEGER NOT NULL DEFAULT 0, added INTEGER NOT NULL DEFAULT 0, playcount INTEGER NOT "
                     "NULL DEFAULT 0, rating INTEGER NOT NULL DEFAULT 0, loved INTEGER NOT NULL DEFAULT 0, artist TEXT "
                     "NOT NULL DEFAULT '', title TEXT NOT NULL DEFAULT '', album TEXT NOT NULL DEFAULT '', path TEXT "
                     "NOT NULL DEFAULT '', subsong INTEGER NOT NULL DEFAULT 0);"
                     "INSERT INTO records (key, playcount) VALUES ('a', 3); INSERT INTO records (key, playcount) VALUES "
                     "('b', 5); PRAGMA user_version=2;",
                     nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }
    int64_t id_a = 0;
    {
        upc::store s;
        s.open(path);
        CHECK(s.size() == 2 && s.get("a")->playcount == 3 && s.get("b")->playcount == 5);
        CHECK(std::filesystem::exists(path + "-before-ids.bak"));
        id_a = row_id(path, "a");
        CHECK(id_a > 0 && row_id(path, "b") > 0 && id_a != row_id(path, "b"));
        s.put_many({{"a", plays(9)}});
        CHECK(s.get("a")->playcount == 9);
    }
    CHECK(row_id(path, "a") == id_a);
    {
        upc::store s; // already upgraded: opens again without another backup or change
        std::filesystem::remove(path + "-before-ids.bak", ec);
        s.open(path);
        CHECK(s.size() == 2 && !std::filesystem::exists(path + "-before-ids.bak"));
    }
    for (auto ext : {"", "-wal", "-shm", "-before-ids.bak"}) std::filesystem::remove(path + ext, ec);
}

// Skips follow the record like plays do.
static void test_store_skips() {
    const auto path = (std::filesystem::temp_directory_path() / "upc_skips.db").string();
    std::error_code ec;
    for (auto ext : {"", "-wal", "-shm"}) std::filesystem::remove(path + ext, ec);
    {
        upc::store s;
        s.open(path);
        CHECK(s.skip_count("a") == 0 && s.last_skip("a") == 0);
        s.put_many({{"a", plays(0)}, {"b", plays(0)}}, {}, {}, {}, {{"a", 300}, {"a", 100}, {"b", 500}});
        CHECK(s.skip_count("a") == 2 && s.last_skip("a") == 300 && s.skip_count("b") == 1);
        s.put_many({{"a", plays(1)}}); // a rewrite keeps them
        CHECK(s.skip_count("a") == 2);
    }
    {
        upc::store s;
        s.open(path); // loaded from disk
        CHECK(s.skip_count("a") == 2 && s.last_skip("b") == 500);
        // a -> c moves (a removed), b -> d copies (b stays)
        s.put_many({{"c", plays(1)}, {"d", plays(1)}}, {"a"}, {{"a", "c"}, {"b", "d"}});
        CHECK(s.skip_count("a") == 0 && s.skip_count("c") == 2 && s.skip_count("b") == 1 && s.skip_count("d") == 1);
        s.put_many({}, {"c"}); // removed with its record
        CHECK(s.skip_count("c") == 0);
    }
    {
        upc::store s;
        s.open(path);
        CHECK(s.skip_count("c") == 0 && s.skip_count("b") == 1 && s.skip_count("d") == 1);
    }
    for (auto ext : {"", "-wal", "-shm"}) std::filesystem::remove(path + ext, ec);
}

// Play history: logged with the record, follows moves, copies on shared keys, goes with removed records.
static void test_store_plays() {
    const auto path = (std::filesystem::temp_directory_path() / "upc_plays.db").string();
    std::error_code ec;
    for (auto ext : {"", "-wal", "-shm"}) std::filesystem::remove(path + ext, ec);
    {
        upc::store s;
        s.open(path);
        s.put_many({{"a", plays(1)}, {"b", plays(1)}}, {}, {}, {{"a", 300}, {"a", 100}, {"b", 500}});
        CHECK((s.play_times("a") == std::vector<int64_t>{100, 300}));
        CHECK(s.plays_since("a", 100) == 2 && s.plays_since("a", 101) == 1 && s.plays_since("a", 301) == 0);
        CHECK(s.plays_since("nobody", 0) == 0 && s.play_times("nobody").empty());
        s.put_many({{"a", plays(2)}}); // a rewrite keeps the history
        CHECK(s.play_times("a").size() == 2);
    }
    {
        upc::store s;
        s.open(path); // loaded from disk
        CHECK((s.play_times("a") == std::vector<int64_t>{100, 300}) && s.play_times("b").size() == 1);
        // a -> c is a move (a removed), b -> d a copy (b stays)
        s.put_many({{"c", plays(2)}, {"d", plays(1)}}, {"a"}, {{"a", "c"}, {"b", "d"}, {"c", "c"}});
        CHECK(s.play_times("a").empty() && s.play_times("c").size() == 2);
        CHECK(s.play_times("b").size() == 1 && s.play_times("d").size() == 1);
        // two records merged into one
        s.put_many({{"d", plays(2)}}, {"c"}, {{"c", "d"}});
        CHECK((s.play_times("d") == std::vector<int64_t>{100, 300, 500}));
    }
    {
        upc::store s;
        s.open(path);
        CHECK((s.play_times("d") == std::vector<int64_t>{100, 300, 500}) && s.play_times("c").empty());
        s.put_many({}, {"d"}); // removed with no move: history goes too
        CHECK(s.play_times("d").empty());
    }
    {
        upc::store s;
        s.open(path);
        CHECK(s.play_times("d").empty() && s.play_times("b").size() == 1 && s.size() == 1);
    }
    for (auto ext : {"", "-wal", "-shm"}) std::filesystem::remove(path + ext, ec);
}

int main() {
    test_threshold();
    test_track_end_mode();
    test_basic_logging();
    test_refresh_does_not_reset();
    test_skips();
    test_backward_seek_adds_nothing();
    test_forward_seek_not_credited();
    test_unreported_jump_capped_by_real_time();
    test_busy_foobar_still_counts();
    test_backward_tick_without_seek();
    test_stop_and_new_track_reset();
    test_threshold_equals_length();
    test_finish_far_from_end();
    test_fractional_ticks();
    test_resume_mid_track();
    test_reannounce_mid_song();
    test_refresh_sequence();
    test_time_format();
    test_merge();
    test_combine();
    test_rekey();
    test_scan_for_cleanup();
    test_pick_auto_clean();
    test_backups_to_prune();
    test_parse_datetime();
    test_parse_played_times();
    test_lastfm_parse();
    test_lastfm_store();
    test_listenbrainz();
    try {
        test_read_playcount2003();
        test_store();
        test_store_v2();
        test_store_ids();
        test_store_plays();
        test_store_skips();
    } catch (std::exception& e) {
        std::printf("FAILED store: %s\n", e.what());
        ++g_failed;
    }
    std::printf(g_failed ? "%d check(s) failed\n" : "All tests passed\n", g_failed);
    return g_failed ? 1 : 0;
}
