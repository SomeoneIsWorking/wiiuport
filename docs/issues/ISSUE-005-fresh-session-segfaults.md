# ISSUE-005 — the runtime segfaults on a fresh session directory

**State:** open. Reproduced twice, cause unknown.

## What happens

A `HeadlessSession` pointed at an activity directory that has never been used
crashes with signal 11 before the title runs. The crash report says `Game info:
Not running`, and `log.txt` holds only the stack trace. It happens with logging
off as well as on, so it is not a capture instrument firing.

The same binary, same disc image and same command run correctly against an
activity directory that has been used before: the run reaches a drawn scene and
records 5,438 draws.

## Why it is not the capture instruments

The discriminator was run rather than reasoned about. The build containing the
display list capture crashes in a fresh `scratch/displaylist/` and succeeds in
`scratch/uniform-capture/`, with the same binary in the same minute. Logging off
changes nothing, and both instruments only act from inside a draw, which this
crash never reaches.

## Why it matters beyond the harness

A player's first launch is a fresh user-data directory. If this is a genuine
first-run fault rather than something about how the harness prepares one, it
reaches ST-SETUP and the AppImage directly. That is the reason to chase it
rather than work around it by reusing a directory.

## Not yet established

- Which of the two directories' contents makes the difference. `prepare()`
  writes settings, keys and an `mlc01` tree into both; the working one has also
  accumulated shader caches and whatever a completed run leaves behind.
- Whether upstream Cemu at the same revision crashes the same way, which would
  place the fault outside this fork entirely.
- Whether a fresh *real* user-data directory crashes, or only one built by the
  harness.
