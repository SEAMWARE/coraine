### New

- `GET /admin/log/lines` - the broker's recent log through the admin API, whatever `--logTo` is (stdout in a
  container or under systemd included): the last lines, by type (`types=EW`), by text (`match=`), since a
  time (`since=`), each with its UTC time first. The broker keeps them in memory, `--logLines` KiB (256 by
  default; 0: none). Feature switch `COR_FEATURE_LOG_LINES`. Needs corLog#12.
