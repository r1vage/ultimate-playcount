#pragma once
// Shared foobar2000-side helpers (defined in main.cpp).
#include <string>

#include "store.h"

namespace upc_fb {

inline constexpr char log_prefix[] = "[Ultimate Playcount] ";

extern upc::store g_store;

titleformat_object::ptr compile(const char* pattern);
int64_t unix_now();
// Record key of a track from its own tags; false if foobar2000 has no info for it.
bool key_of(const metadb_handle_ptr& track, std::string& out);
std::string native_path(const metadb_handle_ptr& track);
// Fills artist, title, album, path and subsong from the track.
void set_label(upc::record& r, const metadb_handle_ptr& track);
// Library plus all playlists.
metadb_handle_list known_tracks();
// Copies the database before a bulk change; returns the copy's path. Throws on failure.
pfc::string8 make_backup(const char* reason);

// True when the setting "ignore tracks outside of library" is on and the track is not in the library.
bool is_ignored(const metadb_handle_ptr& track);
// The artist and title as Last.fm is asked for them (%artist%, %title%); false if either is empty.
bool names_of(const metadb_handle_ptr& track, std::string& artist, std::string& title);

// lastfm.cpp: Last.fm's own numbers, asked for track by track in a background thread.
void lastfm_update(metadb_handle_list_cref tracks);  // look these tracks up now
void lastfm_forget(metadb_handle_list_cref tracks);  // drop what was stored for them
void lastfm_track_started(const metadb_handle_ptr& track);  // playback: look it up now
void lastfm_track_ended(const metadb_handle_ptr& track);    // playback: look it up again shortly (the scrobble)
void lastfm_update_all();    // every track without recent numbers, after asking
void lastfm_stop_updates();  // drop the tracks still waiting
// The body of a GET request (also for 4xx replies, which carry the service's explanation). Throws on failure.
std::string http_get(const std::string& url, abort_callback& abort);
// listenbrainz.cpp: Library > Ultimate Playcount > Sync ListenBrainz history.
void run_listenbrainz_sync(bool everything);
// The user's loved tracks, with a progress dialog. False if aborted or failed (the user was told).
bool download_lastfm_loved(std::vector<upc::lastfm_row>& out);

// cleanup.cpp: the Clean database dialog, and the optional automatic removal run after the library loads.
void run_cleanup();
void run_auto_clean();

} // namespace upc_fb
