# Ultimate Playcount (foo_ultimate_playcount)

Play counts, play dates, ratings and loved marks for foobar2000 x64 (2.x), in one component. It can take the place
of Playcount 2003, foo_playcount, Last.fm Playcount Sync and Enhanced Playback Statistics, and it brings in your
numbers from Last.fm and ListenBrainz too.

## Why

- **Plays are not lost.** foobar2000 sends a "time" update about once a second, but when it is busy (writing
  tags, looking things up, drawing heavy panels) the updates come late and in bunches. Ultimate Playcount counts
  the seconds you really heard, so a late bunch of updates still counts, and a play is logged once you have
  listened long enough. By default that is half of the track. You can use a number of seconds instead, or the end of
  the track. Seeking back adds nothing, skipping ahead does not count the part you skipped, and editing tags or
  refreshing info in the middle of a song changes nothing.
- **One component instead of several.** Play counts, dates, ratings, loved marks, a dated play history, skips,
  Last.fm and ListenBrainz numbers, and the field names of the components it replaces, all in one place.
- **Its own file.** Everything is stored in `ultimate_playcount.db` in your profile folder, not inside
  foobar2000's own database. Backing it up is copying one file, and foobar2000's database is never written to.

## Features

- **Counting you can trust:** counts the seconds you really heard, and survives late updates, seeking and tag edits.
- **Play count, first and last played, added date, rating and loved mark** for every track, with date, Unix time,
  "time ago" and "days ago" forms.
- **A dated history of every play:** plays in the last 7, 30 and 365 days, and the full list of play times.
- **Skip count** (optional): tracks you stop or change before they count as played.
- **Last.fm numbers** for every track: play count, loved mark and play dates, straight from Last.fm. They are asked
  for when a track starts and ends and slowly in the background, with one short summary line in the console.
  You can import your loved tracks from Last.fm.
- **ListenBrainz** listens, downloaded and counted per track.
- **Show the higher count** (optional): the play count field can show the Last.fm or ListenBrainz count when it is
  higher than the local one, without changing your records.
- **Imports** from Playcount 2003, foo_playcount and Enhanced Playback Statistics, each with a preview and a backup.
- **Works with your existing scripts:** the `2003_` prefix, and the field names of foo_playcount, Last.fm Playcount
  Sync and Enhanced Playback Statistics.
- **Rate and love** from the right-click menu, and an Ultimate Playcount section in the Properties dialog.
- **Tag edits follow the track:** a record can move with the track when its tags change, and the record key is
  yours to set.
- **Clean database** tool, optional automatic cleanup, and automatic backups.

## Quick start

1. Install the component.
2. Using Playcount 2003 or foo_playcount? Keep it installed for now and read **Moving from your old component** below.
3. Add a field to a playlist column or a panel, for example `%upc_playcount%`.
4. Right-click a track › **Ultimate Playcount** to rate it or love it.
5. Settings are in **Preferences › Tools › Ultimate Playcount**.

## Fields

Every field starts with `upc_` by default. If you set the prefix to `2003_` in the settings, scripts written for
Playcount 2003 work as they are.

| Field | What it shows |
|---|---|
| `%upc_playcount%` | Number of plays (0 if never played) |
| `%upc_rating%` | 1 to 5, empty if not rated |
| `%upc_loved%` | 1 if loved, otherwise empty |
| `%upc_added%`, `%upc_first_played%`, `%upc_last_played%` | A date like `2026-10-16 13:05:09` (your local time) |
| the same with `_ts` on the end | The date as a Unix timestamp |
| the same with `_ago` on the end | How long ago, like `3wk 2d`, `5d 4h`, `2h 7m` |
| the same with `_ago2` on the end | Days ago: `Today`, `Yesterday`, `1y 2m 1w 3d` |
| `%upc_now%`, `%upc_now_ts%` | The current time |
| `%upc_plays_7d%`, `%upc_plays_30d%`, `%upc_plays_365d%` | Plays in the last 7, 30 or 365 days |
| `%upc_played_times_js%` | Every play as a list of Unix times in milliseconds, like `[1760620000000,1760700000000]` |
| `%upc_skipcount%` | Number of skips (needs **Count skips**) |
| `%upc_last_skipped%`, `%upc_last_skipped_ts%` | When the last skip was |
| `%upc_lastfm_playcount%`, `%upc_lastfm_loved%` | What Last.fm says about you and this track |
| `%upc_lastfm_first_played%`, `%upc_lastfm_last_played%` | First and last play according to Last.fm |
| `%upc_lastfm_played_times_js%`, `%upc_lastfm_played_times%` | Last.fm's plays of the track (milliseconds or dates) |
| `%upc_listenbrainz_playcount%`, `_first_played`, `_last_played`, `_played_times_js`, `_played_times` | The same, from ListenBrainz |

### Fields from other components

If your scripts use the names of another component, you can switch its names on (Preferences › Tools › Ultimate
Playcount › Records and fields, then restart). Do not switch one on while that component is still installed.

- **foo_playcount:** `%play_count%`, `%first_played%`, `%last_played%`, `%added%`, `%rating%`
- **Last.fm Playcount Sync:** `%lfm_playcount%`, `%lfm_loved%`, `%lfm_first_played%`, `%lfm_last_played%`, with
  `_ts` and `_ago`, and `%lfm_now%`, `%lfm_now_ts%`
- **Enhanced Playback Statistics:** `%played_times%`, `%played_times_js%`, `%first_played_enhanced%`,
  `%last_played_enhanced%`, `%added_enhanced%`, and for Last.fm `%lastfm_play_count%`, `%lastfm_loved%`,
  `%lastfm_first_played%`, `%lastfm_last_played%`, `%lastfm_played_times%`, `%lastfm_played_times_js%`.
  (`%played_times_raw%`, `%lastfm_added%` and `%artist_last_played%` are not provided.)

## Properties and right-click menu

- **Properties › Details** has an **Ultimate Playcount** section: play count, rating, loved, first and last
  played, added, recent plays and skips. Last.fm and ListenBrainz get their own sections, so the sources are never
  mixed. With several tracks selected you see totals and the earliest and latest dates.
- **Right-click › Ultimate Playcount:** Rating (set 1 to 5, clear, one up, one down), Love, Unlove, **Update from
  Last.fm** and **Forget Last.fm numbers**.
- Scripts that call the old `Playcount 2003/Rating/...` menu paths need the new name, `Ultimate Playcount/Rating/...`.

## Settings

Preferences › Tools › Ultimate Playcount. There are sub-pages **Records and fields**, **Cleanup and backups**,
**Last.fm** and **ListenBrainz**.

| Setting | Default and what it does |
|---|---|
| Count a play after | 50 % of the track. Or a number of seconds you listened (the whole track if it is shorter), or reaching the end. Streams and tracks without a length: 60 seconds. |
| Ignore tracks outside of library | Off. When on, those tracks are never counted or rated. |
| Count skips | Off. When on, a track you stop or change after at least a second, before it counted as a play, adds a skip. A track that plays to its end is not a skip, and neither is the playlist ending or foobar2000 closing. Skips are counted from the moment you switch this on. |
| Log play counting details | Off. Prints to the console each play start, how long you listened, and whether it was logged. It also prints each Last.fm lookup, except the slow background ones. |
| Log every playback event | Off. For finding problems: prints every event foobar2000 sends. |
| Record key (restart) | See **How tracks are told apart** below. |
| Ignore upper/lower case in the key (restart) | On. "Of The" and "of the" are one song. |
| When tags are edited | On. Edits made in foobar2000 carry the plays, dates and rating over to the new tags. Off: the edited track starts over and the old record stays. |
| Field prefix (restart) | `upc_`. Use `2003_` to keep Playcount 2003 scripts working. |
| Fields from other components (restart) | Off. See above. |

### How tracks are told apart

Every track belongs to a record, found by a "key" made from its tags. The default key is
`%album artist%|%album%|$year(%date%)|$max($num(%discnumber%,1),1)|%artist%|%tracknumber%|%title%`. Tracks with the
same key share one record. If you change the key, the next start tells you how many records would move and asks
first. A backup is made, and records that end up together have their plays added up.

## Moving from your old component

1. Keep the old component installed for now, and install Ultimate Playcount next to it. With the default `upc_`
   prefix the two do not clash.
2. **Library › Ultimate Playcount › Import from Playcount 2003...** and/or **Import from foo_playcount...**.
   You see what would change before anything is written, and a backup is made first. foobar2000's own database
   is only read, never changed.
3. Nothing is added up: each track's play count and first and last played dates are replaced with the old
   component's, so the import you run last wins. Its added date, rating and loved mark are taken when it has
   one (an empty one does not clear yours).
4. Compare `%upc_playcount%` with `%2003_playcount%` for a while if you like.
5. Remove the old component. Either change your scripts from `%2003_...%` to `%upc_...%`, or set the field prefix
   to `2003_` and restart. Importing again would replace the counts with the old component's again, even over plays
   Ultimate Playcount counted in the meantime, so do it before you rely on the new counts.

Good to know:

- Playcount 2003 records are matched through the file they point to. Records whose file foobar2000 no longer
  knows cannot be matched. The preview counts them.
- foo_playcount's data can only be read while foo_playcount is installed. Library and playlist tracks are
  imported.
- **Import from Enhanced Playback Statistics...** keeps its two kinds of data apart. Its Last.fm scrobbles become
  the Last.fm numbers of tracks that have none yet. Its local plays go to the play history only (the ones from
  before the history's first play of the track). No play count, date, rating or loved mark changes. The component
  must be installed, and "Also provide Enhanced Playback Statistics fields" must be off while you import. Importing
  twice changes nothing.

## Last.fm numbers

Preferences › Tools › Ultimate Playcount › **Last.fm**: your user name and a free API key (get one at
last.fm/api/account/create). Only public history can be read, so no password is needed.

These are Last.fm's own numbers. Ultimate Playcount asks Last.fm, for each track, how often you played it, whether
you loved it, and when. It sends the track's artist and title, and Last.fm does the matching. Nothing is guessed on
this side. A track Last.fm does not know shows 0. A track not asked about yet shows nothing.

When Last.fm is asked:

- Every time a track starts playing.
- About 30 seconds after a track ends, however it ends, so the scrobble has time to arrive.
- Slowly in the background, when nothing else is waiting: one track every 10 seconds, tracks never asked first,
  then the oldest numbers. A 25,000-track library takes about 3 days for one round.
- When you ask: right-click › **Update from Last.fm**.
- **Library › Ultimate Playcount › Update Last.fm numbers for all tracks...** fills in the whole library much
  faster (about 4 requests a second, so a big library still takes hours). You can keep using foobar2000.
  **Stop Last.fm updates** drops the rest.

Each of the three automatic kinds (track start, track end, background) has its own switch on the Last.fm page. All
are on by default.

**Console.** Lookups do not print one line each. After every 50 tracks (you can change the number on the Last.fm
page) there is one summary line, for example:
`Last.fm: checked 50 tracks, updated 7 tracks with 23 new plays, 2 newly loved; 1204 waiting`. A long run also
prints one last line when it ends. Failures are counted in the line, with the last reason.

More:

- **Show the Last.fm or ListenBrainz play count instead of the local one when it is higher** (off by default)
  changes only what `%upc_playcount%` shows: the highest count, and the earliest first play and latest last play.
  Your records stay as they are.
- **Library › Ultimate Playcount › Import loved tracks from Last.fm...** marks the tracks you loved on Last.fm as
  loved here, after a preview. It only adds marks and never removes one. Artist and title must match exactly (upper
  and lower case and extra spaces do not matter). Nothing is sent back to Last.fm.
- **Forget Last.fm numbers** (right-click) removes what is stored for the selected tracks.
- Last.fm numbers are stored apart from the plays counted here. The only things sent to Last.fm are your user
  name, your API key and the artist and title of the tracks asked about.

## ListenBrainz

Preferences › Tools › Ultimate Playcount › **ListenBrainz**: your user name. Only public listens can be read, so
no token is needed.

ListenBrainz cannot be asked about a single track. It only offers your whole list of listens (and a list of your top
1000 tracks). So Ultimate Playcount downloads the listens and counts them per track itself. A listen belongs to a
track when:

- its MusicBrainz recording id is the same as the one in your track's `MUSICBRAINZ_TRACKID` tag (ListenBrainz
  attaches that id to the listen itself), or
- its artist and title are exactly the same. Upper and lower case and extra spaces do not matter. For a track with
  several artists, the first artist and all artists joined with ", " are tried.

Nothing else is guessed.

- **Library › Ultimate Playcount › Sync ListenBrainz history...** downloads the listens since the last sync
  (everything the first time), shows how many are new, and adds them only after you say yes. A backup is made
  first. **Download all ListenBrainz history...** reads the whole history again.
- ListenBrainz listens are kept apart from your local plays and from Last.fm. Syncing is manual.
- Nothing is sent to ListenBrainz except the request for your public listens.

## Clean database

**Library › Ultimate Playcount › Clean database...** lists records that do not belong to a track foobar2000 knows
(library or playlists):

- **Tags changed:** the file is still there but its tags changed, so it has a new key. Untick a row to merge the
  old record into the current one (the plays are added up).
- **No track:** the file is not in the library or playlists. The File column shows whether it is still on disk or
  missing. Untick a row to remove it.

Ticked rows stay as they are. Records with plays, ratings or a file still on disk start ticked. There are buttons
to keep everything with plays or ratings, keep all, or keep none. A backup is made before anything changes.

You can also let old records be removed automatically (off by default): records of deleted files with no rating
or loved mark, not played for a set number of days (365 by default). It skips files whose drive is offline, and it
does nothing if it would remove more than a quarter of the database.

## Your data and backups

Everything is in one file, `ultimate_playcount.db`, in the foobar2000 profile folder. Each record also keeps the
artist, title, album and last known path, so records of tracks that are gone can be recognised later. Nothing is
removed unless you turn on the automatic cleanup.

Backups go to the `ultimate_playcount_backups` folder next to it:

- An automatic one at startup (every day by default).
- One before every bulk change (an import, a key change, a cleanup, a Last.fm or ListenBrainz sync).
- Files are named `ultimate_playcount-<kind>-<date>.db`. The newest 10 of each kind are kept (you can change that).

To restore one: close foobar2000 and copy it over `ultimate_playcount.db`. If `-wal` or `-shm` files from a version
before 0.12.1 are next to it, delete them. The `.db` file alone is the whole database, so copying it while
foobar2000 is closed gives a complete copy.

## What changed

See [CHANGELOG.md](CHANGELOG.md) for what changed in each version.

## Build

You need Visual Studio 2022 (or Build Tools 2022) with *Desktop development with C++* and a Windows SDK. The
foobar2000 SDK (2026-10-01) and the SQLite 3.54.0 amalgamation are included in `sdk/` and `third_party/sqlite/`.

```powershell
scripts\build.ps1          # builds x64 Release, runs the tests, writes dist\foo_ultimate_playcount-<version>.fb2k-component
scripts\install-test.ps1   # copies the build into a portable foobar2000 (close it first; never your real profile)
```

Where things are:

- `src/play_counter.h`: the counting logic. It does not need foobar2000.
- `src/store.*`: the SQLite database, with a copy in memory for fast field lookups.
- `src/lastfm*.cpp`, `src/listenbrainz*.cpp`: Last.fm and ListenBrainz.
- `src/time_format.h`: how dates are written as text.
- `src/main.cpp`: the foobar2000 side (playback, fields, menus, imports).
- `tests/core_tests.cpp`: unit tests for the parts that do not need foobar2000.
