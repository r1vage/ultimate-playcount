#pragma once
// Settings, edited on Preferences > Tools > Ultimate Playcount.

namespace upc_cfg {

constexpr char default_key[] =
    "%album artist%|%album%|$year(%date%)|$max($num(%discnumber%,1),1)|%artist%|%tracknumber%|%title%";
constexpr char default_prefix[] = "upc_";
constexpr int default_count_mode = 0; // upc::count_mode::percent
constexpr int default_count_percent = 50;
constexpr int default_count_seconds = 60;

extern cfg_int count_mode;        // upc::count_mode
extern cfg_int count_percent;
extern cfg_int count_seconds;
extern cfg_string key;            // restart
extern cfg_bool ignore_case;      // restart: keys compared without upper/lower case
extern cfg_bool follow_edits;     // tag edits move the record to the new key (off: the edited track starts over)
extern cfg_string prefix;         // restart
extern cfg_bool legacy_names;     // restart
extern cfg_bool lfm_names;        // restart: also answer to Last.fm Playcount Sync field names (lfm_...)
extern cfg_bool enhanced_names;   // restart: also answer to Enhanced Playback Statistics field names
extern cfg_string listenbrainz_user;
extern cfg_string lastfm_user;
extern cfg_string lastfm_key;
extern cfg_int lastfm_summary_every;  // tracks looked up between two summary lines in the console
extern cfg_bool lastfm_update;    // ask Last.fm about a track when it starts playing
extern cfg_bool lastfm_on_end;    // ... and again shortly after it ends (the scrobble)
extern cfg_bool lastfm_background; // ... and slowly about all tracks in the background
extern cfg_bool lastfm_feeds;     // the standard playcount / first / last played fields show Last.fm's when higher (display only)
extern cfg_bool count_skips;      // log a skip when a track is left before it counted as played
extern cfg_bool ignore_outside;
extern cfg_bool debug;
extern cfg_bool auto_clean;       // remove records of deleted files automatically
extern cfg_int auto_clean_days;
extern cfg_bool auto_backup;      // at startup, when the last one is older than auto_backup_days
extern cfg_int auto_backup_days;
extern cfg_int backup_keep;       // per kind of backup
extern cfg_bool log_events;       // diagnostics: every playback event foobar2000 sends

} // namespace upc_cfg
