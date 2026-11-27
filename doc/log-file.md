# The log file kept in bounds

A broker that logs to a file (`--logTo file` or `both`, [Running the broker](installation.md#running-the-broker-foreground-or-daemon-and-where-the-log-goes))
keeps that file from filling its disk. When the disk holding the file is full to a limit, the broker **trims**
the file: it drops the oldest lines of least importance and merges repeated warnings and errors into one line.
It keeps what it dropped, gzipped, until it is fetched or a time has passed. A trim can also be asked for at any
time over the admin API, and every limit can be changed on a running broker.

A broker that logs to stdout (the default) has no file, and nothing here applies to it: there the container
runtime or the journal keeps the log in bounds.

The feature is compiled in with `COR_FEATURE_LOG_TRIM` (on by default, [Building](building-details.md)); a broker
built without it has none of the options below, and the admin API answers its requests with 501.

## When a trim runs

- **The disk:** a thread of the broker looks at the log file's disk once a second (`statvfs`). Full to
  `--logTrimAt` percent (default 90), the log is trimmed. The percentage is the one `df` shows (`Use%`): the
  blocks a filesystem reserves for root (5 % of an ext4 filesystem by default) are not counted as free.
  `--logTrimAt 0`: never for the disk, only on request.
- **On request:** `POST /admin/log/trim` (the `admin` API plugin) trims the log now, whatever the disk.

After a trim for the disk, the next one waits until the log has grown again - by a tenth of the size the trim left
it at, and at least 1 MiB. A disk that other files fill is not helped by trimming the log again every second; the
broker says so instead, in a warning after the trim (here a test, with `trimAt` set to the disk's 11 %):

```text
W: 000081.006: logTrim.c[1044]: trimSay: the disk is still 11% full after the log trim - other files than the log fill it
```

## What a trim does

1. **Merges** repeated warnings and errors. Every `W:` or `E:` line whose text - all of it after the time - is the
   same as another's becomes one line: the last of them, with `(<n> times, <first> - <last>)` at its end. The two
   times are the lines' own, the broker's uptime, as every line of the file has it. Nothing of what they said is
   lost, so this comes first.

   ```text
   W: 000002.290: getEntity.c[983]: entityRetrieve: 404 Entity not found: entity 'urn:ngsi-ld:Pump:1' not found (25 times, 000002.061 - 000002.290)
   ```

2. **Drops** the oldest lines by type, the least important first: traces (`T:`), then verbose (`V:`), FIXME (`F:`),
   info (`I:`) and warnings (`W:`). All the lines of a type go before any of the next; of the last type it needs,
   the oldest go. Never dropped:
   - errors (`E:`) and exits (`X:`);
   - the newest `--logTrimKeep` lines (default 10000), whatever their type;
   - a warning that is one of a merged repeat (it is one line already);
   - a line before the broker's first line in the file, which is not one of its own.

   A line written over several lines of the file (a message with newlines in it) is one line here: it stays or goes
   whole.
3. **Stops** when what the merge saved and what it dropped together bring the disk under `--logTrimTo` percent
   (default 85), or the log to half its size, whichever comes first. A trim on request goes to half the size.
4. **Keeps** what it dropped and merged away, in the order it was in the file, gzipped beside the log:
   `<logDir>/coraine.log.trimmed-<UTC time>.gz` (`-2`, `-3` after the time for a second trim in the same second).
   `--logTrimKeepFor 0`: nothing is kept.
5. **Says** what it did, in one `I:` line that is written whatever the log level is (also without `-v`). It always
   starts with `Log trimmed`, so a tool can find it:

   ```text
   I: 000002.345: logTrim.c[1034]: trimSay: Log trimmed (request): dropped V 21; 48 repeated lines merged; 14 KiB -> 6 KiB, the disk 11% full; the trimmed-off lines in coraine.log.trimmed-20261127T174941Z.gz
   I: 000081.006: logTrim.c[1034]: trimSay: Log trimmed (disk 11% >= 11%): dropped V 66; 8 repeated lines merged; 15 KiB -> 7 KiB, the disk 11% full; the trimmed-off lines in coraine.log.trimmed-20261127T175059Z.gz
   ```

### How the file is trimmed while the broker writes to it

The broker goes on logging during a trim, and no line is lost:

1. The file is renamed `coraine.log.trimming`, and a fresh `coraine.log` takes its place - under the log's own lock,
   with `dup2` onto the descriptor the broker writes to, so no line is written to a closed file. The lines logged
   from then on go to the fresh file.
2. The renamed file is read, and what stays is written to `coraine.log.tmp`, what goes to the gzipped file. Every
   8 MiB read, the space of what was read is given back to the disk (a hole punched in the renamed file), so a log
   that filled its disk is trimmed in the little space that is left.
3. Under the log's lock: the lines the fresh file got meanwhile are appended to `coraine.log.tmp`, which is renamed
   `coraine.log` and put in place of the descriptor. `coraine.log.trimming` is deleted.

A tool that follows the file by its name (`tail -F`) keeps following it. logrotate's `copytruncate` works as before
([The log file](installation.md#the-log-file)).

If something fails on the way (no space even for the trimmed file, a directory that cannot be written), nothing is
thrown away: the broker logs to the fresh file, `coraine.log.tmp` holds the log up to where the trim got and
`coraine.log.trimming` the rest. An `E:` line says what failed and where the trim got, in this form:

```text
E: <uptime>: logTrim.c[<line>]: trimSay: Log trim failed (<disk|request>): cannot write <logDir>/coraine.log.tmp: <the system's reason> - <logDir>/coraine.log.tmp holds the log up to byte <n> of <logDir>/coraine.log.trimming
```

A broker started where a trim of an earlier run did not finish says so with a warning, and leaves the files as they
are.

## The options

| Option | Environment variable | Default | Values |
|--------|----------------------|---------|--------|
| `--logTrimAt` | `CORAINE_LOGTRIMAT` | `90` | 0-100: % full the log file's disk may get - 0: never for the disk |
| `--logTrimTo` | `CORAINE_LOGTRIMTO` | `85` | 1-99: % full a trim for the disk brings it under (or the log to half its size, whichever comes first) - under `--logTrimAt` |
| `--logTrimKeep` | `CORAINE_LOGTRIMKEEP` | `10000` | 0-10000000: the newest lines a trim always keeps |
| `--logTrimKeepFor` | `CORAINE_LOGTRIMKEEPFOR` | `86400` | 0-2592000: seconds the trimmed-off lines are kept - 0: not kept |

A `--logTrimTo` that is not under `--logTrimAt` (with `--logTrimAt` over 0) stops the start, exit code 1:

```text
X: 000000.000: coraine.c[2137]: main: --logTrimTo 60: must be under --logTrimAt 50
```

The options are read with any `--logTo`; with `stdout` there is no file to trim, and `GET /admin/log` says the
limits all the same. Each option is also in [Command-line options](options.md#the-program), and its value and where
it came from in `GET /admin/config`.

```sh
coraine --logTo file                                                # the defaults: 90 %, 85 %, 10000 lines, a day
coraine --logTo file --logTrimAt 95 --logTrimTo 80 --logTrimKeepFor 604800   # later, deeper, a week
coraine --logTo file --logTrimAt 0                                  # only on request
```

## Over the admin API

With the `admin` API plugin loaded (`--apiPlugins admin`, [Administration](installation.md#administration)):

| Request | What it does |
|---------|--------------|
| `GET /admin/log` | the log settings, and with them `"file"` (the log file, `null` for stdout) and `"trim"`: the limits now, `"disk"` (% full now, `null` without a file) and `"last"` (the last trim since the start, `null` for none) |
| `PUT`, `POST` or `PATCH /admin/log?trimAt=&trimTo=&trimKeep=&trimKeepFor=` | the limits of the running broker - any of the four, with the log levels or without. The three verbs set them alike (they differ only for `traceLevels`). A value out of range, or a `trimTo` not under `trimAt`: 400, and nothing changes |
| `POST /admin/log/trim` | trims the log now - 200, the answer is what the trim did (the `"last"` object). 409 without a log file |
| `GET /admin/log/trimmed` | the kept files of trimmed-off lines, oldest first: `name`, `time`, `why`, `size`, `lines`, `expires` |
| `GET /admin/log/trimmed/{name}` | one of them: `application/gzip`, `Content-Disposition: attachment`. `Range: bytes=<first>-<last>` (or `<first>-`, `-<n>`): 206 with that part; a range outside the file: 416. Without a range, a file over 1 GiB is refused (400) - fetch it in parts |
| `DELETE /admin/log/trimmed/{name}` | 204 - the file is deleted. 404 when it is not kept (any more) |

A `GET` of a kept file deletes nothing: the file goes when it is deleted, or when `--logTrimKeepFor` has passed since
the trim. So a download that breaks off can be done again, and the client deletes the file once it has it whole
(its size is the `size` of the list).

The limits changed over the API are the running broker's: a restart starts with its options again. A build without
`COR_FEATURE_LOG_TRIM` has no `"file"` and `"trim"` in `GET /admin/log`, and answers 501 to the trim parameters and
to `/admin/log/trim` and `/admin/log/trimmed`.

### Examples

These ran against a broker started as `coraine --apiPlugins admin --logTo file -v --logTrimKeep 20`, after 60 requests
for two entities that do not exist (each answered 404 - a `W:` line).

```sh
curl 'localhost:1026/admin/log?pretty=2'
```

```json
{
  "verbose": true,
  "info": true,
  "traceLevels": "",
  "traces": true,
  "file": "/var/log/coraine/coraine.log",
  "trim": {
    "at": 90,
    "to": 85,
    "keep": 20,
    "keepFor": 86400,
    "disk": 11,
    "last": null
  }
}
```

```sh
curl -X POST 'localhost:1026/admin/log/trim?pretty=2'
```

```json
{
  "time": "2026-11-27T17:49:41Z",
  "why": "request",
  "diskBefore": 11,
  "diskAfter": 11,
  "dropped": {
    "V": 21
  },
  "merged": 48,
  "sizeBefore": 14443,
  "sizeAfter": 7091,
  "file": "coraine.log.trimmed-20261127T174941Z.gz"
}
```

`dropped` has the types of which lines were dropped, and how many; `merged` the repeated lines merged away;
`file` the kept file (`null`: none kept - nothing dropped, `--logTrimKeepFor 0`, or since deleted). A trim that
failed has an `"error"` too.

```sh
curl 'localhost:1026/admin/log/trimmed?pretty=2'
```

```json
[
  {
    "name": "coraine.log.trimmed-20261127T174941Z.gz",
    "time": "2026-11-27T17:49:41Z",
    "why": "request",
    "size": 613,
    "lines": 69,
    "expires": "2026-11-28T17:49:41Z"
  }
]
```

A kept file found at the start - from an earlier run of the broker - is listed with `"why": null` and
`"lines": null` (not known), and expires `--logTrimKeepFor` after its time.

```sh
curl -o trimmed.gz 'localhost:1026/admin/log/trimmed/coraine.log.trimmed-20261127T174941Z.gz'
zcat trimmed.gz | head -2
```

```text
V: 000000.000: coraine.c[2145]: main: coraine  0.7.0+dev
V: 000000.000: inlineDispatch.c[510]: inlineDispatchInit: inline dispatch: on
```

```sh
curl -s -D - -o part -H 'Range: bytes=0-499' 'localhost:1026/admin/log/trimmed/coraine.log.trimmed-20261127T174941Z.gz'
```

```text
HTTP/1.1 206 Partial Content
Content-Type: application/gzip
Accept-Ranges: bytes
Content-Disposition: attachment; filename="coraine.log.trimmed-20261127T174941Z.gz"
Content-Range: bytes 0-499/613
Content-Length: 500
```

```sh
curl -X DELETE 'localhost:1026/admin/log/trimmed/coraine.log.trimmed-20261127T174941Z.gz'    # 204
curl -X PATCH 'localhost:1026/admin/log?trimAt=80&trimTo=70'                                  # the limits now
curl -X PUT 'localhost:1026/admin/log?trimAt=50&trimTo=60'
```

```json
{
  "type": "https://uri.etsi.org/ngsi-ld/errors/BadRequestData",
  "title": "Invalid URL parameter",
  "status": 400,
  "detail": "trimTo must be under trimAt (trimAt 50, trimTo 60)"
}
```
