# Changelog

What changed in each version of Ultimate Playcount, in plain words. The list starts at version 0.14.0; everything
the component can do is in the [README](README.md).

## [0.14.0] - 2026-10-25

### Added

- The console no longer gets a line for every Last.fm lookup. Instead there is one summary line after a number of
  tracks, for example "checked 50 tracks, updated 7 tracks with 23 new plays, 2 newly loved". The number is a
  setting on the Last.fm page (50 by default). A long run also prints a last line when it ends.

### Fixed

- Last.fm is asked for the exact artist and title first. Its name correction is only used when it does not know the
  track, so a track no longer gets the numbers of a similarly named one.
- "Log play counting details" no longer prints a line for every slow background lookup.
