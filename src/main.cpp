#include "stdafx.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>

#include "config.h"
#include "core.h"
#include "import_core.h"
#include "maintenance.h"
#include "play_counter.h"
#include "store.h"
#include "time_format.h"
#include "version.h"

DECLARE_COMPONENT_VERSION("Ultimate Playcount", UPC_VERSION_STRING,
                          "Counts plays by time actually listened, ignoring tag and info refreshes of the playing "
                          "track.\nReplaces foo_playcount_2003 and foo_playcount.\n\nhttps://github.com/r1vage/"
                          "ultimate-playcount");
VALIDATE_COMPONENT_FILENAME("foo_ultimate_playcount.dll");

namespace cfg = upc_cfg;

namespace upc_fb {

upc::store g_store;

titleformat_object::ptr compile(const char* pattern) {
    titleformat_object::ptr obj;
    titleformat_compiler::get()->compile_force(obj, pattern);
    return obj;
}

// The key pattern in use since startup (a changed setting applies after restart).
const pfc::string8& active_key_pattern() {
    static const pfc::string8 pattern =
        cfg::ignore_case.get() ? pfc::string8(PFC_string_formatter() << "$lower(" << cfg::key.get() << ")") : cfg::key.get();
    return pattern;
}

const titleformat_object::ptr& key_script() {
    static const auto script = compile(active_key_pattern());
    return script;
}

int64_t unix_now() { return static_cast<int64_t>(std::time(nullptr)); }

bool is_ignored(const metadb_handle_ptr& track) {
    return cfg::ignore_outside.get() && !library_manager::get()->is_item_in_library(track);
}

// Runs on the file's own info only (no display field providers), so our fields can't feed back into the key.
bool key_of(const metadb_handle_ptr& track, const metadb_v2::rec_t& rec, std::string& out) {
    if (!rec.info.is_valid()) return false;
    pfc::string8 key;
    key_script()->run_simple(track->get_location(), &rec.info->info(), key);
    out = key.c_str();
    return true;
}

bool key_of(const metadb_handle_ptr& track, std::string& out) { return key_of(track, track->query_v2_(), out); }

std::string native_path(const metadb_handle_ptr& track) {
    pfc::string8 native;
    return filesystem::g_get_native_path(track->get_path(), native) ? native.c_str() : track->get_path();
}

void set_label(upc::record& r, const metadb_handle_ptr& track) {
    static const auto artist = compile("%artist%"), title = compile("%title%"), album = compile("%album%");
    pfc::string8 s;
    track->format_title(nullptr, s, artist, nullptr);
    r.artist = s.c_str();
    track->format_title(nullptr, s, title, nullptr);
    r.title = s.c_str();
    track->format_title(nullptr, s, album, nullptr);
    r.album = s.c_str();
    r.path = native_path(track);
    r.subsong = track->get_subsong_index();
}

bool names_of(const metadb_handle_ptr& track, std::string& artist, std::string& title) {
    static const auto artist_script = compile("%artist%"), title_script = compile("%title%");
    pfc::string8 s;
    track->format_title(nullptr, s, artist_script, nullptr);
    artist = s.c_str();
    track->format_title(nullptr, s, title_script, nullptr);
    title = s.c_str();
    return !artist.empty() && !title.empty();
}

// Every track foobar2000 knows right now: library plus all playlists.
metadb_handle_list known_tracks() {
    metadb_handle_list tracks, items;
    library_manager::get()->get_all_items(tracks);
    auto playlists = playlist_manager::get();
    for (size_t i = 0, n = playlists->get_playlist_count(); i < n; ++i) {
        playlists->playlist_get_all_items(i, items);
        tracks += items;
    }
    tracks.sort_by_pointer_remove_duplicates();
    return tracks;
}

std::filesystem::path fs_path(const char* utf8) { return std::filesystem::path(pfc::stringcvt::string_wide_from_utf8(utf8).get_ptr()); }

// All backups live in this folder, next to the database.
std::filesystem::path backup_folder() {
    return fs_path(filesystem::g_get_native_path(core_api::pathInProfile("ultimate_playcount_backups")));
}

// Deletes the oldest backups beyond the "keep at most" setting, per kind.
void prune_backups() {
    std::error_code ec;
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(backup_folder(), ec))
        if (entry.is_regular_file(ec)) names.push_back(entry.path().filename().string());
    for (const auto& name : upc::backups_to_prune(names, static_cast<size_t>(cfg::backup_keep.get())))
        std::filesystem::remove(backup_folder() / name, ec);
}

// Copies the database into the backup folder as ultimate_playcount-<kind>-<date>.db. Throws on failure.
pfc::string8 backup_as(const char* kind) {
    std::error_code ec;
    std::filesystem::create_directories(backup_folder(), ec);
    char stamp[32];
    const std::tm tm = upc::local_tm(unix_now());
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    const auto path = backup_folder() / (std::string("ultimate_playcount-") + kind + "-" + stamp + ".db");
    const pfc::string8 utf8 = pfc::stringcvt::string_utf8_from_wide(path.c_str()).get_ptr();
    g_store.backup_to(utf8.c_str());
    prune_backups();
    return utf8;
}

pfc::string8 make_backup(const char* reason) { return backup_as(PFC_string_formatter() << "before-" << reason); }

} // namespace upc_fb

namespace {

using namespace upc_fb;

// Applies `change` to the records of all given tracks (deduplicated by key) and refreshes their display.
// `known_key`: the key worked out earlier, used when foobar2000 has no tag info for the track right now.
template <typename F>
void update_records(metadb_handle_list_cref tracks, F change, const std::string& known_key = {},
                    bool log_play = false, bool log_skip = false) {
    if (!g_store.is_open()) return;
    std::set<std::string> seen;
    std::vector<std::pair<std::string, upc::record>> batch;
    std::vector<std::pair<std::string, int64_t>> plays, skips;
    const auto now = unix_now();
    for (const auto& track : tracks) {
        if (is_ignored(track)) continue;
        std::string key;
        const bool has_info = key_of(track, key);
        if (!has_info) {
            if (known_key.empty()) {
                FB2K_console_formatter() << log_prefix << "Not saved, no tag info available right now: " << track;
                continue;
            }
            key = known_key;
        }
        if (!seen.insert(key).second) continue;
        auto rec = g_store.get(key).value_or(upc::record{});
        if (!rec.added) rec.added = now;
        change(rec, now);
        if (has_info) set_label(rec, track); // without info the label would come out blank
        if (log_play) plays.emplace_back(key, now);
        if (log_skip) skips.emplace_back(key, now);
        batch.emplace_back(std::move(key), std::move(rec));
    }
    if (batch.empty()) return;
    try {
        g_store.put_many(batch, {}, {}, plays, skips);
    } catch (const std::exception& e) {
        FB2K_console_formatter() << log_prefix << "Could not save: " << e.what();
        static bool told = false; // once per session, so a stuck database doesn't nag on every play
        if (!told) {
            told = true;
            popup_message::g_complain("Ultimate Playcount: could not save (plays or ratings may be lost until this "
                                      "is fixed; details in the console)",
                                      e);
        }
        return;
    }
    // shortcut: refreshes only these handles; other tracks sharing a key update on their next refresh.
    metadb_io::get()->dispatch_refresh(tracks);
}

// ---- Database lifetime ----

class init_stage_impl : public init_stage_callback {
public:
    void on_init_stage(t_uint32 stage) override {
        if (stage != init_stages::after_config_read) return;
        const auto path = filesystem::g_get_native_path(core_api::pathInProfile("ultimate_playcount.db"));
        try {
            g_store.open(path.c_str());
        } catch (const std::exception& e) {
            // Not counting silently would lose plays: make it visible.
            FB2K_console_formatter() << log_prefix << "Database not available, nothing will be counted: " << e.what();
            popup_message::g_complain("Ultimate Playcount: database not available", e);
            return;
        }
        tidy_old_backups();
        auto_backup();
    }

private:
    // Earlier versions saved backups next to the database: move them into the backup folder.
    static void tidy_old_backups() {
        std::error_code ec;
        const auto profile = fs_path(filesystem::g_get_native_path(core_api::get_profile_path()));
        std::filesystem::create_directories(backup_folder(), ec);
        for (const auto& entry : std::filesystem::directory_iterator(profile, ec)) {
            const auto name = entry.path().filename().string();
            if (name.rfind("ultimate_playcount-before-", 0) == 0 && entry.path().extension() == ".db")
                std::filesystem::rename(entry.path(), backup_folder() / name, ec);
        }
    }

    static void auto_backup() {
        if (!cfg::auto_backup.get() || g_store.size() == 0) return;
        try {
            const auto last = std::atoll(g_store.get_meta("last_auto_backup").c_str());
            if (unix_now() - last < cfg::auto_backup_days.get() * 86400) return;
            const auto copy = backup_as("auto");
            g_store.set_meta("last_auto_backup", std::to_string(unix_now()));
            FB2K_console_formatter() << log_prefix << "Automatic backup: " << copy;
        } catch (const std::exception& e) {
            FB2K_console_formatter() << log_prefix << "Automatic backup failed: " << e.what();
        }
    }
};
FB2K_SERVICE_FACTORY(init_stage_impl);

class initquit_impl : public initquit {
public:
    void on_quit() override { g_store.close(); }
};
FB2K_SERVICE_FACTORY(initquit_impl);

// ---- Play counting ----

class playback_hook : public play_callback_static {
public:
    unsigned get_flags() override {
        // edited / dynamic_info_track / starting / pause are only logged (event diagnostics), never counted.
        // Plain dynamic_info isn't subscribed: it's the live bitrate, sent about once a second, and only noise.
        return flag_on_playback_new_track | flag_on_playback_stop | flag_on_playback_seek | flag_on_playback_time |
               flag_on_playback_starting | flag_on_playback_pause | flag_on_playback_edited |
               flag_on_playback_dynamic_info_track;
    }

    void on_playback_new_track(metadb_handle_ptr track) override {
        const double position = playback_control::get()->playback_get_position();
        if (log_events())
            FB2K_console_formatter() << log_prefix << "Event: new_track at " << pfc::format_float(position, 0, 1) << " s, "
                    << (track == m_track ? "SAME file as before" : "different file") << ": " << track;
        if (m_counter.reannounced(track == m_track, position)) {
            if (debug())
                FB2K_console_formatter() << log_prefix << "Same track announced again at "
                                         << pfc::format_float(position, 0, 1) << " s: keeping the count ("
                                         << pfc::format_float(m_counter.listened(), 0, 1) << " s listened)";
            return;
        }
        end_track("next track", true);
        begin_track(track);
    }
    void on_playback_stop(play_control::t_stop_reason reason) override {
        if (log_events()) {
            static const char* const names[] = {"user", "eof", "starting_another", "shutting_down"};
            FB2K_console_formatter() << log_prefix << "Event: stop, reason "
                    << (static_cast<unsigned>(reason) < std::size(names) ? names[reason] : "other");
        }
        if (!upc::play_counter::stop_ends_track(reason == play_control::stop_reason_starting_another)) return;
        end_track(reason == play_control::stop_reason_eof ? "end of playlist" : "stopped",
                  reason == play_control::stop_reason_user);
    }
    void on_playback_seek(double time) override {
        if (log_events()) FB2K_console_formatter() << log_prefix << "Event: seek to " << pfc::format_float(time, 0, 1) << " s";
        m_counter.on_seek(time);
    }
    void on_playback_time(double time) override {
        const bool reached = m_counter.on_time(time, clock_now());
        if (debug() && m_counter.active() && m_counter.last_step() > upc::play_counter::max_step)
            FB2K_console_formatter() << log_prefix << "Time jumped " << pfc::format_float(m_counter.last_step(), 0, 1)
                                     << " s since the last update (foobar2000 was busy); counted, listened "
                                     << pfc::format_float(m_counter.listened(), 0, 1) << " s";
        if (reached) log_play();
    }

    void on_playback_starting(play_control::t_track_command command, bool paused) override {
        if (log_events()) FB2K_console_formatter() << log_prefix << "Event: starting, command " << (unsigned)command << (paused ? ", paused" : "");
    }
    void on_playback_pause(bool state) override {
        if (log_events()) FB2K_console_formatter() << log_prefix << "Event: " << (state ? "pause" : "unpause");
    }
    void on_playback_edited(metadb_handle_ptr track) override {
        if (log_events()) FB2K_console_formatter() << log_prefix << "Event: edited (ignored for counting): " << track;
    }
    void on_playback_dynamic_info(const file_info&) override {}
    void on_playback_dynamic_info_track(const file_info&) override {
        if (log_events()) FB2K_console_formatter() << log_prefix << "Event: dynamic_info_track (ignored for counting)";
    }
    void on_volume_change(float) override {}

private:
    static bool debug() { return cfg::debug.get(); }
    static double clock_now() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static bool log_events() { return cfg::log_events.get(); }

    void begin_track(const metadb_handle_ptr& track) {
        m_track = track;
        m_key.clear();
        if (!g_store.is_open()) return;
        if (is_ignored(track)) {
            if (debug()) FB2K_console_formatter() << log_prefix << "Not counting (outside library): " << track;
            return;
        }
        const auto mode = static_cast<upc::count_mode>(cfg::count_mode.get());
        const double value = static_cast<double>(
            mode == upc::count_mode::percent ? cfg::count_percent.get() : cfg::count_seconds.get());
        const double length = track->get_length();
        const double threshold = upc::threshold_for(mode, value, length);
        // Worked out now, while the tags are surely readable: the play is saved under it even if foobar2000
        // has no info for the file at the moment the threshold is reached (e.g. the file is being rewritten).
        key_of(track, m_key);
        lastfm_track_started(track);
        m_counter.start(threshold, length, playback_control::get()->playback_get_position(), clock_now());
        if (debug()) {
            console::formatter line; // prints when it goes out of scope
            line << log_prefix << "Play start: " << track << " | length " << length << " s | counts ";
            if (std::isinf(threshold)) line << "on reaching the end";
            else line << "after " << pfc::format_float(threshold, 0, 1) << " s of listening";
        }
    }

    // `by_user`: the track was left because of something the user did (stop, another track), not the end of the
    // playlist or foobar2000 closing.
    void end_track(const char* why, bool by_user) {
        if (!m_counter.active()) return;
        if (m_counter.finish()) log_play();
        if (debug() && !m_counter.logged())
            FB2K_console_formatter() << log_prefix << "Not logged (" << why << "): listened "
                                     << pfc::format_float(m_counter.listened(), 0, 1) << " of "
                                     << pfc::format_float(m_counter.threshold(), 0, 1) << " s";
        if (by_user && cfg::count_skips.get() && m_counter.skipped()) log_skip();
        m_counter.stop();
        lastfm_track_ended(m_track);
    }

    void log_skip() {
        if (debug())
            FB2K_console_formatter() << log_prefix << "Skipped after " << pfc::format_float(m_counter.listened(), 0, 1)
                                     << " s: " << m_track;
        update_records(pfc::list_single_ref_t<metadb_handle_ptr>(m_track), [](upc::record&, int64_t) {}, m_key, false,
                       true);
    }

    void log_play() {
        if (debug())
            FB2K_console_formatter() << log_prefix << "Logged: listened "
                                     << pfc::format_float(m_counter.listened(), 0, 1) << " s >= threshold "
                                     << pfc::format_float(m_counter.threshold(), 0, 1) << " s: " << m_track;
        update_records(pfc::list_single_ref_t<metadb_handle_ptr>(m_track), [](upc::record& r, int64_t now) {
            ++r.playcount;
            r.last_played = now;
            if (!r.first_played) r.first_played = now;
        }, m_key, true);
    }

    upc::play_counter m_counter;
    metadb_handle_ptr m_track;
    std::string m_key; // record key of m_track, from when it started
};
FB2K_SERVICE_FACTORY(playback_hook);

// ---- Key pattern changes: move the data along ----

constexpr char meta_key_pattern[] = "key_pattern";

void check_key_pattern() {
    if (!g_store.is_open()) return;
    const std::string current = active_key_pattern().c_str();
    std::string stored;
    try {
        stored = g_store.get_meta(meta_key_pattern);
        if (stored == current) return;
        if (stored.empty() || g_store.size() == 0) { // first run of this version: nothing to move
            g_store.set_meta(meta_key_pattern, current);
            return;
        }
    } catch (const std::exception& e) {
        FB2K_console_formatter() << log_prefix << "Could not check the key pattern: " << e.what();
        return;
    }

    const auto old_script = compile(stored.c_str());
    const auto tracks = known_tracks();
    std::vector<std::pair<std::string, std::string>> moves;
    for (const auto& track : tracks) {
        const auto rec = track->query_v2_();
        std::string now_key;
        if (!key_of(track, rec, now_key)) continue;
        pfc::string8 old_key;
        old_script->run_simple(track->get_location(), &rec.info->info(), old_key);
        moves.emplace_back(old_key.c_str(), std::move(now_key));
    }
    const auto changes = upc::rekey(g_store.all(), moves);
    if (changes.puts.empty() && changes.removes.empty()) {
        try {
            g_store.set_meta(meta_key_pattern, current);
        } catch (const std::exception& e) {
            FB2K_console_formatter() << log_prefix << "Could not save the key pattern: " << e.what();
        }
        return;
    }

    // Only the case setting changed: say so instead of showing two near-identical patterns.
    const bool case_on = current == "$lower(" + stored + ")", case_off = stored == "$lower(" + current + ")";
    pfc::string_formatter msg;
    if (case_on)
        msg << "\"Ignore upper/lower case\" is now on, so your records need to move to lowercase keys.";
    else if (case_off)
        msg << "\"Ignore upper/lower case\" is now off, so your records need to move to keys with their original case.";
    else
        msg << "The record key pattern has changed.\n\nOld: " << stored.c_str() << "\nNew: " << current.c_str();
    msg << "\n\nMoving your data to the new keys writes " << changes.puts.size() << " records and removes "
        << changes.removes.size()
        << " old ones. Records that end up together have their play counts added up. Records of tracks foobar2000 "
           "doesn't know right now keep their old key.\n\nA backup is made first. Move the data now?\n\n(No: nothing "
           "changes and you'll be asked again at the next start. To go back, "
        << (case_on || case_off ? "switch \"Ignore upper/lower case\" back" : "set the old pattern")
        << " in Preferences > Tools > Ultimate Playcount > Records and fields.)";
    if (uMessageBox(core_api::get_main_window(), msg, "Ultimate Playcount: key pattern changed",
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    try {
        const auto backup = make_backup("rekey");
        g_store.put_many(changes.puts, changes.removes, moves);
        g_store.set_meta(meta_key_pattern, current);
        FB2K_console_formatter() << log_prefix << "Moved data to the new key pattern. Backup: " << backup;
    } catch (const std::exception& e) {
        popup_message::g_complain("Ultimate Playcount: moving data failed, nothing was changed", e);
        return;
    }
    metadb_io::get()->dispatch_refresh(tracks);
}

// ---- Tag edits: move the data along ----

class edit_hook : public metadb_io_edit_callback {
public:
    void on_edited(metadb_handle_list_cref items, t_infosref before, t_infosref after) override {
        if (!g_store.is_open() || !cfg::follow_edits.get()) return;
        std::vector<std::pair<std::string, std::string>> moves;
        std::set<std::string> leaving; // old keys that have a record and change
        std::map<std::string, metadb_handle_ptr> track_of; // new key -> edited track, for the label
        std::set<const metadb_handle*> edited;
        for (size_t i = 0, n = items.get_count(); i < n; ++i) {
            if (!before[i] || !after[i]) continue;
            pfc::string8 from, to;
            key_script()->run_simple(items[i]->get_location(), before[i], from);
            key_script()->run_simple(items[i]->get_location(), after[i], to);
            edited.insert(items[i].get_ptr());
            if (from == to || !g_store.get(from.c_str())) continue;
            leaving.insert(from.c_str());
            track_of.emplace(to.c_str(), items[i]);
            moves.emplace_back(from.c_str(), to.c_str());
        }
        if (leaving.empty()) return;
        // Tracks not edited that still share an old key keep their own copy of the record. Read in parallel: this
        // runs on every edit, over the whole library.
        const auto tracks = known_tracks();
        std::vector<std::string> keys(tracks.get_count());
        metadb_v2::get()->queryMultiParallel_(
            tracks, [&](size_t i, const metadb_v2::rec_t& rec) { key_of(tracks[i], rec, keys[i]); });
        for (size_t i = 0; i < keys.size(); ++i)
            if (!edited.count(tracks[i].get_ptr()) && leaving.count(keys[i])) moves.emplace_back(keys[i], keys[i]);
        // Only the records involved, not a copy of the whole database.
        std::vector<std::pair<std::string, upc::record>> involved;
        std::set<std::string> involved_keys;
        for (const auto& [from, to] : moves)
            for (const auto* k : {&from, &to})
                if (involved_keys.insert(*k).second)
                    if (auto r = g_store.get(*k)) involved.emplace_back(*k, std::move(*r));
        auto changes = upc::rekey(involved, moves);
        for (auto& [key, rec] : changes.puts)
            if (auto it = track_of.find(key); it != track_of.end()) set_label(rec, it->second);
        try {
            g_store.put_many(changes.puts, changes.removes, moves);
        } catch (const std::exception& e) {
            FB2K_console_formatter() << log_prefix << "Could not move data to the edited tags: " << e.what();
            return;
        }
        if (cfg::debug.get())
            FB2K_console_formatter() << log_prefix << "Tags edited: " << changes.puts.size() << " records moved to the new tags";
        metadb_io::get()->dispatch_refresh(items);
    }
};
FB2K_SERVICE_FACTORY(edit_hook);

class library_hook : public library_callback_v2 {
public:
    void on_library_initialized() override {
        check_key_pattern();
        run_auto_clean();
    }
    void on_items_modified_v2(metadb_handle_list_cref, metadb_io_callback_v2_data&) override {}

    void on_items_added(metadb_handle_list_cref items) override {
        if (!g_store.is_open()) return;
        const auto now = unix_now();
        std::set<std::string> seen;
        std::vector<std::pair<std::string, upc::record>> batch;
        for (const auto& track : items) {
            std::string key;
            if (!key_of(track, key) || g_store.get(key) || !seen.insert(key).second) continue;
            upc::record r;
            r.added = now;
            set_label(r, track);
            batch.emplace_back(std::move(key), std::move(r));
        }
        if (batch.empty()) return;
        try {
            g_store.put_many(batch);
        } catch (const std::exception& e) {
            FB2K_console_formatter() << log_prefix << "Could not save added dates: " << e.what();
        }
    }
    void on_items_removed(metadb_handle_list_cref) override {}
    void on_items_modified(metadb_handle_list_cref) override {}
};
FB2K_SERVICE_FACTORY(library_hook);

// ---- Title formatting fields ----

enum class value { playcount, rating, loved, date, ts, ago, ago2, now, now_ts, plays_in_days, played_times,
                   played_dates, skips_count, skips_last, skips_last_ts, lastfm_count, lastfm_first, lastfm_last, lastfm_times_js, lastfm_times,
                   lastfm_loved, lastfm_first_ts, lastfm_last_ts, lastfm_first_ago, lastfm_last_ago,
                   listens_count, listens_first, listens_last, listens_times_js, listens_times };
enum class date { none, added, first_played, last_played };

struct field_def {
    const char* name; // without prefix
    value what;
    date which = date::none;
    bool legacy = false; // foo_playcount name, no prefix
    int days = 0;        // plays_in_days: the window
    bool eps = false;    // Enhanced Playback Statistics name, no prefix
    bool lfm = false;    // Last.fm Playcount Sync name, no prefix
};

constexpr field_def all_fields[] = {
    {"playcount", value::playcount},
    {"rating", value::rating},
    {"loved", value::loved},
    {"added", value::date, date::added},
    {"first_played", value::date, date::first_played},
    {"last_played", value::date, date::last_played},
    {"added_ts", value::ts, date::added},
    {"first_played_ts", value::ts, date::first_played},
    {"last_played_ts", value::ts, date::last_played},
    {"added_ago", value::ago, date::added},
    {"first_played_ago", value::ago, date::first_played},
    {"last_played_ago", value::ago, date::last_played},
    {"added_ago2", value::ago2, date::added},
    {"first_played_ago2", value::ago2, date::first_played},
    {"last_played_ago2", value::ago2, date::last_played},
    {"now", value::now},
    {"now_ts", value::now_ts},
    {"plays_7d", value::plays_in_days, date::none, false, 7},
    {"plays_30d", value::plays_in_days, date::none, false, 30},
    {"plays_365d", value::plays_in_days, date::none, false, 365},
    {"played_times_js", value::played_times},
    {"skipcount", value::skips_count},
    {"last_skipped", value::skips_last},
    {"last_skipped_ts", value::skips_last_ts},
    {"played_times_js", value::played_times, date::none, false, 0, true},
    {"played_times", value::played_dates, date::none, false, 0, true},
    {"listenbrainz_playcount", value::listens_count},
    {"listenbrainz_first_played", value::listens_first},
    {"listenbrainz_last_played", value::listens_last},
    {"listenbrainz_played_times_js", value::listens_times_js},
    {"listenbrainz_played_times", value::listens_times},
    {"lastfm_playcount", value::lastfm_count},
    {"lastfm_loved", value::lastfm_loved},
    {"lastfm_loved", value::lastfm_loved, date::none, false, 0, true},
    {"lastfm_first_played", value::lastfm_first},
    {"lastfm_last_played", value::lastfm_last},
    {"lastfm_played_times_js", value::lastfm_times_js},
    {"lastfm_played_times", value::lastfm_times},
    {"lastfm_play_count", value::lastfm_count, date::none, false, 0, true},
    {"lastfm_first_played", value::lastfm_first, date::none, false, 0, true},
    {"lastfm_last_played", value::lastfm_last, date::none, false, 0, true},
    {"lastfm_played_times_js", value::lastfm_times_js, date::none, false, 0, true},
    {"lastfm_played_times", value::lastfm_times, date::none, false, 0, true},
    {"first_played_enhanced", value::date, date::first_played, false, 0, true},
    {"last_played_enhanced", value::date, date::last_played, false, 0, true},
    {"added_enhanced", value::date, date::added, false, 0, true},
    {"lfm_playcount", value::lastfm_count, date::none, false, 0, false, true},
    {"lfm_loved", value::lastfm_loved, date::none, false, 0, false, true},
    {"lfm_first_played", value::lastfm_first, date::none, false, 0, false, true},
    {"lfm_last_played", value::lastfm_last, date::none, false, 0, false, true},
    {"lfm_first_played_ts", value::lastfm_first_ts, date::none, false, 0, false, true},
    {"lfm_last_played_ts", value::lastfm_last_ts, date::none, false, 0, false, true},
    {"lfm_first_played_ago", value::lastfm_first_ago, date::none, false, 0, false, true},
    {"lfm_last_played_ago", value::lastfm_last_ago, date::none, false, 0, false, true},
    {"lfm_now", value::now, date::none, false, 0, false, true},
    {"lfm_now_ts", value::now_ts, date::none, false, 0, false, true},
    {"play_count", value::playcount, date::none, true},
    {"first_played", value::date, date::first_played, true},
    {"last_played", value::date, date::last_played, true},
    {"added", value::date, date::added, true},
    {"rating", value::rating, date::none, true},
};

struct field_list {
    std::vector<const field_def*> defs;
    std::vector<std::string> names;
};

const field_list& fields() {
    static const field_list list = [] {
        field_list l;
        const auto prefix = cfg::prefix.get();
        const bool legacy = cfg::legacy_names.get(), eps = cfg::enhanced_names.get(), lfm = cfg::lfm_names.get();
        for (const auto& f : all_fields) {
            if ((f.legacy && !legacy) || (f.eps && !eps) || (f.lfm && !lfm)) continue;
            l.defs.push_back(&f);
            l.names.push_back(f.legacy || f.eps || f.lfm ? f.name : std::string(prefix.c_str()) + f.name);
        }
        return l;
    }();
    return list;
}

// ListenBrainz listens of a track: those with its MusicBrainz recording id and those with exactly its artist
// and title (the first artist, or all artists joined with ", ").
std::vector<int64_t> listens_of(const metadb_v2::rec_t& rec) {
    if (!rec.info.is_valid()) return {};
    const auto& info = rec.info->info();
    const char* title = info.meta_get("title", 0);
    if (!title) return {};
    std::vector<std::string> keys;
    std::string all;
    const size_t artists = info.meta_get_count_by_name("artist");
    for (size_t i = 0; i < artists; ++i) {
        const char* artist = info.meta_get("artist", i);
        if (i == 0) keys.push_back(upc::match_key(artist, title));
        all += (i ? ", " : "") + std::string(artist);
    }
    if (artists > 1) keys.push_back(upc::match_key(all, title));
    const char* mbid = info.meta_get("musicbrainz_trackid", 0);
    return g_store.listen_times(keys, mbid ? mbid : "");
}

// With "Show the Last.fm or ListenBrainz play count instead of the local one when it is higher" (display only, the
// record is not changed): the highest count, the earliest first and latest last play.
std::optional<upc::record> with_external(std::optional<upc::record> r, const std::string& key,
                                         const metadb_v2::rec_t& rec) {
    auto raise = [&](uint32_t count, int64_t first, int64_t last) {
        if (!r) r.emplace();
        r->playcount = (std::max)(r->playcount, count);
        if (first && (!r->first_played || first < r->first_played)) r->first_played = first;
        if (last > r->last_played) r->last_played = last;
    };
    if (const auto t = g_store.get_lastfm_track(key))
        raise(t->playcount, t->times.empty() ? 0 : t->times.front(), t->times.empty() ? 0 : t->times.back());
    if (const auto times = listens_of(rec); !times.empty())
        raise(static_cast<uint32_t>(times.size()), times.front(), times.back());
    return r;
}

// Like Enhanced Playback Statistics: a JSON array of milliseconds, so scripts can parse it as it is.
std::string times_as_ms(const std::vector<int64_t>& times) {
    std::string js = "[";
    for (size_t i = 0; i < times.size(); ++i) js += (i ? "," : "") + std::to_string(times[i] * 1000);
    return js + "]";
}

std::string times_as_dates(const std::vector<int64_t>& times) {
    std::string js = "[";
    for (size_t i = 0; i < times.size(); ++i) js += (i ? ",\"" : "\"") + upc::format_date(times[i]) + "\"";
    return js + "]";
}

class field_provider : public metadb_display_field_provider_v2 {
public:
    t_uint32 get_field_count() override { return static_cast<t_uint32>(fields().defs.size()); }
    void get_field_name(t_uint32 index, pfc::string_base& out) override { out = fields().names[index].c_str(); }

    bool process_field(t_uint32 index, metadb_handle* handle, titleformat_text_out* out) override {
        return process_field_v2(index, handle, handle->query_v2_(), out);
    }

    bool process_field_v2(t_uint32 index, metadb_handle* handle, const metadb_v2::rec_t& rec,
                          titleformat_text_out* out) override {
        const field_def& f = *fields().defs[index];
        const auto now = unix_now();
        if (f.what == value::now) return write(out, upc::format_date(now));
        if (f.what == value::now_ts) return write_int(out, now);
        if (!g_store.is_open()) return false;

        std::string key;
        if (!key_of(handle, rec, key)) return false;
        if (f.what >= value::listens_count) { // ListenBrainz listens, counted here
            if (!g_store.listens_total()) return false;
            const auto times = listens_of(rec);
            switch (f.what) {
            case value::listens_count: return write_int(out, static_cast<int64_t>(times.size()));
            case value::listens_first: return !times.empty() && write(out, upc::format_date(times.front()));
            case value::listens_last: return !times.empty() && write(out, upc::format_date(times.back()));
            case value::listens_times_js: return !times.empty() && write(out, times_as_ms(times));
            default: return !times.empty() && write(out, times_as_dates(times));
            }
        }
        if (f.what >= value::lastfm_count) { // what Last.fm itself reports, stored by the background lookups
            if (f.what == value::lastfm_count || f.what == value::lastfm_loved) {
                const auto t = g_store.get_lastfm_summary(key);
                if (!t) return false;
                return f.what == value::lastfm_count ? write_int(out, t->playcount) : (t->loved && write_int(out, 1));
            }
            const auto t = g_store.get_lastfm_track(key);
            if (!t || t->times.empty()) return false;
            switch (f.what) {
            case value::lastfm_first: return write(out, upc::format_date(t->times.front()));
            case value::lastfm_last: return write(out, upc::format_date(t->times.back()));
            case value::lastfm_first_ts: return write_int(out, t->times.front());
            case value::lastfm_last_ts: return write_int(out, t->times.back());
            case value::lastfm_first_ago: return write(out, upc::format_ago(now - t->times.front()));
            case value::lastfm_last_ago: return write(out, upc::format_ago(now - t->times.back()));
            case value::lastfm_times_js: return write(out, times_as_ms(t->times));
            default: return write(out, times_as_dates(t->times));
            }
        }
        if (f.what == value::skips_count) return write_int(out, static_cast<int64_t>(g_store.skip_count(key))); // 0 if none
        if (f.what == value::skips_last || f.what == value::skips_last_ts) {
            const auto last = g_store.last_skip(key);
            if (!last) return false;
            return f.what == value::skips_last ? write(out, upc::format_date(last)) : write_int(out, last);
        }
        if (f.what == value::plays_in_days)
            return write_int(out, static_cast<int64_t>(g_store.plays_since(key, now - int64_t{f.days} * 86400)));
        if (f.what == value::played_dates) {
            const auto times = g_store.play_times(key);
            if (times.empty()) return false;
            return write(out, times_as_dates(times));
        }
        if (f.what == value::played_times) {
            const auto times = g_store.play_times(key);
            if (times.empty()) return false;
            return write(out, times_as_ms(times));
        }
        auto r = g_store.get_stats(key);
        if (cfg::lastfm_feeds.get()) r = with_external(std::move(r), key, rec);
        if (f.what == value::playcount) return write_int(out, r ? r->playcount : 0); // 0, like foo_playcount
        if (!r) return false;

        switch (f.what) {
        case value::rating: return r->rating && write_int(out, r->rating);
        case value::loved: return r->loved && write_int(out, 1);
        default: break;
        }
        const int64_t ts = f.which == date::added ? r->added
                         : f.which == date::first_played ? r->first_played
                                                         : r->last_played;
        if (!ts) return false;
        switch (f.what) {
        case value::date: return write(out, upc::format_date(ts));
        case value::ts: return write_int(out, ts);
        case value::ago: return write(out, upc::format_ago(now - ts));
        case value::ago2: return write(out, upc::format_ago2(ts, now));
        default: return false;
        }
    }

private:
    static bool write(titleformat_text_out* out, const std::string& s) {
        out->write(titleformat_inputtypes::meta, s.c_str());
        return true;
    }
    static bool write_int(titleformat_text_out* out, int64_t v) {
        out->write_int(titleformat_inputtypes::meta, v);
        return true;
    }
};
FB2K_SERVICE_FACTORY(field_provider);

// ---- Context menu: Ultimate Playcount/Rating/..., Ultimate Playcount/Love, Unlove ----

constexpr GUID guid_menu_root = {0x59dd9498, 0x2489, 0x4d53, {0x81, 0x5a, 0x03, 0x30, 0x44, 0x30, 0x33, 0x17}};
constexpr GUID guid_menu_rating = {0x63e92740, 0x25cc, 0x4b1b, {0xbd, 0x6a, 0x54, 0x23, 0x89, 0xba, 0x0d, 0x19}};
constexpr GUID guid_rating_items = {0x9a5a55bd, 0xfa53, 0x48e3, {0xa6, 0x41, 0xf3, 0x22, 0x03, 0xa1, 0xcd, 0xfa}};
constexpr GUID guid_love_items = {0xf14df445, 0xe208, 0x4322, {0x85, 0xb1, 0x8a, 0x91, 0x7c, 0xa5, 0xfc, 0x70}};

contextmenu_group_popup_factory g_menu_root(guid_menu_root, contextmenu_groups::root, "Ultimate Playcount");
contextmenu_group_popup_factory g_menu_rating(guid_menu_rating, guid_menu_root, "Rating");

GUID nth_guid(GUID base, unsigned n) {
    base.Data1 += n;
    return base;
}

class menu_rating : public contextmenu_item_simple {
public:
    enum { set1, set5 = set1 + 4, clear, increment, decrement, count };

    GUID get_parent() override { return guid_menu_rating; }
    unsigned get_num_items() override { return count; }
    void get_item_name(unsigned i, pfc::string_base& out) override {
        if (i <= set5) out = PFC_string_formatter() << "Set Rating to " << (i - set1 + 1);
        else if (i == clear) out = "Clear";
        else if (i == increment) out = "Increment Rating by 1";
        else out = "Decrement Rating by 1";
    }
    GUID get_item_guid(unsigned i) override { return nth_guid(guid_rating_items, i); }
    bool get_item_description(unsigned, pfc::string_base& out) override {
        out = "Sets the Ultimate Playcount rating of the selected tracks.";
        return true;
    }
    void context_command(unsigned i, metadb_handle_list_cref tracks, const GUID&) override {
        update_records(tracks, [i](upc::record& r, int64_t) {
            if (i <= set5) r.rating = i - set1 + 1;
            else if (i == clear) r.rating = 0;
            else if (i == increment) r.rating = (std::min)(r.rating + 1, 5u);
            else if (r.rating) --r.rating;
        });
    }
};
FB2K_SERVICE_FACTORY(menu_rating);

class menu_love : public contextmenu_item_simple {
public:
    GUID get_parent() override { return guid_menu_root; }
    unsigned get_num_items() override { return 2; }
    void get_item_name(unsigned i, pfc::string_base& out) override { out = i == 0 ? "Love" : "Unlove"; }
    GUID get_item_guid(unsigned i) override { return nth_guid(guid_love_items, i); }
    bool get_item_description(unsigned, pfc::string_base& out) override {
        out = "Marks or unmarks the selected tracks as loved.";
        return true;
    }
    void context_command(unsigned i, metadb_handle_list_cref tracks, const GUID&) override {
        update_records(tracks, [i](upc::record& r, int64_t) { r.loved = i == 0; });
    }
};
FB2K_SERVICE_FACTORY(menu_love);

constexpr GUID guid_lastfm_items = {0xe41b6a07, 0x2d58, 0x4f93, {0xa0, 0x1c, 0x75, 0x83, 0xb9, 0x4e, 0xd2, 0x06}};

class menu_lastfm : public contextmenu_item_simple {
public:
    GUID get_parent() override { return guid_menu_root; }
    unsigned get_num_items() override { return 2; }
    void get_item_name(unsigned i, pfc::string_base& out) override {
        out = i == 0 ? "Update from Last.fm" : "Forget Last.fm numbers";
    }
    GUID get_item_guid(unsigned i) override { return nth_guid(guid_lastfm_items, i); }
    bool get_item_description(unsigned i, pfc::string_base& out) override {
        out = i == 0 ? "Asks Last.fm for the play count, loved mark and play dates of the selected tracks."
                     : "Removes the stored Last.fm numbers of the selected tracks.";
        return true;
    }
    void context_command(unsigned i, metadb_handle_list_cref tracks, const GUID&) override {
        if (i == 0) lastfm_update(tracks);
        else lastfm_forget(tracks);
    }
};
FB2K_SERVICE_FACTORY(menu_lastfm);

// ---- Properties dialog: "Ultimate Playcount" section ----

class properties_section : public track_property_provider_v2 {
public:
    // Three sections, one per source, never mixed: the plays counted here, Last.fm's numbers, ListenBrainz's listens.
    // Several selected tracks: totals (counts added up, earliest / latest dates); rating and loved only for one.
    void enumerate_properties(trackListRef tracks, track_property_callback& out) override { enumerate(tracks, out, nullptr); }
    void enumerate_properties_v2(trackListRef tracks, track_property_callback_v2& out) override {
        enumerate(tracks, out, &out);
    }
    bool is_our_tech_info(const char*) override { return false; }

private:
    static constexpr char group[] = "Ultimate Playcount", group_lastfm[] = "Ultimate Playcount: Last.fm",
                          group_listens[] = "Ultimate Playcount: ListenBrainz";

    struct dates {
        int64_t first = 0, last = 0;
        void add(int64_t first_at, int64_t last_at) {
            first = upc::earliest(first, first_at);
            last = (std::max)(last, last_at);
        }
    };

    // `wanted`: null = every section.
    static void enumerate(trackListRef tracks, track_property_callback& out, track_property_callback_v2* wanted) {
        if (!g_store.is_open()) return;
        const auto now = unix_now();
        std::set<std::string> keys;
        upc::record sum;
        bool any = false;
        size_t dated = 0, last_7 = 0, last_30 = 0, last_365 = 0, skips = 0;
        int64_t last_skip = 0;
        uint32_t lastfm_count = 0;
        size_t lastfm_tracks = 0, listens = 0;
        bool lastfm_loved = false;
        dates lastfm_dates, listen_dates;
        for (const auto& track : tracks) {
            const auto rec = track->query_v2_();
            std::string key;
            if (!key_of(track, rec, key) || !keys.insert(key).second) continue;
            if (const auto r = g_store.get_stats(key)) {
                sum = upc::combine(sum, *r);
                any = true;
            }
            dated += g_store.play_times(key).size();
            last_7 += g_store.plays_since(key, now - 7 * 86400);
            last_30 += g_store.plays_since(key, now - 30 * 86400);
            last_365 += g_store.plays_since(key, now - 365 * 86400);
            skips += g_store.skip_count(key);
            last_skip = (std::max)(last_skip, g_store.last_skip(key));
            if (const auto t = g_store.get_lastfm_track(key)) {
                ++lastfm_tracks;
                lastfm_count += t->playcount;
                lastfm_loved = lastfm_loved || t->loved;
                if (!t->times.empty()) lastfm_dates.add(t->times.front(), t->times.back());
            }
            if (g_store.listens_total()) {
                const auto times = listens_of(rec);
                listens += times.size();
                if (!times.empty()) listen_dates.add(times.front(), times.back());
            }
        }
        const bool one = keys.size() == 1;
        auto date = [](int64_t ts) { return ts ? upc::format_date(ts) : std::string(); };
        auto section = [&](const char* name) {
            return [&out, name, order = 0.0](const char* label, const std::string& value) mutable {
                if (!value.empty()) out.set_property(name, order++, label, value.c_str());
            };
        };

        if (any && (!wanted || wanted->is_group_wanted(group))) {
            auto row = section(group);
            row(one ? "Play count" : "Play count (total)", std::to_string(sum.playcount));
            if (one && sum.rating) row("Rating", std::to_string(sum.rating));
            if (one && sum.loved) row("Loved", "Yes");
            row("First played", date(sum.first_played));
            row("Last played", date(sum.last_played));
            row("Added", date(sum.added));
            if (dated) { // counted from the dated play history, which starts with version 0.4.0
                row("Plays in the last 7 days", std::to_string(last_7));
                row("Plays in the last 30 days", std::to_string(last_30));
                row("Plays in the last 365 days", std::to_string(last_365));
            }
            if (skips) {
                row(one ? "Skips" : "Skips (total)", std::to_string(skips));
                row("Last skipped", date(last_skip));
            }
        }
        if (lastfm_tracks && (!wanted || wanted->is_group_wanted(group_lastfm))) {
            auto row = section(group_lastfm);
            row(one ? "Play count" : "Play count (total)", std::to_string(lastfm_count));
            if (one && lastfm_loved) row("Loved", "Yes");
            row("First played", date(lastfm_dates.first));
            row("Last played", date(lastfm_dates.last));
        }
        if (listens && (!wanted || wanted->is_group_wanted(group_listens))) {
            auto row = section(group_listens);
            row(one ? "Listens" : "Listens (total)", std::to_string(listens));
            row("First listened", date(listen_dates.first));
            row("Last listened", date(listen_dates.last));
        }
    }
};
FB2K_SERVICE_FACTORY(properties_section);

// ---- Import (Library > Ultimate Playcount) ----

// shortcut: runs on the main thread (a few seconds for a big library); move to threaded_process if it feels slow.
class import_plan {
public:
    // `combine`: how the plugin's record lands on ours (the existing record, or an empty one carrying the label).
    explicit import_plan(upc::record (*combine)(upc::record, const upc::record&)) : m_combine(combine) {}

    // Several files can land on one key: the one with the most plays is used, never a mix.
    // `times`: the plugin's dated local plays (Unix seconds) for the play history.
    // `lastfm`: the plugin's Last.fm numbers, stored only for tracks that have none yet.
    void add(const metadb_handle_ptr& track, const upc::record& imported, const std::vector<int64_t>& times = {},
             const std::optional<upc::lastfm_track>& lastfm = std::nullopt) {
        std::string key;
        if (is_ignored(track) || !key_of(track, key)) {
            ++m_unmatched;
            return;
        }
        ++m_matched;
        m_tracks.add_item(track);
        auto [it, fresh] = m_pending.try_emplace(key);
        auto& p = it->second;
        if (fresh) {
            p.base = g_store.get(key).value_or(upc::record{});
            set_label(p.base, track);
        } else if (imported.playcount + times.size() <= p.imported.playcount + p.times.size()) {
            return;
        }
        p.imported = imported;
        p.times = times;
        p.lastfm = lastfm;
    }

    // Shows what would change and `rules` (what the import does), asks, backs up, writes.
    void confirm_and_apply(const char* source, const char* found, const char* rules) {
        std::vector<std::pair<std::string, upc::record>> batch;
        std::vector<std::pair<std::string, int64_t>> new_plays;
        std::vector<std::pair<std::string, upc::lastfm_track>> lastfm;
        size_t fresh = 0, changed = 0, unchanged = 0;
        for (auto& [key, p] : m_pending) {
            const auto rec = m_combine(p.base, p.imported);
            const auto existing = g_store.get(key);
            const auto before = new_plays.size();
            if (!p.times.empty()) {
                // Only plays from before the history's first one: the plugin and Ultimate Playcount may both have
                // logged the same listen a few seconds apart, so later dates can't be told apart.
                const auto have = g_store.play_times(key);
                for (const auto at : p.times)
                    if (have.empty() || at < have.front()) new_plays.emplace_back(key, at);
            }
            const bool add_lastfm = p.lastfm && !g_store.get_lastfm_summary(key);
            if (add_lastfm) lastfm.emplace_back(key, *p.lastfm);
            if (existing && upc::same_stats(*existing, rec) && new_plays.size() == before && !add_lastfm) {
                ++unchanged;
                continue;
            }
            ++(existing ? changed : fresh);
            batch.emplace_back(key, rec);
        }
        const pfc::string8 title = PFC_string_formatter() << "Import from " << source;
        pfc::string_formatter msg;
        msg << found << "\n\nMatched to tracks: " << m_matched << " files\nNo tag info / not counted: " << m_unmatched
            << " files\n\nNew records: " << fresh << "\nUpdated records: " << changed
            << "\nAlready up to date: " << unchanged;
        if (!new_plays.empty()) msg << "\nPlays with a date to add to the history: " << new_plays.size();
        if (!lastfm.empty()) msg << "\nTracks getting Last.fm numbers: " << lastfm.size();
        if (batch.empty()) {
            popup_message::g_show(msg << "\n\nNothing to import.", title);
            return;
        }
        msg << "\n\n" << rules << " Importing again changes nothing.\n\nA backup of your Ultimate Playcount database "
               "is made first. Import now?";
        if (uMessageBox(core_api::get_main_window(), msg, title, MB_YESNO | MB_ICONQUESTION) != IDYES) return;

        pfc::string8 backup;
        try {
            backup = make_backup("import");
        } catch (const std::exception& e) {
            popup_message::g_complain("Ultimate Playcount: import failed, nothing was changed", e);
            return;
        }
        try {
            g_store.put_many(batch, {}, {}, new_plays);
            if (!lastfm.empty()) g_store.put_lastfm_tracks(lastfm);
        } catch (const std::exception& e) {
            popup_message::g_complain(PFC_string_formatter() << "Ultimate Playcount: import failed. The backup made "
                                                                "before it: " << backup, e);
            return;
        }
        metadb_io::get()->dispatch_refresh(m_tracks);
        popup_message::g_show(PFC_string_formatter() << "Imported " << batch.size() << " records.\n\nBackup: " << backup,
                              title);
    }

private:
    struct pending {
        upc::record base, imported;
        std::vector<int64_t> times;
        std::optional<upc::lastfm_track> lastfm;
    };
    upc::record (*m_combine)(upc::record, const upc::record&);
    std::map<std::string, pending> m_pending;
    metadb_handle_list m_tracks;
    size_t m_matched = 0, m_unmatched = 0;
};

// Rules shown before an import of another plugin's local numbers.
constexpr char replace_rules[] =
    "Each track's play count and first / last played dates are replaced with this plugin's: the last import wins, "
    "nothing is added up or compared. Its added date, rating and loved mark are taken when it has one (an empty one "
    "doesn't clear yours). The play history and the Last.fm numbers aren't touched.";

void import_playcount2003() {
    upc::pc2003_data data;
    try {
        data = upc::read_playcount2003(
            filesystem::g_get_native_path(core_api::pathInProfile("metadb.sqlite")).c_str());
    } catch (const std::exception& e) {
        popup_message::g_complain("Ultimate Playcount: could not read Playcount 2003 data", e);
        return;
    }
    import_plan plan(upc::replace_stats);
    auto api = metadb::get();
    for (const auto& row : data.rows) plan.add(api->handle_create(row.path.c_str(), row.subsong), row.rec);
    plan.confirm_and_apply("Playcount 2003",
                           PFC_string_formatter() << "Playcount 2003 has " << data.records << " records ("
                                                  << data.orphans << " no longer belong to any file and are skipped), "
                                                  << data.rows.size() << " files.",
                           replace_rules);
}

void import_foo_playcount() {
    if (cfg::legacy_names.get()) {
        popup_message::g_show("Turn off \"Also provide foo_playcount fields\" in Preferences > Tools > Ultimate "
                              "Playcount and restart first; otherwise the import would read our own values.",
                              "Import from foo_playcount");
        return;
    }
    // foo_playcount's storage isn't documented: read its fields while it is installed.
    const auto tracks = known_tracks();

    const auto script = compile("[%play_count%]|[%first_played%]|[%last_played%]|[%added%]|[%rating%]");
    import_plan plan(upc::replace_stats);
    size_t with_data = 0;
    pfc::string8 text;
    for (const auto& track : tracks) {
        track->format_title(nullptr, text, script, nullptr);
        pfc::list_t<pfc::string8> parts;
        pfc::splitStringByChar(parts, text, '|');
        if (parts.get_count() != 5) continue;
        upc::record r;
        r.playcount = static_cast<uint32_t>(strtoul(parts[0], nullptr, 10));
        r.first_played = upc::parse_local_datetime(parts[1].c_str());
        r.last_played = upc::parse_local_datetime(parts[2].c_str());
        r.added = upc::parse_local_datetime(parts[3].c_str());
        const auto rating = strtoul(parts[4], nullptr, 10);
        r.rating = rating <= 5 ? static_cast<uint32_t>(rating) : 0;
        if (!r.playcount && !r.first_played && !r.last_played && !r.added && !r.rating) continue;
        ++with_data;
        plan.add(track, r);
    }
    if (!with_data) {
        popup_message::g_show("No foo_playcount data found in the library or playlists. foo_playcount must be "
                              "installed and enabled for the import.",
                              "Import from foo_playcount");
        return;
    }
    plan.confirm_and_apply("foo_playcount", PFC_string_formatter() << "foo_playcount has data for " << with_data
                                                                   << " of " << tracks.get_count()
                                                                   << " library and playlist tracks.",
                        replace_rules);
}

void import_enhanced_stats() {
    if (cfg::enhanced_names.get()) {
        popup_message::g_show("Turn off \"Also provide Enhanced Playback Statistics fields\" in Preferences > Tools > "
                              "Ultimate Playcount and restart first; otherwise the import would read our own values.",
                              "Import from Enhanced Playback Statistics");
        return;
    }
    // Its storage isn't documented: read its fields while it is installed. Its Last.fm plays and its local ones stay
    // apart here too: the first go to the Last.fm numbers, the second to the play history. No count or date changes.
    const auto tracks = known_tracks();
    const auto local_script = compile("[%played_times_js%]"), lastfm_script = compile("[%lastfm_played_times_js%]");
    import_plan plan([](upc::record ours, const upc::record&) { return ours; });
    size_t with_data = 0, plays = 0, scrobbles = 0;
    pfc::string8 text;
    for (const auto& track : tracks) {
        track->format_title(nullptr, text, local_script, nullptr);
        const auto times = upc::parse_played_times_ms(text.c_str());
        track->format_title(nullptr, text, lastfm_script, nullptr);
        std::optional<upc::lastfm_track> lastfm;
        if (auto scrobbled = upc::parse_played_times_ms(text.c_str()); !scrobbled.empty()) {
            // The count is the scrobbles listed: its %lastfm_play_count% can be one higher (its "increment" option).
            lastfm.emplace();
            lastfm->playcount = static_cast<uint32_t>(scrobbled.size());
            lastfm->times = std::move(scrobbled); // fetched_at 0: "Update Last.fm numbers for all tracks" asks again
        }
        if (times.empty() && !lastfm) continue;
        ++with_data;
        plays += times.size();
        scrobbles += lastfm ? lastfm->times.size() : 0;
        plan.add(track, upc::record{}, times, lastfm);
    }
    if (!with_data) {
        popup_message::g_show("No play times found in the library or playlists. Enhanced Playback Statistics must be "
                              "installed and enabled for the import.",
                              "Import from Enhanced Playback Statistics");
        return;
    }
    plan.confirm_and_apply("Enhanced Playback Statistics",
                           PFC_string_formatter() << "Enhanced Playback Statistics has " << plays << " local plays and "
                                                  << scrobbles << " Last.fm scrobbles of " << with_data << " of "
                                                  << tracks.get_count() << " library and playlist tracks.",
                           "Its Last.fm scrobbles become the Last.fm numbers (upc_lastfm_ fields) of tracks that have "
                           "none yet; tracks already looked up on Last.fm keep theirs. Its local plays go to the play "
                           "history only, those from before the history's first play of the track. Play counts, "
                           "dates, ratings and loved marks are not changed, and nothing is mixed between local and "
                           "Last.fm.");
}

void import_lastfm_loved() {
    const char* title = "Import loved tracks from Last.fm";
    std::vector<upc::lastfm_row> loved;
    if (!download_lastfm_loved(loved)) return;
    if (loved.empty()) {
        popup_message::g_show("Last.fm has no loved tracks for this user.", title);
        return;
    }
    std::set<std::string> keys;
    for (const auto& r : loved) keys.insert(upc::match_key(r.artist, r.track));
    import_plan plan(upc::merge);
    size_t matched = 0;
    for (const auto& track : known_tracks()) {
        std::string artist, title;
        if (!names_of(track, artist, title) || !keys.count(upc::match_key(artist, title))) continue;
        ++matched;
        upc::record r;
        r.loved = true; // merge only ever adds the mark, it never removes one
        plan.add(track, r);
    }
    plan.confirm_and_apply("Last.fm loved tracks", PFC_string_formatter() << "Last.fm has " << loved.size()
                                                                          << " loved tracks; " << matched
                                                                          << " library and playlist tracks match them.",
                        "Only the loved mark is set; nothing else changes and no mark is removed.");
}

constexpr GUID guid_mainmenu_group = {0x6c99a9bb, 0xfc13, 0x4e10, {0xb0, 0x26, 0xc5, 0x6e, 0x32, 0x18, 0x75, 0x1b}};
mainmenu_group_popup_factory g_mainmenu_group(guid_mainmenu_group, mainmenu_groups::library,
                                              mainmenu_commands::sort_priority_dontcare, "Ultimate Playcount");

class mainmenu_import : public mainmenu_commands {
public:
    t_uint32 get_command_count() override { return 9; }
    GUID get_command(t_uint32 i) override {
        static constexpr GUID ids[] = {
            {0xc1d3152e, 0xbd2b, 0x4898, {0xa6, 0xd5, 0x46, 0xb2, 0xe6, 0x1a, 0x3e, 0xcc}},
            {0x8b1b2c09, 0x7e2a, 0x4099, {0xa4, 0x02, 0xc0, 0x6c, 0xb7, 0xc5, 0xad, 0x9b}},
            {0x2f6c4a18, 0x93d7, 0x4b5e, {0x8a, 0x61, 0xd0, 0x7e, 0x15, 0xb3, 0xc9, 0x44}},
            {0xa4e19c73, 0x5d02, 0x4f86, {0x9b, 0x3a, 0x71, 0xc8, 0x0e, 0xd5, 0x62, 0xf9}},
            {0x6b3d7f20, 0xe1c4, 0x4a95, {0x82, 0x0d, 0x4c, 0x9e, 0xb7, 0x13, 0xa6, 0x58}},
            {0xd2f08b35, 0x47a9, 0x4c1e, {0x9d, 0x66, 0x0b, 0xe3, 0x58, 0x21, 0xa7, 0x9c}},
            {0x5b8470dd, 0xcb58, 0x4323, {0xa8, 0x08, 0xaf, 0x24, 0x13, 0x5d, 0x42, 0x04}},
            {0x7e30c9a1, 0x4b85, 0x4d12, {0xb6, 0x4f, 0x28, 0xa0, 0x1d, 0xe7, 0x93, 0x5c}},
            {0x19fa5d62, 0xc047, 0x4e38, {0x85, 0xb1, 0x6a, 0xd4, 0x03, 0xf2, 0x8e, 0x77}},
        };
        return ids[i];
    }
    void get_name(t_uint32 i, pfc::string_base& out) override {
        out = i == 0   ? "Import from Playcount 2003..."
              : i == 1 ? "Import from foo_playcount..."
              : i == 2 ? "Import from Enhanced Playback Statistics..."
              : i == 3 ? "Update Last.fm numbers for all tracks..."
              : i == 4 ? "Stop Last.fm updates"
              : i == 5 ? "Import loved tracks from Last.fm..."
              : i == 7 ? "Sync ListenBrainz history..."
              : i == 8 ? "Download all ListenBrainz history..."
                       : "Clean database...";
    }
    bool get_description(t_uint32 i, pfc::string_base& out) override {
        out = i == 0   ? "Imports play counts, dates, ratings and loved marks stored by foo_playcount_2003."
              : i == 1 ? "Imports play counts, dates and ratings from foo_playcount (it must be installed)."
              : i == 2 ? "Imports every dated play from Enhanced Playback Statistics (it must be installed)."
              : i == 3 ? "Asks Last.fm about every track that has no recent Last.fm numbers, in the background."
              : i == 4 ? "Drops the tracks still waiting to be looked up on Last.fm."
              : i == 5 ? "Marks the tracks you loved on Last.fm as loved here. Nothing is unmarked."
              : i == 7 ? "Adds the listens ListenBrainz has recorded since the last sync."
              : i == 8 ? "Reads your whole ListenBrainz history again; listens already here are skipped."
                       : "Lists records whose track is gone or whose tags changed, to merge or remove them.";
        return true;
    }
    GUID get_parent() override { return guid_mainmenu_group; }
    void execute(t_uint32 i, service_ptr_t<service_base>) override {
        if (!g_store.is_open()) {
            popup_message::g_show("The Ultimate Playcount database is not available.", "Ultimate Playcount");
            return;
        }
        if (i == 0) import_playcount2003();
        else if (i == 1) import_foo_playcount();
        else if (i == 2) import_enhanced_stats();
        else if (i == 3) lastfm_update_all();
        else if (i == 4) lastfm_stop_updates();
        else if (i == 5) import_lastfm_loved();
        else if (i == 7 || i == 8) run_listenbrainz_sync(i == 8);
        else run_cleanup();
    }
};
FB2K_SERVICE_FACTORY(mainmenu_import);

} // namespace
