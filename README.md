# minilsm

A small embedded key-value store in C++17, built brick by brick as a learning project. It is an LSM-tree storage engine: writes go to a write-ahead log (WAL) and an in-memory table (MemTable), which is flushed to sorted, immutable files (SSTables) on disk. The final goal is to store **protobuf values with schema evolution**, so old data stays readable after schema changes.

Background: this project builds on experience upgrading Protocol Buffers (3.0.x to 3.19.x) in a large C++ storage codebase at AWS Aurora, and generalizes that serialization/compatibility work into a portfolio piece.

Every piece was built test-first, and the focus throughout is correctness under failure: crashes, torn writes, corrupt files, failed disks and thread races. About 200 tests, run under ThreadSanitizer.

## Contents

- [Status at a glance](#status-at-a-glance)
- [Build and test](#build-and-test)
- [Project layout](#project-layout)
- [Progress by brick](#progress-by-brick)
- [How it works](#how-it-works)
- [Design decisions worth remembering](#design-decisions-worth-remembering)
- [Challenges and how they were resolved](#challenges-and-how-they-were-resolved)
- [Concepts covered along the way](#concepts-covered-along-the-way)
- [Roadmap](#roadmap)
- [Known gaps and small open items](#known-gaps-and-small-open-items)

## Status at a glance

| Area | State |
|---|---|
| WAL (record format, writer, reader, recovery, truncation of a torn tail) | Done |
| MemTable with tombstones and byte-size accounting | Done |
| SSTable format, atomic writer, reader | Done |
| Synchronous flush, then asynchronous flush (background thread, halt on failure) | Done, ThreadSanitizer clean |
| Per-SSTable Bloom filters | Done |
| MANIFEST (authoritative list of live SSTables, file-number counter) | Done |
| Blocks, sparse index, `pread`, block cache | Deferred on purpose (see design decisions) |
| Compaction | Designed, not started (see roadmap) |
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

Run a single test:

```bash
ctest -V --test-dir build -R ManifestTest.RoundTripEmptyList
./build/manifest_test --gtest_filter='ManifestTest.RoundTripEmptyList'   # quote the filter in zsh
```

After adding a `.cc` file, add it to the library sources in `CMakeLists.txt` and re-run `cmake`. A "symbol(s) not found" link error for a new class almost always means this.

Because flushing runs on a background thread, run the suite under ThreadSanitizer before trusting concurrency changes:

```bash
cmake -S . -B build-tsan -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1"
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

Each build directory needs its own `cmake` and build step. A stale test binary can show old failures, so rebuild before debugging a failure that "should be fixed".

Platform: macOS on Apple Silicon (uses `F_FULLFSYNC`).

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
│   ├── coding.h              shared PutFixed32/64, GetFixed32/64
│   ├── wal_record.h
│   ├── wal_writer.h
│   ├── wal_reader.h
│   ├── sstable_format.h      SstEntry, BuildSstable, SstableView
│   ├── sstable_writer.h      atomic publish of an SSTable file
│   ├── sstable_reader.h      ReadSstableFile, ReadBloomFilterBlock
│   ├── bloomfilter.h         BloomFilter, BloomFilterView
│   └── manifest.h, manifest_writer.h, manifest_reader.h
├── src/                  implementations (.cc)
└── tests/                GoogleTest files, one per component
```

Conventions: Google C++ style (`.cc`/`.h`, `kCamelCase` enumerators, trailing `_` on private members, anonymous namespaces for file-private helpers). Each component is built test-first: write the test, watch it fail to link, then implement.

## Progress by brick

| Brick | What was built | Key idea |
|---|---|---|
| 1 | CMake, GoogleTest, smoke test | Prove the toolchain first |
| 2 | `Status`, abstract `DB` | `Get` returns a `Status`, so "not found" and I/O errors differ from values |
| 3 | MemTable (`std::map`) | Deletes are tombstones; `Get` has three outcomes: found / deleted / absent |
| 4 | `LSMDB` implementing `DB` | In-memory only at first |
| 5 to 7 | WAL record encode/decode, CRC-32 | `[crc][type][key_len][value_len][key][value]`; CRC covers everything after itself; verified against `0xCBF43926` for `"123456789"` |
| 8 | `WalWriter` | open/write/fsync; partial writes and `EINTR` handled; `F_FULLFSYNC` on macOS |
| 9 | `ReadWal` | Replay the valid prefix, stop at the first bad record; report `valid_bytes` and `clean` |
| 10 | `LSMDB::Open(dir)` recovery | WAL replay into the MemTable; truncate a damaged tail |
| 11 | SSTable format (in memory) | Sorted entries, full index, footer with CRC and magic |
| 12a | `SstableWriter` | Atomic publish: write `.tmp`, fsync, rename, fsync the directory |
| 12b | `ReadSstableFile` | POSIX read loop with offset tracking |
| 12c | `MemTable::Snapshot()`, size accounting | Sorted copy for flushing; subtract the old entry on overwrite |
| 12d | Synchronous flush | Numbered `sst-N.sst` and `wal-N.log`; size-triggered flush; `Get` falls through SSTables newest first |
| 12e | Asynchronous flush | One worker thread, FIFO queue, halt on failure, ThreadSanitizer clean |
| 13 (partial) | Bloom filters | Per-SSTable filter stored inside the SSTable |
| 14a | MANIFEST | Atomic list of live SSTables; recovery trusts it instead of directory names |

## How it works

### Files on disk

```
<dir>/
  MANIFEST              which SSTables are live, plus a file-number counter
  MANIFEST.tmp          in-flight MANIFEST write; deleted by Open
  sst-000001.sst        immutable sorted tables, one per flush
  sst-000003.sst.tmp    in-flight or abandoned write; deleted by Open
  wal-000007.log        write-ahead logs; the highest number takes writes
```

SSTables and WALs have **independent counters**. Names are parsed strictly (`<prefix><digits><suffix>`); anything else in the directory is ignored.

### Write path

```
Put/Delete
  1. reject if the database is halted
  2. reject if the single entry is larger than the memtable limit (InvalidArgument)
  3. if this entry would push the MemTable past the limit, flush first (return the error if it fails)
  4. reject if there is no WAL (IOError); never silently skip the WAL
  5. append to the WAL            (on failure, memory is untouched)
  6. apply to the MemTable
```

### Flush path (asynchronous)

```
main thread                                    background worker (single, FIFO)
-----------                                    --------------------------------
check state; snapshot the MemTable
build Bloom filter, serialize its payload
BuildSstable(entries, bloom_payload) -> bytes
parse bytes into an in-memory view
rotate the WAL first; on failure, halt and return
take SSTable number K, register the filter and
  view (reads see the new table at once)
clear the MemTable
enqueue task(bytes, K, keep_from)  ──────────► 1. write sst-K.sst atomically
return to the caller                            2. build a COPY of the manifest with K added,
                                                   write MANIFEST atomically
                                                3. only after success: adopt the copy
                                                4. drop the in-memory view
                                                5. delete WALs older than keep_from   <- LAST
                                                any failure: remove partial file, HALT
```

Rules that matter:

- A WAL may be deleted **only after the SSTable holding its data is durable and listed in the MANIFEST**. Until then the WAL is the only copy of that data.
- `ClearWal(keep_from)` must be given the WAL that is live after rotation, not the old one. If it is not found, return an error instead of erasing everything.
- The SSTable number is assigned on the submitting thread, not in the worker, so two queued flushes never share a number.

### Read path

`Get` checks the MemTable, then the in-memory SSTable views newest first, then the Bloom filters newest first for files that exist only on disk. A filter that answers "definitely not" skips the file; on "maybe" the file is read (whole file, about 4 MiB). A tombstone stops the search; "not present" continues to the next older file. A key missing everywhere costs one filter probe per file, plus one wasted read per false positive.

### Recovery (`Open`)

1. Scan the directory; delete `*.sst.tmp` and `MANIFEST.tmp`.
2. Read the MANIFEST:
   - **Missing:** a fresh database, or one from before the MANIFEST existed. Adopt the `.sst` files found on disk as the live list. Never delete anything in this case.
   - **Corrupt, empty or unreadable:** `Open` fails. Without a valid MANIFEST the live set is unknown, so we do not guess.
   - **Valid:** it is authoritative. Every listed file must exist (else `Corruption`), and `.sst` files that are not listed are orphans and are deleted.
3. Load each listed SSTable's Bloom filter, oldest first.
4. `next_sst_number = max(manifest counter, highest .sst on disk) + 1`, so an orphan left by a crash can never share a number with a new file.
5. Replay **every** WAL in ascending order into the MemTable. Replaying a WAL whose data is already in an SSTable is harmless, because newer entries shadow older ones.
6. Truncate a damaged tail, but only on the last WAL.
7. Append to the highest WAL (or create `wal-000001.log`).

Crash windows during a flush, and why each is safe:

| Crash happens | State left behind | Result on restart |
|---|---|---|
| Before the SSTable is renamed | `.sst.tmp` and the old WAL(s) | Temp file deleted, WAL replayed, nothing lost |
| After the SSTable rename, before the MANIFEST write | unlisted `.sst`, old WAL | `.sst` is an orphan and is deleted; WAL replays; nothing lost |
| After the MANIFEST write, before the old WAL is deleted | listed `.sst`, old WAL | WAL replays on top of the SSTable; duplicates are harmless |
| After the new WAL is created, before the old one is deleted | Two WALs | Both replayed in ascending order |

### SSTable layout

```
[entries][index][bloom block][footer]
entry:       [key_len 4][value_len 4][type 1][key][value]
index:       per entry: [key_len 4][key][offset 8]
bloom block: [bloom payload][bloom_offset 8][crc 4]        crc covers payload + offset
footer:      [index_offset 8][entry_count 8][crc 4][magic 8]
```

- Written in one pass, never modified, so the index and footer come after the data.
- Entries are variable length, so the index (keys + offsets) is what makes binary search possible.
- The footer is fixed size at the end of the file, so a reader can find it and learn where the variable-size parts start.
- The magic number (`"MINILSM1"`) identifies the file type, versions the format, and cheaply detects truncation.
- Input must be sorted and duplicate-free; `BuildSstable` rejects anything else rather than silently choosing a winner.

### Bloom filters

Standard Bloom filter, one per SSTable, stored inside the file. Sizing from the key count `n` and target false-positive rate `p`: `m = -n ln p / (ln 2)^2` bits and `k = (m/n) ln 2` hashes. Probes use double hashing: probe `i` is `(h1 + i*h2) % num_bits`. For 1000 keys at 1% that is 9592 bits (1199 bytes) and 7 hashes, covered by a test.

- Filters include tombstone keys, and help point lookups only.
- Filters are immutable. A compaction builds a new filter for its output; a filter is never edited to "remove" a key.
- Payload: `[count 8][num_bits 8][num_hashes 8][bits]`. The payload has no checksum of its own; the SSTable block CRC covers it. Corruption is detected lazily at `Get`.

### MANIFEST

The authoritative list of live SSTables. It makes file-set changes atomic: a crash leaves either the old list or the new one.

```
[crc 4][counter 8][sstable number 8] * N        crc covers the counter and all numbers
```

- The whole file is rewritten on every update (write `MANIFEST.tmp`, fsync, rename, fsync the directory). The writer is one-shot: a second `WriteManifest` on the same writer fails, because a still-open fd would append into the published file.
- The decoder rejects anything under 12 bytes (an empty file is `Corruption`, never "an empty database"), any length that is not a multiple of 8 after the header, and any CRC mismatch.
- The counter lives in the MANIFEST so numbers are not reused after files are deleted.
- A MANIFEST write failure halts the database like any other disk failure.

## Design decisions worth remembering

**Tombstones.** SSTables are immutable, so a delete must be written as data. Newer entries shadow older ones. Without tombstones, a deleted key would reappear from an older file.

**Interface vs. concrete class.** `DB` is abstract (`= 0`) so we can later add a `FakeDB` (plain `std::map`) for differential testing and for testing the protobuf layer without disk.

**Struct vs. class.** Structs for plain data with no invariants (`WalRecord`, `WalReadResult`, `SstEntry`); classes for types that own resources or enforce rules (`Status`, `MemTable`, `WalWriter`, `LSMDB`).

**Status vs. void vs. enum.** `void` where nothing can fail (`MemTable::Put`), an enum where there are several normal outcomes (`GetResult`), `Status` where I/O or corruption is possible.

**Check, then read.** Decoders never trust lengths from disk. Bounds checks are written to avoid integer overflow (`value_len > remaining - key_len`), and checksums are verified before interpreting contents.

**Checksums detect, they don't repair.** There is no replica, so recovery policy is: stop at the first bad record, keep the good prefix (a state the database really was in), and report how many bytes were discarded. We never skip ahead past a bad record, because its length fields can't be trusted and replaying later records could produce a state that never existed.

**`ReadWal` returns the good records.** Corruption is reported in the result (`clean`, `valid_bytes`), not as a failing `Status`. `Status` means "the read operation itself failed" (missing file, I/O error). Policy (log, truncate, refuse to open) lives in `LSMDB::Open`.

**Write path order.** WAL append first, then MemTable. If the append fails, memory is untouched.

**Truncate on recovery.** If the WAL tail is damaged, `Open` truncates the file at `valid_bytes` before appending. Otherwise new records would sit after garbage and be lost on the next restart. Truncation uses `std::filesystem::resize_file` and is idempotent.

**Durability levels.**

| Failure | `sync = false` | `sync = true` |
|---|---|---|
| Our process crashes | Safe (kernel still holds the data) | Safe |
| OS crash or power loss | Recent writes can be lost | Safe |

`sync = true` costs real latency (an `fsync` per write), which is why it is an option. Group commit and timed sync are later topics.

**Atomic publish.** An SSTable (and the MANIFEST) is written to `path.tmp`, fsynced, closed, then renamed onto its final name, and the parent directory is fsynced. A reader or a crash therefore sees either no file or a complete one. `rename` silently overwrites its target, so **file numbers must never be reused**.

**Independent counters for SSTables and WALs.** Early on the plan was to derive `sst-N` from `wal-N`. That couples two things that fail separately: a WAL can be created without its SSTable ever being written, and the reverse. With independent counters, recovery does not depend on the two lining up. `Open` replays every WAL it finds, so a stale WAL left by a crash is harmless.

**WAL cleanup belongs to flush, not compaction.** A WAL covers one MemTable. Once that MemTable's SSTable is durable and listed, the WAL is redundant.

**WAL rotation order.** Create the new WAL first, then swap the writer, and only then delete old WALs. If the new WAL cannot be created, the database halts and a restart replays the old WAL in full.

**Memory cap.** The MemTable has a hard size limit (`kmaximum_bytes_`, 4 MiB). A single entry larger than the limit is rejected with `InvalidArgument("entry too large")` before it reaches the WAL. The MemTable estimates the real memory of an entry as key + value + a per-entry overhead (roughly 100 bytes), because small keys cost far more memory than their payload. Overwriting a key subtracts the old entry first.

**Fail-stop on any disk failure.** If a background flush fails (including the MANIFEST write, or opening the next WAL during rotation), the database halts: `Put`, `Delete`, `Get` and `Flush` return an `IOError`, the worker stops taking tasks, and a restart recovers from the WALs. The reasons:

- Once a flush has failed, its WAL is the only copy of that data and must not be deleted.
- Carrying on would let memory grow without bound, because nothing can be flushed.
- If a later flush succeeded, it would delete older WALs, including the failed flush's WAL, and a restart could replay an older WAL over a newer SSTable. Halting removes both hazards.

**One background thread.** A single worker with a FIFO queue keeps flushes in order, so SSTables become durable oldest to newest and tasks never race each other.

**Locking rules for the async path.**

- The main thread owns the containers and counters; the worker writes only its captured data and takes a lock for any shared state it touches.
- `sstable_mutex` guards `sstables_`, `bloomfilters_`, `next_sst_number_` and the live manifest (every access, including `Get`).
- `wal_mutex_` guards `live_wals_`. `PanicDatabase` must not take it (deadlock).
- `queue_mutex_` / `queue_cv_` guard the task queue and the shutdown flag. `is_halted_` is atomic; the error status has its own mutex.
- The destructor sets shutdown, wakes the worker and joins it. The worker drains the queue first, so a clean close completes every accepted flush.

**Reads load whole files for now.** A sparse index, blocks with per-block checksums, and a block cache are deferred. At about 4 MiB per file, loading a few whole files is acceptable for this project, and a sparse index changes block creation, encoding and checksums.

**Known portability gap.** The Bloom filter hash uses `std::hash`, which is not stable across platforms or standard libraries. Replace it with a stable hash (and add a golden-bytes test) before filters are persisted across platforms.

## Challenges and how they were resolved

**Atomic file publish had many small traps (12a).** Local `path_` shadowing; the `rename` skipped on macOS because `F_FULLFSYNC` returned early; rename result unchecked; `O_APPEND` appending to a stale `.tmp` (fixed by unlinking it first); no directory fsync; the fd left open on success, so a second call appended into the published file; and a `Close()` cleanup that would have deleted the finished file. All fixed, with a 17-test suite. The same family reappeared in `ManifestWriter`.

**Reading a file correctly (12b).** `read(fd, buf, sizeof(st.st_size))` read 8 bytes into the same place instead of looping with an offset. `ifstream` error reporting is unreliable, so the reader uses a POSIX read loop. `Parse` takes its input by value and moves it (sink-parameter idiom).

**Size accounting was wrong in several ways (12d).** A delete only counted toward the limit when the key already existed; overwrites kept growing the counter; a single oversized entry could never fit and would loop forever trying to flush; and counting only payload bytes underestimated real memory. Resolved with `ApproximateSize()`, subtract-on-overwrite, a per-entry overhead estimate, and rejection of oversized entries up front.

**WAL and SSTable numbering (12d).** The first version tied SSTable numbers to WAL numbers. Walking through concrete crash windows showed they fail independently, so the counters were separated and `Open` replays every WAL.

**A data race that looked harmless (12e).** `live_wals_` was appended on the main thread while the background task erased from it. The disagreement was that each side touched "different elements". It was settled with a small ThreadSanitizer program: TSan flagged it, and uninstrumented runs crashed with heap corruption. The fix is `wal_mutex_`. ThreadSanitizer then found two more in the real code: the worker incremented `next_sst_number_`, and it popped from `sstables_` while the main thread pushed and read. Both fixed by assigning the number on the submitting thread and locking every access.

**The wrong file number (12e).** Reading `next_sst_number_` after the increment put N+1 in the file name and N in the filter. Two queued flushes also shared a number because it was assigned in the worker. Both fixed by capturing the number at submit time.

**Deleting WALs when a flush might fail (12e).** Clearing the WAL list at submission time loses track of WALs whose SSTable write has not succeeded. The resolution was to separate *tracking* from *deleting*: a WAL file is removed only after its SSTable is durable, and any failure halts the database so that no later task can delete an older WAL.

**A silent failure path (12e).** A failed `SstableWriter::Open` inside the task originally returned without halting. The next flush would then succeed and delete the WAL holding the unwritten data. Every failure path now calls `PanicDatabase`.

**`ClearWal` never deleted the old WAL (12e).** It deletes WALs before its argument, but the flush passed the old WAL. Fix: pass the WAL that is live after rotation, and return an error if it is not found (otherwise `erase(begin, end())` removes everything).

**Rotation failure was ignored (12e).** The flush ignored `RotateWal`'s status and returned OK after a halt. It now checks it and returns. Related: `Put` once skipped the WAL when `wal_` was null, silently losing durability. It is now an error.

**Bloom filter bugs (13).** Serializing after `std::move` wrote an empty filter (24 bytes of header, no bits), so serialize first. A footer/CRC check inside `deserialize` rejected valid payloads because `BuildSstable` already wraps the payload with an offset and CRC, so `deserialize` is payload-only. A bit-flip test inside the payload cannot pass without a payload CRC, so that test lives at the SSTable level.

**Flaky stress test (12e).** It first looked like a race. TSan found two (above). The remaining "intermittent failure" was a stale binary; after a rebuild it passed 50 of 50 runs, and under TSan.

**Tests that contradicted the design (12e).** Tests expected writes to continue after a failed rotation, or an SSTable to appear after a halt. The design halts, so the tests were changed (flush returns an error, the next write fails, reopen recovers from the old WAL). The source was not bent to the tests.

**Testing code whose work happens elsewhere (12e).** After a flush call returns, the SSTable may not exist yet. Tests therefore wait in one of two ways:

- **Close and reopen** the database. The destructor joins the worker after it drains the queue, so afterwards the files are in their final state. Best for file-layout and recovery checks.
- **Poll** until a condition holds (an SSTable appears, or `Get` starts failing with something other than `NotFound`, which signals the halt). Best for failure tests, where the database must stay open.

Failure injection uses no mocks. Creating a directory with the name of the target file (`wal-000002.log`, `sst-000001.sst.tmp`, `MANIFEST.tmp`) makes `open()` fail with `EISDIR`.

**MANIFEST review findings (14a).** The reviewed first version had several bugs, most caught by review or by the tests written for it:

- `record = new Manifest()` reassigned a local pointer and leaked (use `*record = Manifest()`).
- The decoder read fields before checking the size, read the CRC with `GetFixed64` instead of `GetFixed32`, and an empty file decoded as OK (which would have deleted every SSTable as an orphan).
- The flush task passed the SSTable path to the manifest writer, which would have overwritten the SSTable.
- The worker never stored the updated manifest, so every MANIFEST listed one file, and `Open` never stored it either.
- `ClearWal` ran before the MANIFEST write, so a crash in between lost data.
- **A missing MANIFEST deleted every SSTable.** In `Open`, the result of reading the MANIFEST was overwritten by the directory scan before it was checked, so a missing MANIFEST took the "delete unlisted files" branch with an empty live list. The test for bootstrapping without a MANIFEST caught it. Orphan cleanup now runs only when a MANIFEST was actually read.

**Designing compaction safely (14, on paper).** Giving a compaction output the number of one of its inputs is unsafe: the atomic rename would overwrite that input before the MANIFEST swap, and a crash at that point could let deleted keys reappear. The output takes a fresh number instead. A related finding: once compaction exists, a file's number no longer tells how old it is, so age has to come from its position in the MANIFEST list. This was worked out and written down before any code.

## Concepts covered along the way

- `.h` declares, `.cc` defines; the linker matches symbols by name, and CMake decides which sources form a library
- One Definition Rule, `inline`, and why non-inline function bodies in headers cause duplicate-symbol errors
- Pure virtual functions, abstract classes, virtual destructors, dependency injection by constructor
- RAII, file descriptors as kernel-held tickets, private constructors with static factories (`Open`)
- `errno` and `strerror`, partial `write()` loops, `EINTR`
- Endianness-independent encoding, sign extension when widening `char`
- Page cache vs. drive cache, `fsync` vs. `F_FULLFSYNC`
- Atomic rename and why the directory itself must be fsynced
- `std::thread`, `std::mutex`, `std::lock_guard`, `std::condition_variable` and predicates (spurious and lost wakeups), `std::atomic`
- The worker-thread pattern: queue plus condition variable, shutdown flag, join in the destructor
- Data races on containers, and ThreadSanitizer
- Fail-stop vs. fail-soft error handling
- Bloom filter sizing and double hashing
- Write-then-rename metadata (MANIFEST) and orphan cleanup

## Roadmap

### Compaction (next)

Every flush adds a file. Without compaction, overwritten values and deleted keys are never reclaimed, and a read for a missing key costs one filter probe per file. Compaction merges SSTables so there are fewer files, duplicates are removed, and tombstones are eventually dropped.

Key points of the design:

- Merge by key; the newest version wins. Tombstones are kept in a partial compaction and dropped only in a full one, because a surviving older file could otherwise bring a deleted key back.
- The output gets a fresh Bloom filter and a never-used file number, and is published through one atomic MANIFEST rewrite that removes the inputs and adds the output.
- Input files are deleted only after the MANIFEST swap, so a crash leaves orphans that `Open` cleans up.
- Reads see the old file list until the swap and the new list after it, in one critical section.

| Brick | Content | Tests |
|---|---|---|
| 14b | Pure merge function, no I/O | Newest wins; tombstones kept vs. dropped; sorted output; empty and disjoint inputs |
| 14c | Offline full compaction through the MANIFEST | Differential test against a `std::map`; crash windows by fault injection; orphan cleanup; numbers never reused |
| 14d | Background compaction on the worker, swap under the lock | ThreadSanitizer stress: concurrent `Put`/`Get` during compaction; halt during compaction |
| 14e | Size-tiered trigger and partial compaction | Tombstones kept in partial runs; only the oldest contiguous run is merged |

### After compaction

- Backpressure: bound the number of pending flushes and make writers wait
- Sparse index, blocks with per-block checksums, block cache
- WAL-size flush trigger in addition to the MemTable-size trigger
- Skip-list MemTable behind the same interface, benchmarked against `std::map`
- Protobuf layer with schema ID/version tags and a compatibility matrix (including compaction round trips that preserve unknown fields)
- Fuzzing (libFuzzer), crash injection, and benchmarks against RocksDB

## Known gaps and small open items

Kept visible on purpose.

- A failed or partial WAL `Append` can leave a partial record before later appends; real engines mark the WAL broken after a write error.
- The halted error returned to callers is generic; return the stored cause from `CheckDatabaseState`.
- Default `sync` to `true` for a real database; tests pass `false` explicitly.
- `fsync` the directory after creating a new WAL file.
- Add `Status stop_reason` to `WalReadResult` so recovery can tell a torn tail from a checksum mismatch.
- Tighten corruption tests to assert on the specific error message, so each test proves which check fired.
- Make `RotateWal` and `ClearWal` private.
- Share one `ReadFileToString` helper between the readers.
- After a reopen, in-memory SSTable views are not rebuilt, so `Get` uses only the Bloom filter and whole-file path.
- Replace `std::hash` in the Bloom filter with a stable hash before filters move across platforms.