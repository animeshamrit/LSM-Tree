# minilsm

A small embedded key-value store in C++17, built brick by brick as a learning project. It is an LSM-tree storage engine: writes go to a write-ahead log (WAL) and an in-memory table (MemTable), which is flushed to sorted, immutable files (SSTables) on disk. The final goal is to store **protobuf values with schema evolution**, so old data stays readable after schema changes.

Background: this project builds on experience upgrading Protocol Buffers (3.0.x to 3.19.x) in a large C++ storage codebase at AWS Aurora, and generalizes that serialization/compatibility work into a portfolio piece.

## Status at a glance

| Area | State |
|---|---|
| WAL (record format, writer, reader, recovery, truncation of a torn tail) | Done |
| MemTable with tombstones and byte-size accounting | Done |
| SSTable format, atomic writer, reader | Done |
| Synchronous flush, numbered files, recovery from SSTables + WALs | Done |
| **Asynchronous flush (background thread, halt on failure)** | **In progress** (Brick 12e) |
| Blocks, sparse index, `pread`, block cache, Bloom filters | Not started (Brick 13) |
| MANIFEST, compaction | Not started |
| Protobuf layer with schema evolution | Not started |

## Build and test

```bash
brew install cmake            # macOS; also needs Apple Command Line Tools (xcode-select --install)
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

GoogleTest v1.14.0 is fetched automatically by CMake (`FetchContent`).

`ctest` hides the output of passing tests. To see `std::cout` / `std::cerr` output, run the test binary directly or use `ctest -V`.

Because flushing runs on a background thread, run the suite under ThreadSanitizer before trusting it:

```bash
cmake -S . -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1"
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

## Project layout

```
minilsm/
├── CMakeLists.txt
├── include/minilsm/      public headers (declarations)
│   ├── status.h
│   ├── db.h
│   ├── memtable.h
│   ├── lsm_db.h
│   ├── crc32.h
│   ├── coding.h          shared PutFixed32/64, GetFixed32/64
│   ├── wal_record.h
│   ├── wal_writer.h
│   ├── wal_reader.h
│   ├── sstable_format.h  SstEntry, BuildSstable, SstableView
│   ├── sstable_writer.h  atomic publish of an SSTable file
│   └── sstable_reader.h  ReadSstableFile
├── src/                  implementations (.cc)
└── tests/                GoogleTest files, one per component
```

Conventions: Google C++ style (`.cc`/`.h`, `kCamelCase` enumerators, trailing `_` on private members, anonymous namespaces for file-private helpers, data members declared before functions). Each component is built test-first: write the test, watch it fail to link, then implement.

## Progress so far

| Brick | What was built | Key idea |
|---|---|---|
| 1 | CMake + GoogleTest + smoke test | Prove the toolchain works before writing real code |
| 2 | `Status` and abstract `DB` interface | `Get` returns a `Status` so "not found" and I/O errors are distinguishable from values |
| 3 | `MemTable` (wraps `std::map`) | Deletes are **tombstones**, not removals; `Get` has three outcomes: found / deleted / not present |
| 4 | `LSMDB` implementing `DB` | Translates MemTable results into `Status`; in-memory only |
| 5 | WAL record encode/decode | Self-describing byte format, overflow-safe bounds checks, offset unchanged on failure |
| 6 | `Crc32` | Standard CRC-32, verified against the known vector `0xCBF43926` for `"123456789"` |
| 7 | Checksum added to WAL records | `[crc][type][key_len][value_len][key][value]`; CRC covers everything after itself, including lengths |
| 8 | `WalWriter` | Append-only file via `open/write/fsync`; handles partial writes and `EINTR`; `F_FULLFSYNC` on macOS |
| 9 | `ReadWal` | Replays the valid prefix and stops at the first bad record; reports `valid_bytes` and `clean` |
| 10 | `LSMDB::Open(dir)` with recovery | WAL first, then MemTable; replay on open; truncate a damaged tail so later writes stay reachable |
| 11 | SSTable format (in memory) | Sorted entries, full index, footer with CRC and magic number; binary search lookup |
| 12a | `SstableWriter` | Atomic publish: write `path.tmp`, fsync, close, rename, fsync the parent directory; temp file removed on failure |
| 12b | `ReadSstableFile` | POSIX `open/fstat/read` loop with offset tracking, then `SstableView::Parse` |
| 12c | `MemTable::Snapshot()`, `ApproximateSize()`, `empty()` | Sorted copy of the MemTable for flushing; byte accounting that subtracts the old entry on overwrite |
| 12d | Synchronous flush | Numbered `sst-N.sst` and `wal-N.log`, `Open` loads SSTables and replays WALs, size-triggered flush, `Get` falls through to SSTables newest first |
| 12e | **Asynchronous flush** (in progress) | One persistent worker thread and a task queue; the write path no longer waits for disk; a failed flush halts the database |

## How it works

### Files on disk

```
<dir>/
  sst-000001.sst        immutable sorted tables, one per flush
  sst-000002.sst
  sst-000003.sst.tmp    in-flight or abandoned write; deleted by Open
  wal-000007.log        write-ahead logs; the highest number is the one taking writes
```

SSTables and WALs have **independent counters**. Names are parsed strictly (`<prefix><digits><suffix>`); anything else in the directory is ignored.

### Write path

```
Put/Delete
  1. reject if the database is halted
  2. reject if the single entry is larger than the memtable limit (InvalidArgument)
  3. if this entry would push the MemTable past the limit, flush first
  4. append to the WAL            (on failure, memory is untouched)
  5. apply to the MemTable
```

### Flush path (asynchronous)

```
main thread                                   background worker
-----------                                   -----------------
snapshot MemTable, build SSTable bytes
clear MemTable
open wal-(N+1), swap the writer to it
parse the bytes into an in-memory view
install the view (reads see it at once)
enqueue task(bytes, N+1)  ───────────────►    write sst-K.sst atomically
return to the caller                          on success: K++, delete WALs older than N+1
                                              on failure: delete partial file, HALT the database
```

A WAL may be deleted **only after the SSTable holding its data is durable**. Until then the WAL is the only copy.

### Read path

`Get` checks the MemTable, then the SSTables from newest to oldest. A tombstone stops the search; "not present" continues to the next older file.

### Recovery (`Open`)

1. Scan the directory; delete `*.sst.tmp` leftovers.
2. Load SSTables in ascending order (a corrupt one fails `Open`); the next SSTable number is highest + 1.
3. Replay **every** WAL in ascending order into the MemTable. Replaying a WAL whose data is already in an SSTable is harmless, because newer entries shadow older ones.
4. Truncate a damaged tail, but only on the last WAL.
5. Append to the highest WAL (or create `wal-000001.log`).

Crash windows during a flush, and why each is safe:

| Crash happens | State left behind | Result on restart |
|---|---|---|
| Before the SSTable is renamed | `.sst.tmp` and the old WAL(s) | Temp file deleted, WAL replayed, nothing lost |
| After the rename, before the old WAL is deleted | SSTable and the old WAL | WAL replays on top of the SSTable; duplicates are harmless |
| After the new WAL is created, before the old one is deleted | Two WALs | Both replayed in ascending order |

## Design decisions worth remembering

**Tombstones.** SSTables are immutable, so a delete must be written as data. Newer entries shadow older ones. Without tombstones, a deleted key would reappear from an older file.

**Interface vs. concrete class.** `DB` is abstract (`= 0`) so we can later add a `FakeDB` (plain `std::map`) for differential testing and for testing the protobuf layer without disk. This is a design choice, not a requirement; collapsing it is easy.

**Struct vs. class.** Structs for plain data with no invariants (`WalRecord`, `WalReadResult`, `SstEntry`); classes for types that own resources or enforce rules (`Status`, `MemTable`, `WalWriter`, `LSMDB`).

**Status vs. void vs. enum.** `void` where nothing can fail (`MemTable::Put`), an enum where there are several normal outcomes (`GetResult`), `Status` where I/O or corruption is possible.

**Check, then read.** Decoders never trust lengths from disk. Bounds checks are written to avoid integer overflow (`value_len > remaining - key_len`), and checksums are verified before interpreting contents.

**Checksums detect, they don't repair.** There is no replica, so recovery policy is: stop at the first bad record, keep the good prefix (a state the database really was in), and report how many bytes were discarded. We never skip ahead past a bad record, because its length fields can't be trusted and replaying later records could produce a state that never existed.

**`ReadWal` returns the good records.** Corruption is reported in the result (`clean`, `valid_bytes`), not as a failing `Status`. `Status` means "the read operation itself failed" (missing file, I/O error). Policy (log, truncate, refuse to open) lives in `LSMDB::Open`.

**Write path order.** WAL append first, then MemTable. If the append fails, memory is untouched.

**Truncate on recovery.** If the WAL tail is damaged, `Open` truncates the file at `valid_bytes` before appending. Otherwise new records would sit after garbage and be lost on the next restart. Truncation uses `std::filesystem::resize_file` and is idempotent (repeating it after a crash gives the same result).

**`recovery_dropped_bytes`** counts the surviving fragment of the torn record (`file_size - valid_bytes`), not the bytes that were missing. For a 15-byte record chopped by 3 bytes, the value is 12.

**Durability levels.**

| Failure | `sync = false` | `sync = true` |
|---|---|---|
| Our process crashes | Safe (kernel still holds the data) | Safe |
| OS crash or power loss | Recent writes can be lost | Safe |

`sync = true` costs real latency (an `fsync` per write), which is why it is an option. Group commit and timed sync are later topics.

**SSTable layout.**

```
[entry 0]...[entry N-1][index][footer]
entry:  [key_len 4][value_len 4][type 1][key][value]
index:  per entry: [key_len 4][key][offset 8]
footer: [index_offset 8][entry_count 8][crc 4][magic 8]
```

- Written in one pass, never modified, so the index and footer come after the data.
- Entries are variable length, so the index (keys + offsets) is what makes binary search possible.
- The footer is fixed size at the end of the file, so a reader can find it and learn where the variable-size index starts.
- The magic number (`"MINILSM1"`) identifies the file type, versions the format, and cheaply detects truncation.
- Input must be sorted and duplicate-free; `BuildSstable` rejects anything else rather than silently choosing a winner.

**Atomic publish.** An SSTable is written to `path.tmp`, fsynced, closed, then renamed onto its final name, and the parent directory is fsynced. A reader or a crash therefore sees either no file or a complete one. `rename` silently overwrites its target, so file numbers must never be reused. A leftover `.tmp` is deleted by `Open` and by the next `SstableWriter::Open` for the same path.

**Independent counters for SSTables and WALs.** Early on the plan was to derive `sst-N` from `wal-N`. That couples two things that fail separately: a WAL can be created without its SSTable ever being written, and the reverse. With independent counters, recovery does not depend on the two lining up. `Open` replays every WAL it finds, so a stale WAL left by a crash is harmless.

**WAL cleanup belongs to flush, not compaction.** A WAL covers one MemTable. Once that MemTable's SSTable is durable, the WAL is redundant. Compaction merges SSTables and is a separate concern.

**WAL rotation order.** Create the new WAL first, then swap the writer, and only then delete old WALs. If the new WAL cannot be created, nothing changes and the old WAL keeps taking writes; a restart replays it in full.

**Memory cap.** The MemTable has a hard size limit (`kmaximum_bytes_`, 4 MiB). A single entry larger than the limit is rejected with `InvalidArgument("entry too large")` before it reaches the WAL. Otherwise the limit would be a suggestion. The MemTable estimates the real memory of an entry as key + value + a per-entry overhead (`kEntryOverhead`, the map node, two string objects and allocator cost, roughly 100 bytes), because small keys cost far more memory than their payload. Overwriting a key subtracts the old entry first, so repeated overwrites of one key do not grow the counter.

**Fail-stop on a failed flush.** If a background flush fails, the database halts: further `Put`, `Delete`, `Get` and `Flush` calls return an `IOError`, and a restart recovers from the WALs. The reasons:

- Once a flush has failed, its WAL is the only copy of that data and must not be deleted.
- Carrying on would let memory grow without bound, because nothing can be flushed.
- If a later flush succeeded, it would delete older WALs, including the failed flush's WAL, and a restart could replay an older WAL over a newer SSTable. Halting removes both hazards.

**One background thread.** A single persistent worker thread with a FIFO task queue keeps flushes in order, so SSTables become durable oldest to newest and a task never races another task.

**Locking rules for the async path.**

- `wal_mutex_` protects `live_wals_`. It is touched by `RotateWal` (main thread) and by `ClearWal` (worker). A `std::vector` shared between threads is a data race even when the elements touched are different, because `erase` moves the tail and changes the end pointer while `push_back` reads and bumps it.
- `queue_mutex_` and `queue_cv_` guard the task queue and the shutdown flag.
- `is_halted_` is a `std::atomic<bool>`; the error status is guarded by `status_mutex_`.
- The destructor sets the shutdown flag, wakes the worker and joins it. The worker drains the queue before exiting, so a clean close completes every accepted flush.

## Challenges and how they were resolved

**Atomic file publish had many small traps (12a).** Local `path_` shadowing; the `rename` skipped on macOS because `F_FULLFSYNC` returned early; rename result unchecked; `O_APPEND` appending to a stale `.tmp` (fixed by unlinking it first); no directory fsync; the fd left open on success, so a second call appended into the published file; and a `Close()` cleanup that would have deleted the finished file. All fixed, with a 17-test suite covering them.

**Reading a file correctly (12b).** `read(fd, buf, sizeof(st.st_size))` read 8 bytes into the same place instead of looping with an offset. `ifstream` error reporting is unreliable, so the reader uses a POSIX read loop. `Parse` takes its input by value and moves it (sink-parameter idiom).

**Size accounting was wrong in several ways (12d).** A delete only counted toward the limit when the key already existed; overwrites kept growing the counter; a single oversized entry could never fit and would loop forever trying to flush; and counting only payload bytes underestimated real memory. Resolved with `ApproximateSize()`, subtract-on-overwrite, a per-entry overhead estimate, and rejection of oversized entries up front.

**WAL and SSTable numbering (12d).** The first version tied SSTable numbers to WAL numbers. Walking through concrete crash windows showed they fail independently, so the counters were separated, and `Open` was changed to replay every WAL.

**A data race that looked harmless (12e).** `live_wals_` was appended on the main thread while the background task erased from it. The disagreement was that each side touched "different elements". It was settled with a small ThreadSanitizer program: TSan flagged it, and uninstrumented runs crashed with heap corruption and gave wrong sizes. The fix is the mutex described above.

**Deleting WALs when a flush might fail (12e).** Clearing the WAL list on the main thread at submission time loses track of WALs whose SSTable write has not succeeded. The resolution was to separate *tracking* from *deleting*: a WAL file is removed only after its SSTable is durable, and any failure halts the database so that no later task can delete an older WAL.

**A silent failure path (12e).** A failed `SstableWriter::Open` inside the task originally returned without halting. The next flush would then succeed and delete the WAL holding the unwritten data. Every failure path in the task now calls `PanicDatabase`.

**Testing code whose work happens elsewhere (12e).** After a flush call returns, the SSTable may not exist yet. Tests therefore wait in one of two ways:

- **Close and reopen** the database. The destructor joins the worker after it drains the queue, so afterwards the files are in their final state. Best for file-layout and recovery checks.
- **Poll** until a condition holds (an SSTable appears, or `Get` starts failing with something other than `NotFound`, which signals the halt). Best for failure tests, where the database must stay open and must not be reopened early.

Failure injection uses no mocks. Creating a non-empty directory with the name of the target file (for example `sst-000001.sst.tmp` or `wal-000002.log`) makes `open()` fail with `EISDIR` and survives both `unlink` and `std::filesystem::remove`.

## Concepts covered along the way

- `.h` declares, `.cc` defines; the linker matches symbols by name, and CMake decides which sources form a library
- One Definition Rule, `inline`, and why non-inline function bodies in headers cause duplicate-symbol errors
- Pure virtual functions, abstract classes, virtual destructors, dependency injection by constructor
- RAII, file descriptors as kernel-held tickets, private constructors with static factories (`Open`)
- `errno` and `strerror`, partial `write()` loops, `EINTR`
- Endianness-independent encoding, sign extension when widening `char`
- Page cache vs. drive cache, `fsync` vs. `F_FULLFSYNC`
- Atomic rename and why the directory itself must be fsynced
- `std::thread`, `std::mutex`, `std::lock_guard`, `std::condition_variable` and predicates (spurious and lost wakeups), `std::atomic`, `std::async`/`std::future`
- The worker-thread pattern: queue plus condition variable, shutdown flag, join in the destructor, `std::thread` declared last
- Data races on containers, and ThreadSanitizer
- Fail-stop vs. fail-soft error handling

## Known gaps in the async flush (open as of this writing)

Tick these off as they are done.

- [ ] **The worker does not stop when the database halts.** A task already queued behind the failed one still runs, and a later success calls `ClearWal`, which deletes older WALs, including the failed flush's WAL. Fix: have the worker return when `is_halted_` is set (check it in the wait predicate and after waking). This is the highest-priority item.
- [ ] **No backpressure.** If the disk is slow, the main thread keeps rotating MemTables and queueing tasks, and memory grows. Bound the number of pending flushes and make the writer wait on a condition variable.
- [ ] `RotateWal()`'s result is ignored in `FlushMemTableToSstable`. It is safe today (the old WAL keeps taking writes and `ClearWal` leaves it alone), but the failure is invisible to the caller.
- [ ] The halted error is generic. Return the stored cause (`bg_error_status_`, read under `status_mutex_`) from `CheckDatabaseState`.
- [ ] The SSTable number is read when the task runs, not when it is queued. It is safe only because a single worker is the sole user of `next_sst_number_`.
- [ ] `RotateWal` and `ClearWal` should be private; `ClearWal` can return `void`.
- [ ] In `Open`, `recovery_dropped_bytes_` is assigned (`=`) and should accumulate (`+=`); `sync_` needs a default value; the file needs `#include <algorithm>` and the header `<vector>`/`<cstdint>`; file-private helpers (`SstPath`, `WalPath`, `ParseNumbered`, ...) belong in an anonymous namespace.
- [ ] Confirm the per-entry overhead (`kEntryOverhead`) is used in the size trigger, not just raw key + value bytes.
- [ ] Adapt the synchronous flush tests to the async behavior (helpers for waiting, halt instead of retry) and add: a failed flush halts and keeps all acknowledged data; reopen recovers; a queued second flush never deletes the first WAL (stress version with random timing); a multi-flush stress run under ThreadSanitizer.
- [ ] A test that destroys the database while many flushes are still queued and checks that nothing is lost after a reopen.

## Next steps

1. Finish Brick 12e: the items above, then the async test suite.
2. **Brick 13:** blocks, a sparse index, `pread`-based reads, a block cache, and Bloom filters. Today an `SstableView` holds a whole file in memory, which is a stand-in. A Bloom filter (bit array plus k hashes, about 10 bits per key for roughly 1% false positives) must include tombstone keys, helps point lookups only, and slots in before the per-file search.
3. MANIFEST: persist the file list and counters, so recovery stops depending on directory scans and filenames.
4. Compaction (size-tiered first, then leveled); tombstones are dropped only when safe.
5. A WAL-size flush trigger in addition to the MemTable-size trigger.
6. Skip-list MemTable behind the same interface, benchmarked against `std::map`.
7. Protobuf layer with schema ID/version tags and a compatibility test matrix (including compaction round trips preserving unknown fields).
8. Fuzzing (libFuzzer), crash injection, and benchmarks against RocksDB.

## Small open items from earlier bricks

- Add `Status stop_reason` to `WalReadResult` so recovery can distinguish a torn tail from a checksum mismatch.
- Fix `TornTailIsDroppedAndNewWritesStillWork` to expect the surviving fragment size (12 bytes), or compute it from the encoded record size.
- Tighten corruption tests to assert on the specific error message, so each test proves which check fired.
- Known gap: a failed or partial WAL `Append` can leave a partial record before later successful appends; real engines mark the WAL broken after a write error.
- Default `sync` to `true` for a real database; tests pass `false` explicitly.
- `fsync` the directory after creating a new WAL file.
- Share one `ReadFileToString` helper between the SSTable reader and other readers.
