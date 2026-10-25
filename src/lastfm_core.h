#pragma once
// Last.fm: building requests and reading replies. No foobar2000 dependency.
#include <cstdint>
#include <string>
#include <vector>

#include "store.h"

namespace upc {

// One page of a track list: the user's loved tracks, or the scrobbles of one track.
struct track_page {
    std::vector<lastfm_row> items; // scrobbles: only those with a date (the one playing right now is left out)
    int pages = 1;                 // total pages for this request
    int total = 0;                 // total items Last.fm reports, 0 if not given
    int error_code = 0;            // Last.fm's own error number, 0 = none
    std::string error;             // readable reason; empty = fine
};

// track.getInfo for one user: what Last.fm says about the track and this user's plays of it.
struct track_info {
    bool found = true;       // false: Last.fm doesn't know the track (error 6), counts are 0
    uint32_t playcount = 0;  // userplaycount
    bool loved = false;      // userloved
    int error_code = 0;
    std::string error;       // empty = fine
};

track_info parse_track_info(const std::string& json);
// user.getTrackScrobbles, the user's own scrobbles of one track.
track_page parse_track_scrobbles(const std::string& json);
// user.getLovedTracks.
track_page parse_loved_tracks(const std::string& json);

std::string track_info_url(const std::string& user, const std::string& api_key, const std::string& artist,
                           const std::string& track, bool autocorrect);
std::string track_scrobbles_url(const std::string& user, const std::string& api_key, const std::string& artist,
                                const std::string& track, int page, bool autocorrect);
std::string loved_tracks_url(const std::string& user, const std::string& api_key, int page);

// Last.fm error numbers worth trying again after a pause: busy, rate limit, temporary failure.
bool is_temporary_error(int error_code);
// Numbers after which every further request will fail too: bad or suspended API key, no permission.
bool is_fatal_error(int error_code);

} // namespace upc
