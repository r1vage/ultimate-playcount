#include "stdafx.h"

#include <algorithm>
#include <memory>
#include <tuple>

#include "config.h"
#include "core.h"
#include "listenbrainz_core.h"

namespace cfg = upc_cfg;

namespace upc_fb {

namespace {

constexpr int page_size = 1000; // the most ListenBrainz returns per request

struct fetched {
    std::vector<upc::lastfm_row> rows;
    std::string error;
};

// One page. Tries again a few times when ListenBrainz is busy or the connection drops.
upc::listens_page fetch_page(const std::string& user, int64_t max_ts, int64_t min_ts, abort_callback& abort) {
    upc::listens_page result;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt) abort.sleep(5.0 * attempt);
        bool retry = false;
        try {
            result = upc::parse_listens(http_get(upc::listens_url(user, page_size, max_ts, min_ts), abort));
            retry = !result.error.empty() && upc::is_temporary_listenbrainz_error(result.error_code);
        } catch (const exception_aborted&) {
            throw;
        } catch (const std::exception& e) {
            result = {};
            result.error = std::string("could not reach ListenBrainz: ") + e.what();
            retry = true;
        }
        if (!retry) break;
    }
    return result;
}

} // namespace

void run_listenbrainz_sync(bool everything) {
    const char* title = everything ? "Download all ListenBrainz history" : "Sync ListenBrainz history";
    if (!g_store.is_open()) {
        popup_message::g_show("The Ultimate Playcount database is not available.", title);
        return;
    }
    const std::string user = cfg::listenbrainz_user.get().c_str();
    if (user.empty()) {
        popup_message::g_show("Enter your ListenBrainz user name first: Preferences > Tools > Ultimate Playcount > "
                              "ListenBrainz.",
                              title);
        return;
    }
    // Overlapping the last stored second is harmless: a listen already stored is skipped.
    const int64_t since = everything ? 0 : g_store.listens_latest();

    auto result = std::make_shared<fetched>();
    auto task = threaded_process_callback_lambda::create([=](threaded_process_status& status, abort_callback& abort) {
        try {
            int64_t max_ts = 0, min_ts = since ? since - 1 : 0;
            for (;;) {
                abort.check();
                status.set_item(PFC_string_formatter() << result->rows.size() << " listens so far");
                const auto page = fetch_page(user, max_ts, min_ts, abort);
                if (!page.error.empty()) {
                    result->error = page.error;
                    return;
                }
                if (page.items.empty()) return;
                result->rows.insert(result->rows.end(), page.items.begin(), page.items.end());
                if (page.items.size() < page_size) return; // the last page
                int64_t oldest = page.items.front().at, newest = oldest;
                for (const auto& r : page.items) {
                    oldest = (std::min)(oldest, r.at);
                    newest = (std::max)(newest, r.at);
                }
                if (!upc::next_page(since != 0, oldest, newest, max_ts, min_ts)) return;
                abort.sleep(0.5);
            }
        } catch (const exception_aborted&) {
            throw;
        } catch (const std::exception& e) {
            result->error = e.what();
        }
    });
    const unsigned flags = threaded_process::flag_show_abort | threaded_process::flag_show_item;
    if (!threaded_process::g_run_modal(task, flags, core_api::get_main_window(), title)) return; // aborted
    if (!result->error.empty()) {
        popup_message::g_show(PFC_string_formatter() << "ListenBrainz: " << result->error.c_str()
                                                     << "\n\nNothing was changed.",
                              title);
        return;
    }

    // Pages overlap by a second: drop the listens that came twice, so the counts below are right.
    auto& rows = result->rows;
    const auto order = [](const upc::lastfm_row& a, const upc::lastfm_row& b) {
        return std::tie(a.at, a.artist, a.track) < std::tie(b.at, b.artist, b.track);
    };
    std::sort(rows.begin(), rows.end(), order);
    rows.erase(std::unique(rows.begin(), rows.end(),
                           [&](const auto& a, const auto& b) { return !order(a, b) && !order(b, a); }),
               rows.end());
    const size_t fresh = g_store.count_new_listens(result->rows);
    pfc::string_formatter msg;
    msg << "ListenBrainz sent " << result->rows.size() << " listens" << (since ? " since your last sync" : "")
        << ".\n\nNew: " << fresh << "\nAlready here: " << result->rows.size() - fresh;
    if (!fresh) {
        popup_message::g_show(msg << "\n\nNothing to add.", title);
        return;
    }
    msg << "\n\nListenBrainz has no per-track number to ask for, so the listens are kept as they come and counted per "
           "track here: a listen belongs to a track when its MusicBrainz recording id (MUSICBRAINZ_TRACKID) is the "
           "same or its artist and title are exactly the same (case and extra spaces ignored). They are kept apart "
           "from the plays counted here; the listenbrainz_ fields show them. A backup of your Ultimate Playcount "
           "database is made first. Add them now?";
    if (uMessageBox(core_api::get_main_window(), msg, title, MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    pfc::string8 backup;
    size_t added = 0;
    try {
        backup = make_backup("listenbrainz");
        added = g_store.add_listens(result->rows);
    } catch (const std::exception& e) {
        popup_message::g_complain("Ultimate Playcount: could not save the ListenBrainz listens, nothing was changed", e);
        return;
    }
    metadb_io::get()->dispatch_refresh(known_tracks());
    popup_message::g_show(PFC_string_formatter() << "Added " << added << " listens.\n\nBackup: " << backup, title);
}

} // namespace upc_fb
