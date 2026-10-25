#pragma once
// ListenBrainz: building requests and reading replies. No foobar2000 dependency.
#include <cstdint>
#include <string>
#include <vector>

#include "store.h"

namespace upc {

struct listens_page {
    std::vector<lastfm_row> items; // `album` holds the release name; `mbid` the recording id ListenBrainz mapped
    int error_code = 0;            // HTTP-style code from the reply, 0 = none
    std::string error;             // empty = fine
};

// Reads a GET /1/user/<name>/listens reply.
listens_page parse_listens(const std::string& json);

// `max_ts`: only listens before it (0 = newest first from the top). `min_ts`: only listens after it. Not both.
std::string listens_url(const std::string& user, int count, int64_t max_ts, int64_t min_ts);

// After a full page whose listens run from `oldest` to `newest`: moves the cursor for the next page, walking forward
// from `min_ts` (sync since the last time) or back from `max_ts` (everything). Both limits leave their own second
// out, and a second can hold several listens, so pages overlap by one second (repeats are dropped when saving).
// False when the cursor can't move: no further page.
bool next_page(bool forward, int64_t oldest, int64_t newest, int64_t& max_ts, int64_t& min_ts);

// Busy or temporary: rate limit and server errors.
bool is_temporary_listenbrainz_error(int error_code);

} // namespace upc
