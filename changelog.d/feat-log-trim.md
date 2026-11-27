### New

- The log file kept in bounds ([doc/log-file.md](doc/log-file.md)): when the disk holding `<logDir>/coraine.log` is
  full to `--logTrimAt` percent (90), the broker trims the file - repeated warnings and errors merged into one line
  `(<n> times, <first> - <last>)`, then the oldest lines of least importance dropped (traces, verbose, FIXME, info,
  warnings - never errors, nor the newest `--logTrimKeep` lines) until the disk is under `--logTrimTo` percent (85)
  or the log at half its size. What goes is kept gzipped for `--logTrimKeepFor` seconds (a day). Over the admin API:
  `POST /admin/log/trim` (trim now), `GET /admin/log/trimmed` and `GET`/`DELETE /admin/log/trimmed/{name}` (the kept
  files, Range supported), the limits in `GET /admin/log` and changed with `PUT /admin/log?trimAt=&trimTo=&trimKeep=&trimKeepFor=`.
  Each trim says what it did in an `I:` line starting `Log trimmed`, written whatever the log level.
  Compiled in with `COR_FEATURE_LOG_TRIM` (on); the broker now links zlib.
