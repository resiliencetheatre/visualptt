# Optional multi-peer delivery

VisualPTT still runs without a collector, SQLite, Syncthing, or any network
service. Its existing `[pttkey] input_dir` and `output_dir` settings are unchanged.
Keep the existing two-peer arrangement if that is all you need. The collector
is a separately launched companion for hosts receiving from several origins.
It only copies remote pools into a private inbox; it never forwards that inbox
or removes files from a remote pool.

## Build and run

```sh
make visualptt-collector
sudo make install-collector
mkdir -p ~/.config/visualptt ~/.config/systemd/user
install -m 600 visualptt-collector.ini ~/.config/visualptt/collector.ini
```

Edit the sample config for your user and paths, and create all its directories.
`input_dir` and `state_dir` must be owned by the service user and not writable by
other users; mode 0700 is recommended. Keep the state directory on durable local
storage. Source directories must be readable. The optional own pool must be
writable if purge is enabled. Paths must be absolute, with no `.` or `..`
components or symlinks anywhere in their ancestry. No configured directories
may overlap, alias each other, or be nested. Place the state directory outside
**every** synchronized folder (including unconfigured Syncthing folders).
Directory inode/ancestor checks also reject aliases encountered through bind
mount paths. Keep mount mappings fixed while the collector is running.

```sh
visualptt-collector --config ~/.config/visualptt/collector.ini --once
visualptt-collector --config ~/.config/visualptt/collector.ini
```

`--once` recovers pending work and performs one scan. With `stable_scans=2`, use
the running daemon: the unchanged-stat history is deliberately in memory and
restarting a one-shot process does not count as a second scan.

For the user service:

```sh
install -m 644 visualptt-collector.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now visualptt-collector.service
journalctl --user -u visualptt-collector.service -f
```

The service does not launch or require VisualPTT or Syncthing. For operation
without a logged-in session, configure user lingering using your system's
normal administration procedure. Nothing enables services during `make install`.

`make all` and `make install` also build/install the collector. It is a native
C executable linked to SQLite, with a built-in streaming SHA-256 implementation. It does not
invoke an interpreter or helper process. Building, testing and running the
collector require no Python. On Debian, install the development dependencies:

```sh
sudo apt install build-essential pkg-config libsqlite3-dev
make visualptt-collector
make test-collector
```

The collector has no GTK, GStreamer, SQLite server, or Syncthing API dependency.
The collector does not require OpenSSL. Its SHA-256 digests remain compatible
with existing ledger entries and pending copies. The regular TX, RX, and
annotation executables do not acquire SQLite dependencies. Linux `renameat2(RENAME_NOREPLACE)` and a filesystem
supporting atomic no-replace rename and directory fsync are required; errors
fail closed.

The native implementation preserves the existing collector INI format,
SQLite schema, delivery identities, pending-copy names, and deployment binding.
To replace the earlier interpreter-based implementation, stop its service,
replace the binary, and restart using the same config, state and inbox. Keep
the ledger and pending inbox files: completed rows remain suppressed and
pending deliveries recover without resetting history.

## Configuration

See [visualptt-collector.ini](visualptt-collector.ini). Unknown settings and
sections, duplicate source IDs, and the local ID in `[sources]` are rejected.
Peer IDs are stable, case-sensitive ASCII letters, digits, and hyphens, 1–32
characters. This restricted alphabet is the safe filename encoding: no escaping
or ambiguous underscore parsing is needed. Keep IDs globally unique. Each
source ID maps to exactly one locally synchronized pool directory. Never put
your own output pool in `[sources]` under a different ID.

| Setting | Default | Meaning |
| --- | --- | --- |
| `local_peer` | required | This host's stable origin ID |
| `input_dir` | required | Private VisualPTT inbox |
| `state_dir` | required | Private directory for `ledger.sqlite3` and its SQLite sidecars |
| `poll_seconds` | 2 | Full scan interval; startup files and missed events are recovered |
| `stable_scans` | 1 | Either 1 or 2 unchanged metadata scans before copy |
| `own_pool` | absent | This origin's output pool; distinct from all incoming sources |
| `purge_enabled` | false | Explicit permission to purge only `own_pool` |
| `purge_interval_seconds` | 900 | Own-pool observation/purge interval |
| `retention_seconds` | 172800 | Minimum age since local first observation (48 hours) |

Syncthing downloads to temporary names and publishes a final `.mkv` name by
rename. The collector treats that final name as the producer's completion
signal. Shared-storage producers must follow the same rule: finish and close a
non-final temporary file, then rename it into place. Never write or mutate a
published `.mkv` in place. `stable_scans=2` adds a delay/check, **not proof of
completion**: a paused writer can have a stable size. Producers and same-user
processes are trusted to respect publication and private-directory ownership.
The collector rejects symlinks, hardlinked files, empty files, directories,
FIFOs, temporary names, malformed dates, unsafe basenames, and extensions other
than exact lowercase `.mkv`. It does not decode media; any nonempty regular
file with a valid recording name can be delivered. Rejections/copy failures
never delete the source. Keep filesystem clocks and local system time accurate.

## Three peers: A, B, C

Use three Syncthing folders, one per origin, shared with both other devices.
For example, the host-local paths below use `/srv/visualptt` as their base
(create/chown them for the user running the programs):

| Host | Its VisualPTT output pool | Collector sources | Private VisualPTT input | Private state |
| --- | --- | --- | --- | --- |
| A | `outgoing` (folder `pool-A`) | B → `pools/B`, C → `pools/C` | `inbox` | `collector-state` |
| B | `outgoing` (folder `pool-B`) | A → `pools/A`, C → `pools/C` | `inbox` | `collector-state` |
| C | `outgoing` (folder `pool-C`) | A → `pools/A`, B → `pools/B` | `inbox` | `collector-state` |

Thus `pool-A` maps to A's `/srv/visualptt/outgoing`, B's
`/srv/visualptt/pools/A`, and C's `/srv/visualptt/pools/A`. Map `pool-B` and
`pool-C` analogously. Each host's `pttkey.ini` points `output_dir` to its
`outgoing` and `input_dir` to its `inbox`. Configure the collector with its
local A/B/C ID and the two other IDs/paths. The sample config is A's version.
Syncthing folder mappings belong in Syncthing deployment configuration, never
in `pttkey.ini` or VisualPTT application configuration.

Sync **only** the output pools/remote pool replicas. Never sync `inbox`,
`collector-state`, or TX's working/staging directory. Configure the pool
relationships as normal bidirectional **send-and-receive** folders so that the
origin's deletions propagate, including to devices still catching up. Do not
use receive-only folders for this layout. Only the origin application/purge
may modify its pool; treat remote replicas as read-only operationally.

A single A recording can exist in A's outgoing and both B/C remote pools.
B and C each receive their own private copy, prefixed `A_`. B deleting/playing
its inbox copy has no effect on C's copy or any pool. If C was offline, it
catches up from A's still-retained pool when synchronization resumes. A pool
file purged before C finishes fetching it is lost to C; deletion propagates
even if C is still catching up. This is **best-effort delivery within a
retention window**, not guaranteed delivery to every peer. No per-recipient
acknowledgements are collected.

## Names and annotations

Both transmitters use the same `recording.c` helpers. Each recording has a
local start-time name `rec_YYYYMMDD_HHMMSS_<32 lowercase hex digits>.mkv`.
`getrandom()` supplies a fresh 128-bit ID; entropy failures stop recording.
The working directory holds an exclusively created
`.rec_YYYYMMDD_HHMMSS_<id>.mkv.part` reservation (0600). Reservation collisions
retry with a fresh ID. A retained descriptor is passed to `fdsink`; no sink
reopens the path. Inspection of GStreamer 1.26.2's
[filesink source](https://github.com/GStreamer/gstreamer/blob/1.26.2/subprojects/gstreamer/plugins/elements/gstfilesink.c)
confirmed default `"wb"`/`O_TRUNC` behavior, while
[fdsink](https://github.com/GStreamer/gstreamer/blob/1.26.2/subprojects/gstreamer/plugins/elements/gstfdsink.c)
uses the supplied descriptor and leaves closing to its owner. Merely reserving
a pathname before giving it to `filesink` would therefore not be sufficient.

After successful EOS, the transmitter fsyncs/closes the recording, copies it to
an exclusive temporary file inside `output_dir`, fsyncs/closes that copy, and
renames without replacement. This also supports an output on a different
filesystem. The source is removed only after successful durable publication.
An existing destination is always an error, even if its bytes match. On
failure the working-directory `.part` recording is retained; errors identify
its recovery path. Keep that directory writable and private. With no
`output_dir` in standalone TX, completed recordings are published in the
working directory. Failed/incomplete recordings stay hidden `.part` files;
inspect them before manually recovering and never blindly overwrite a pool
file. A publication failure after rename/fsync may leave both copies; compare
them before cleanup. An interrupted TX copy can leave a hidden
`.visualptt-<id>.part` in output; remove those only when TX is stopped. They are
never treated as messages or automatically purged.

The collector delivers `A_rec_YYYYMMDD_HHMMSS_<id>.mkv`. Its identity is the
pair `(origin, full original basename)`. Legacy `rec_YYYYMMDD_HHMMSS.mkv`
files remain accepted by receiver, watcher and collector. Legacy names can
collide within an origin/second or be reused; after delivery, reuse of that
identity is intentionally suppressed, even if the replacement bytes differ.
Use the updated transmitters to avoid that limitation. Do not recycle IDs.

The watcher validates the complete name/date/extension for timestamp display,
including legacy, new, and prefixed forms. Optional printing still shows
`YYYY-MM-DD HH:MM:SS`. WAV and TXT companions use the **entire** MKV stem,
including origin and random suffix. The existing list receiver looks up and
deletes these companions by that same full stem. The watcher extracts audio
and runs Whisper speech-to-text; it does not perform text-to-speech.

## Durability, recovery, and collisions

Only one collector may lock an inbox or state directory at a time. Keep state
on a local filesystem with working fsync and SQLite locking. Copying is
streamed and records SHA-256 once; ordinary scans query delivered identities
and do not repeatedly hash completed recordings. Source device/inode, size,
mtime and ctime are checked before/after the copy, including the directory
entry. A changed/disappeared source discards the temporary copy and retries.

The journal protocol is:

1. Copy to a unique hidden `.visualptt-collector-<id>.part` **inside** the inbox,
   fsync/close it and fsync the inbox directory. No final MKV exists yet.
2. Commit its name, digest and delivery identity in SQLite with FULL synchronous
   durability. This is a pending delivery intent.
3. Atomically rename the temp to the origin-prefixed final name, without
   replacement, then fsync the directory. Mark the ledger row completed.

Recovery examines the durable intent. If its temporary name still exists,
it verifies the copy and retries publication. If the temp is absent, rename
has completed (the receiver may already have deleted the final file), so it
marks delivery completed without replay. Unjournaled collector temporary
files are remnants of interrupted copies and are removed on startup. A crash
before intent retries from source; a crash after rename never depends on final
file existence. The policy assumes the private staging files and journal are
not removed by an outside cleaner. Never manually delete a pending temp or
roll back only the database. Loss of/corruption to state requires operator
recovery from a consistent backup; deleting the database is not a repair.
Process-crash boundaries are covered with actual SIGKILL tests. Power-loss
durability relies on the filesystem's rename/fsync contract and honest storage;
it is not a distributed transaction with the playback application's own state.

If a final destination exists, only identical verified bytes satisfy the
intent; a symlink, changed file, or different bytes is a logged error. The
pending copy and source are retained, and the collector retries without
overwriting. Stop the collector, inspect/quarantine the conflicting final
file, then restart to publish the pending copy. Do not delete the pending temp
to resolve a collision. Normal delivered identities stay suppressed even if
the inbox, source or source directory contents later disappear/reappear.

Keep delivered rows **indefinitely**. They are compact tombstones that survive
pool purges and inbox deletion. There is deliberately no age-based ledger-row
pruning: an old pool backup or offline replica can reintroduce an identity.
For maintenance, stop the collector and back up the entire state directory,
then use SQLite checkpoint/VACUUM without deleting delivery rows. Back up
pending inbox temps with state if a snapshot includes pending intents. State
is bound to the local ID and private/own-pool paths; moving these requires an
explicit coordinated migration with collectors stopped, preserved inbox
pending files, and an audited update of the `metadata` binding. Source paths
and additional remote IDs can change without discarding tombstones. Never
use a fresh ledger to forget a still-accessible historical identity.

## Origin purge and storage

Purge is **off by default**; enable it only for this origin's explicit
`own_pool`. The daemon checks immediately and then every 15 minutes by default.
A trustworthy conservative publication-age lower bound is persisted locally:
the time the origin collector first observes a valid final file with that
identity/metadata. It does not trust the timestamp encoded in its name, its
mtime, or remote syncing metadata as a creation time. Files already present
at startup get a full retention period starting from first observation.
Metadata/inode changes or a clock moving behind the recorded observation reset
the retention period. Downtime before first observation extends retention.
Retention uses the origin's system clock; large forward clock corrections can
shorten real elapsed retention, so maintain a reliable clock.

After 48 hours (configurable), unchanged, nonempty regular own-pool files are
eligible. Purge obtains an advisory exclusive file lock and rechecks identity
before unlinking. Producers must publish once by rename and never edit final
files; TX's active `.part` files cannot match. Unreadable, locked, changed,
symlinked or otherwise unsafe files are retained and failures logged. Repeated
purges are idempotent and each deletion is logged. No last-N exemption is
implemented: age alone controls eligibility. Purge never touches inbox copies,
remote sources, annotations or delivered ledger rows.

Allow space for all pools on every host, independently consumed inbox copies,
annotation WAV/TXT files, temporary copies during delivery, and a growing
ledger. Unconsumed inboxes are not aged out by this collector. A disabled or
stopped purge means the origin pool keeps growing; retained failures and hidden
staging files need operator inspection. The effective offline catch-up window
is bounded by origin retention, synchronization delays and the purge schedule,
not by whether another peer has played the message.

## Verification

`make test-delivery` covers shared TX naming/reservation, same-second names,
actual GStreamer descriptor sink behavior, no-overwrite publication, legacy/new
annotation and print timestamps, multiple origins, existing startup files,
source mutation/replacement, short writes, interrupted copy, SIGKILL at intent,
rename, consumer-deletion and ledger boundaries, rescan after inbox deletion,
symlinks/temp files, collision recovery, disjoint-path validation, own-pool
retention/restart/failure and simulated disconnected-peer history. Syncthing
network behavior and physical camera/keyboard/printer hardware need deployment
checks; automated tests simulate pool synchronization and consumer deletion.
The collector regression suite is C and includes upgrade-ledger compatibility,
short writes and SQLite failure injection. `make test-collector-sanitize` runs
the same native suite under AddressSanitizer and UndefinedBehaviorSanitizer.
Run `make test-xmpp` for the existing independent exporter regression suite.
