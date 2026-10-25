#include "stdafx.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>

#include "config.h"
#include "core.h"
#include "lastfm_core.h"
#include "version.h"

namespace cfg = upc_cfg;

namespace upc_fb {

namespace {

using clock_type = std::chrono::steady_clock;

std::string download(const std::string& url, abort_callback& abort) {
    auto request = http_client::get()->create_request("GET");
    request->add_header("User-Agent", "foo_ultimate_playcount/" UPC_VERSION_STRING);
    // run_ex: Last.fm explains its errors in the body of a 4xx reply.
    file::ptr reply = request->run_ex(url.c_str(), abort);
    std::string body;
    char buf[16384];
    for (;;) {
        const size_t n = reply->read(buf, sizeof buf, abort);
        if (!n) break;
        body.append(buf, n);
    }
    return body;
}

// Runs `fetch` (one request, returns Last.fm's error number, 0 = fine) again a few times when Last.fm is busy
// or the connection drops. Anything else it gives up on. Returns the last error text, empty = fine.
template <typename F> std::string with_retries(abort_callback& abort, int& error_code, F fetch) {
    std::string error;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt) abort.sleep(2.0 * attempt);
        error_code = 0;
        try {
            error = fetch(error_code);
            if (error.empty() || !upc::is_temporary_error(error_code)) return error;
        } catch (const exception_aborted&) {
            throw;
        } catch (const std::exception& e) {
            error = std::string("could not reach Last.fm: ") + e.what();
        }
    }
    return error;
}

// A track Last.fm is asked about, in the background, one request at a time.
struct job {
    std::string key, artist, title;
    metadb_handle_ptr track;
    clock_type::time_point not_before;
    const char* why = ""; // for the log: what asked for it
};

class updater {
public:
    ~updater() { stop(); }

    // Main thread. `front`: before the tracks already waiting.
    void add(job j, bool front, const std::string& user, const std::string& api_key) {
        std::lock_guard lock(m_mutex);
        if (m_stop) return;
        m_user = user;
        m_api_key = api_key;
        m_summary_every = static_cast<size_t>((std::max)(1, static_cast<int>(cfg::lastfm_summary_every.get())));
        if (!m_queued.insert(j.key).second) { // already waiting
            if (!front) return;
            // e.g. far back in a library-wide run while the track plays now: it moves up, with the new timing
            m_queue.erase(std::find_if(m_queue.begin(), m_queue.end(), [&](const job& q) { return q.key == j.key; }));
        }
        if (front) m_queue.push_front(std::move(j));
        else m_queue.push_back(std::move(j));
        if (!m_thread.joinable()) m_thread = std::thread([this] { run(); });
        m_cv.notify_all();
    }

    size_t clear() {
        std::lock_guard lock(m_mutex);
        const size_t n = m_queue.size();
        m_queue.clear();
        m_queued.clear();
        return n;
    }

    size_t pending() {
        std::lock_guard lock(m_mutex);
        return m_queue.size();
    }

    void stop() {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
            m_abort.set();
            m_cv.notify_all();
        }
        if (m_thread.joinable()) m_thread.join();
    }

private:
    void run() {
        try {
            for (;;) {
                job j;
                std::string user, key;
                {
                    std::unique_lock lock(m_mutex);
                    m_cv.wait(lock, [&] { return m_stop || !m_queue.empty(); });
                    if (m_stop) return;
                    if (m_queue.front().not_before > clock_type::now()) {
                        // a track just ended: give the scrobbler a moment to reach Last.fm. A track that starts
                        // meanwhile goes in front of it and is looked up right away.
                        const auto until = m_queue.front().not_before;
                        m_cv.wait_until(lock, until, [&] {
                            return m_stop || m_queue.empty() || m_queue.front().not_before <= clock_type::now();
                        });
                        if (m_stop) return;
                        continue;
                    }
                    j = std::move(m_queue.front());
                    m_queue.pop_front();
                    m_queued.erase(j.key);
                    user = m_user;
                    key = m_api_key;
                }
                process(j, user, key);
                if (m_checked + m_failed >= m_summary_every) report(false);
                else if (m_reported_any && !pending()) report(true); // the end of a long run
                note_done(j.track);
                m_abort.sleep(0.25); // Last.fm asks for no more than a few requests a second
            }
        } catch (const exception_aborted&) {
        }
    }

    void process(const job& j, const std::string& user, const std::string& api_key) {
        try {
            upc::track_info info;
            int code = 0;
            // The exact title first: autocorrect can map it onto another track (say "You Should've" onto
            // "You Should Have"), whose count and loved mark would then be stored for this one.
            bool autocorrect = false;
            std::string error;
            for (;; autocorrect = true) {
                error = with_retries(m_abort, code, [&](int& error_code) {
                    info = upc::parse_track_info(
                        download(upc::track_info_url(user, api_key, j.artist, j.title, autocorrect), m_abort));
                    error_code = info.error_code;
                    return info.error;
                });
                if (!error.empty() || info.found || autocorrect) break;
                m_abort.sleep(0.25);
            }
            if (!error.empty()) {
                if (upc::is_fatal_error(code)) {
                    clear();
                    FB2K_console_formatter() << log_prefix << "Last.fm refused the request (" << error.c_str()
                                             << "). Check the user name and API key; the waiting tracks were dropped.";
                } else {
                    ++m_failed;
                    m_last_error = error;
                }
                return;
            }
            upc::lastfm_track t;
            t.playcount = info.playcount;
            t.loved = info.loved;
            t.fetched_at = unix_now();
            // The dates are a bonus: if this reply isn't what we expect, the count and loved mark still stand.
            for (int page = 1; info.playcount > 0 && page <= 25; ++page) {
                m_abort.sleep(0.25);
                upc::track_page p;
                const std::string e = with_retries(m_abort, code, [&](int& error_code) {
                    p = upc::parse_track_scrobbles(
                        download(upc::track_scrobbles_url(user, api_key, j.artist, j.title, page, autocorrect), m_abort));
                    error_code = p.error_code;
                    return p.error;
                });
                if (!e.empty()) {
                    if (!m_told_dates) {
                        m_told_dates = true;
                        FB2K_console_formatter() << log_prefix << "Last.fm: could not read the dates of the plays of "
                                                 << j.artist.c_str() << " - " << j.title.c_str() << ": " << e.c_str();
                    }
                    break;
                }
                for (const auto& r : p.items) t.times.push_back(r.at);
                if (page >= p.pages) break;
            }
            // The key from now, not from when the track was queued: its tags may have been edited since.
            std::string key;
            if (!key_of(j.track, key)) return;
            upc::record r; // for a track without a record yet (playlist only)
            r.added = unix_now();
            set_label(r, j.track);
            g_store.add_if_missing(key, r);
            const auto before = g_store.get_lastfm_summary(key);
            const uint32_t was = before ? before->playcount : 0;
            const bool was_loved = before && before->loved;
            g_store.put_lastfm_track(key, t);
            ++m_checked;
            if (t.playcount != was || t.loved != was_loved) {
                ++m_updated;
                if (t.playcount > was) m_new_plays += t.playcount - was;
                if (t.loved && !was_loved) ++m_new_loved;
                if (!t.loved && was_loved) ++m_unloved;
            }
            // the slow background refresh only shows in the summary lines, not one line per track
            if (cfg::debug.get() && strcmp(j.why, "background") != 0)
                FB2K_console_formatter() << log_prefix << "Last.fm (" << j.why << "): " << j.artist.c_str() << " - "
                                         << j.title.c_str() << ": " << t.playcount << " plays"
                                         << (t.loved ? ", loved" : "") << ", " << t.times.size() << " dates"
                                         << (info.found ? "" : " (Last.fm doesn't know this track)");
        } catch (const exception_aborted&) {
            throw;
        } catch (const std::exception& e) {
            ++m_failed;
            m_last_error = e.what();
        }
    }

    // One line for a batch of lookups: "checked 50 tracks, updated 7 tracks with 23 new plays, ...".
    void report(bool final) {
        if (m_checked || m_failed) {
            const size_t waiting = pending();
            console::formatter line; // prints when it goes out of scope
            line << log_prefix << "Last.fm: checked " << m_checked << (m_checked == 1 ? " track" : " tracks")
                 << ", updated " << m_updated << (m_updated == 1 ? " track" : " tracks");
            if (m_updated) line << " with " << m_new_plays << " new plays";
            if (m_new_loved) line << ", " << m_new_loved << " newly loved";
            if (m_unloved) line << ", " << m_unloved << " no longer loved";
            if (m_failed) line << ", " << m_failed << " failed (" << m_last_error.c_str() << ")";
            if (waiting) line << "; " << waiting << " waiting";
        }
        m_checked = m_updated = m_failed = m_new_loved = m_unloved = 0;
        m_new_plays = 0;
        m_last_error.clear();
        m_reported_any = !final;
    }

    // Shows new numbers: refreshes in batches, so a long run doesn't redraw for every track.
    void note_done(const metadb_handle_ptr& track) {
        m_dirty.push_back(track);
        const auto now = clock_type::now();
        if (now - m_last_flush < std::chrono::seconds(2) && pending()) return;
        m_last_flush = now;
        auto list = std::move(m_dirty);
        m_dirty.clear();
        fb2k::inMainThread([list] {
            metadb_handle_list items;
            for (const auto& t : list) items.add_item(t);
            metadb_io::get()->dispatch_refresh(items);
        });
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<job> m_queue;
    std::set<std::string> m_queued;
    std::string m_user, m_api_key;
    bool m_stop = false;
    abort_callback_impl m_abort;
    std::thread m_thread;
    // worker thread only
    size_t m_checked = 0, m_updated = 0, m_failed = 0, m_new_loved = 0, m_unloved = 0; // since the last summary line
    uint64_t m_new_plays = 0;
    std::string m_last_error;
    bool m_reported_any = false; // a summary line was printed: the end of the run gets one too
    std::atomic<size_t> m_summary_every{50};
    bool m_told_dates = false;
    std::vector<metadb_handle_ptr> m_dirty;
    clock_type::time_point m_last_flush{};
};

// Built on first use, not at DLL load: abort_callback_impl asks foobar2000 for a service in its constructor, which
// fails while the DLL is still loading ("DLL initialization routine failed").
updater& the_updater() {
    static updater u;
    return u;
}

class updater_quit : public initquit {
public:
    void on_quit() override { the_updater().stop(); }
};
FB2K_SERVICE_FACTORY(updater_quit);

bool credentials(std::string& user, std::string& key) {
    user = cfg::lastfm_user.get().c_str();
    key = cfg::lastfm_key.get().c_str();
    return !user.empty() && !key.empty();
}

constexpr int refresh_days = 30;

// Adds the track unless it is outside the library and ignored, or has no usable names. Main thread.
bool queue_track(const metadb_handle_ptr& track, bool front, std::chrono::seconds delay, const std::string& user,
                 const std::string& api_key, const char* why) {
    job j;
    j.why = why;
    if (is_ignored(track) || !key_of(track, j.key) || !names_of(track, j.artist, j.title)) return false;
    j.track = track;
    j.not_before = clock_type::now() + delay;
    the_updater().add(std::move(j), front, user, api_key);
    return true;
}

} // namespace

std::string http_get(const std::string& url, abort_callback& abort) { return download(url, abort); }

void lastfm_update(metadb_handle_list_cref tracks) {
    std::string user, key;
    if (!credentials(user, key)) {
        popup_message::g_show("Enter your Last.fm user name and API key first: Preferences > Tools > Ultimate "
                              "Playcount > Last.fm.",
                              "Update from Last.fm");
        return;
    }
    // Reverse, so the first selected track is looked up first.
    for (size_t i = tracks.get_count(); i-- > 0;) queue_track(tracks[i], true, std::chrono::seconds(0), user, key, "on request");
}

void lastfm_forget(metadb_handle_list_cref tracks) {
    if (!g_store.is_open()) return;
    std::set<std::string> keys;
    for (const auto& track : tracks) {
        std::string key;
        if (key_of(track, key)) keys.insert(key);
    }
    try {
        g_store.forget_lastfm({keys.begin(), keys.end()});
    } catch (const std::exception& e) {
        popup_message::g_complain("Ultimate Playcount: could not forget the Last.fm numbers", e);
        return;
    }
    metadb_io::get()->dispatch_refresh(tracks);
}

void lastfm_track_started(const metadb_handle_ptr& track) {
    std::string user, key;
    if (!cfg::lastfm_update.get() || !g_store.is_open() || !credentials(user, key)) return;
    queue_track(track, true, std::chrono::seconds(0), user, key, "track started");
}

void lastfm_track_ended(const metadb_handle_ptr& track) {
    std::string user, key;
    if (!cfg::lastfm_on_end.get() || !g_store.is_open() || !credentials(user, key)) return;
    queue_track(track, true, std::chrono::seconds(30), user, key, "track ended"); // the scrobble needs a moment to arrive
}

void lastfm_update_all() {
    const char* title = "Update Last.fm numbers";
    std::string user, key;
    if (!g_store.is_open() || !credentials(user, key)) {
        popup_message::g_show("Enter your Last.fm user name and API key first: Preferences > Tools > Ultimate "
                              "Playcount > Last.fm.",
                              title);
        return;
    }
    const int64_t stale = unix_now() - int64_t{refresh_days} * 86400;
    metadb_handle_list todo;
    std::set<std::string> seen;
    for (const auto& track : known_tracks()) {
        std::string track_key;
        if (is_ignored(track) || !key_of(track, track_key) || !seen.insert(track_key).second) continue;
        const auto have = g_store.get_lastfm_summary(track_key);
        if (!have || have->fetched_at < stale) todo.add_item(track);
    }
    if (!todo.get_count()) {
        popup_message::g_show(PFC_string_formatter() << "Every track has Last.fm numbers from the last "
                                                     << refresh_days << " days.",
                              title);
        return;
    }
    // A request every quarter second; a track with plays needs a second one for the dates.
    const size_t minutes = todo.get_count() * 3 / 4 / 60 + 1;
    if (uMessageBox(core_api::get_main_window(),
                    PFC_string_formatter() << todo.get_count() << " tracks have no Last.fm numbers yet (or only old "
                                           << "ones). Last.fm will be asked about each of them in the background, "
                                           << "which takes up to about " << minutes << " minutes. You can keep "
                                           << "using foobar2000; a summary is printed to the console after every "
                                           << cfg::lastfm_summary_every.get() << " tracks. Stop any time with Library > Ultimate Playcount > Stop "
                                           << "Last.fm updates.\n\nStart?",
                    title, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    for (const auto& track : todo) queue_track(track, false, std::chrono::seconds(0), user, key, "update all");
}

namespace {

// Slow background refresh, for scrobbles that reach Last.fm after the track's lookups and for tracks not played
// lately: whenever nothing else waits, one track every few seconds, the longest-unasked first. Main thread only.
constexpr double background_every = 10;          // seconds per track: a 25,000-track library takes about 3 days
constexpr int64_t background_min_age = 86400;    // tracks asked within the last day are left for the next round
metadb_handle_list g_round;                      // this round's tracks, oldest numbers first
size_t g_round_next = 0;
int64_t g_next_round_at = 0;                     // unix time; a round with nothing to do waits a while
fb2k::objRef g_background_timer;

bool is_stale(const metadb_handle_ptr& track, int64_t now) {
    std::string key;
    if (is_ignored(track) || !key_of(track, key)) return false;
    const auto have = g_store.get_lastfm_summary(key);
    return !have || have->fetched_at < now - background_min_age;
}

// The tracks to go through, the ones without numbers first, then the oldest numbers.
metadb_handle_list make_round() {
    const auto now = unix_now();
    std::vector<std::pair<int64_t, metadb_handle_ptr>> stale;
    std::set<std::string> seen;
    for (const auto& track : known_tracks()) {
        std::string key;
        if (is_ignored(track) || !key_of(track, key) || !seen.insert(key).second) continue;
        const auto have = g_store.get_lastfm_summary(key);
        const int64_t fetched = have ? have->fetched_at : -1;
        if (fetched < now - background_min_age) stale.emplace_back(fetched, track);
    }
    std::stable_sort(stale.begin(), stale.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    metadb_handle_list out;
    for (const auto& [fetched, track] : stale) out.add_item(track);
    return out;
}

void background_tick() {
    std::string user, key;
    if (!cfg::lastfm_background.get() || !g_store.is_open() || !credentials(user, key) || the_updater().pending())
        return;
    if (!library_manager_v4::get()->is_initialized()) return;
    const auto now = unix_now();
    while (g_round_next < g_round.get_count()) {
        const auto& track = g_round[g_round_next++];
        if (is_stale(track, now)) { // played since the round began: already fresh
            queue_track(track, false, std::chrono::seconds(0), user, key, "background");
            return;
        }
    }
    if (now < g_next_round_at) return;
    g_round = make_round();
    g_round_next = 0;
    if (!g_round.get_count()) g_next_round_at = now + 3600;
}

class background_init : public initquit {
public:
    void on_init() override { g_background_timer = fb2k::registerTimer(background_every, background_tick); }
    void on_quit() override {
        g_background_timer.release();
        g_round.remove_all();
    }
};
FB2K_SERVICE_FACTORY(background_init);

} // namespace

void lastfm_stop_updates() {
    const size_t n = the_updater().clear();
    popup_message::g_show(PFC_string_formatter() << "Dropped " << n << " waiting tracks.", "Update Last.fm numbers");
}

bool download_lastfm_loved(std::vector<upc::lastfm_row>& out) {
    const char* title = "Import loved tracks from Last.fm";
    std::string user, key;
    if (!credentials(user, key)) {
        popup_message::g_show("Enter your Last.fm user name and API key first: Preferences > Tools > Ultimate "
                              "Playcount > Last.fm.",
                              title);
        return false;
    }
    struct fetched {
        std::vector<upc::lastfm_row> rows;
        std::string error;
    };
    auto result = std::make_shared<fetched>();
    auto task = threaded_process_callback_lambda::create([=](threaded_process_status& status, abort_callback& abort) {
        try {
            int pages = 0;
            for (int page = 1;; ++page) {
                abort.check();
                status.set_item(PFC_string_formatter() << "Page " << page << (pages ? " of " : "") << (pages ? pages : 0)
                                                       << ", " << result->rows.size() << " loved tracks so far");
                upc::track_page p;
                int code = 0;
                const std::string error = with_retries(abort, code, [&](int& error_code) {
                    p = upc::parse_loved_tracks(download(upc::loved_tracks_url(user, key, page), abort));
                    error_code = p.error_code;
                    return p.error;
                });
                if (!error.empty()) {
                    result->error = error;
                    return;
                }
                pages = p.pages;
                result->rows.insert(result->rows.end(), p.items.begin(), p.items.end());
                status.set_progress(static_cast<t_size>(page), static_cast<t_size>(pages));
                if (page >= pages) return;
                abort.sleep(0.25);
            }
        } catch (const exception_aborted&) {
            throw;
        } catch (const std::exception& e) {
            result->error = e.what();
        }
    });
    const unsigned flags = threaded_process::flag_show_abort | threaded_process::flag_show_progress |
                           threaded_process::flag_show_item;
    if (!threaded_process::g_run_modal(task, flags, core_api::get_main_window(), title)) return false; // aborted
    if (!result->error.empty()) {
        popup_message::g_show(PFC_string_formatter() << "Last.fm: " << result->error.c_str() << "\n\nNothing was changed.",
                              title);
        return false;
    }
    out = std::move(result->rows);
    return true;
}

} // namespace upc_fb
