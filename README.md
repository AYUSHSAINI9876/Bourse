# Bourse

**A from-scratch trading exchange with its own storage engine, cache, and query layer.** Modern C++20, zero third-party runtime dependencies, a hand-written `epoll` reactor, and wire compatibility with `redis-cli`.

<p>
  <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white">
  <img alt="CMake" src="https://img.shields.io/badge/build-CMake%20%2B%20Ninja-064F8C?logo=cmake&logoColor=white">
  <img alt="Tests" src="https://img.shields.io/badge/tests-228%20passing-2ea043">
  <img alt="Sanitizers" src="https://img.shields.io/badge/ASan%20%C2%B7%20UBSan%20%C2%B7%20TSan-clean-2ea043">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Linux%20%C2%B7%20WSL%20%C2%B7%20Docker-333">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-blue">
</p>

---

## What this is

A real exchange is not one system, it is five stacked on top of each other: a low-latency matching core, a durable journal, an in-memory state store, a query layer for historical data, and multi-protocol client access. Bourse builds that whole stack from scratch in one coherent codebase — no Boost, no libuv, no hiredis, no SQLite.

All five layers are built and tested. You can point `redis-cli` at it, `curl` at it, or open the dashboard in a browser, and all three reach the same command registry.

<p align="center">
  <img src="docs/img/architecture.svg" alt="Bourse layered architecture: core, storage, cache, exec, match, sql, net, dashboard" width="100%">
</p>

---

## The 60-second demo

```bash
bash scripts/build.sh
bash scripts/demo.sh                 # guided tour of every layer, then leaves the server up
```

Or start it yourself:

```bash
./build/bin/bourse-server            # RESP on 6380, HTTP + dashboard on 8080
```

**It is a Redis server.** Stock `redis-cli`, nothing custom:

```console
$ redis-cli -p 6380
127.0.0.1:6380> SET user:1 ayush EX 300
OK
127.0.0.1:6380> INCRBY pageviews 99
(integer) 99
127.0.0.1:6380> RPUSH queue a b c
(integer) 3
127.0.0.1:6380> GET queue
(error) WRONGTYPE Operation against a key holding the wrong kind of value
```

**It is an exchange.** Price-time priority, real order types:

```console
127.0.0.1:6380> ORDER AAPL SELL LIMIT 10 100.50
1) "order_id"   2) (integer) 1
3) "status"     4) "NEW"
127.0.0.1:6380> ORDER AAPL BUY LIMIT 4 101.00
3) "status"     4) "FILLED"
11) "trades"    12) 1) 1) "sequence"  2) (integer) 1
                      3) "price"     4) "100.50"    <-- resting price, not 101
127.0.0.1:6380> BOOK AAPL
```

**It is a database.** Real lexer, parser, AST, and query plan:

```console
127.0.0.1:6380> SQL CREATE TABLE fills (id INTEGER PRIMARY KEY, sym TEXT, qty INTEGER)
OK
127.0.0.1:6380> SQL INSERT INTO fills VALUES (1, 'AAPL', 100), (2, 'MSFT', 50)
127.0.0.1:6380> SQL SELECT sym, qty * 2 AS doubled FROM fills WHERE qty > 60 ORDER BY qty DESC
127.0.0.1:6380> EXPLAIN SELECT * FROM fills WHERE qty > 60 LIMIT 1
"Limit(limit=1, offset=0)
   Filter((qty > 60), examined=1, passed=1)
     SeqScan(fills, rows=2)"
```

**It has an on-disk index.** A 4 KiB pager with a free list, a pinning buffer pool, and a B+ tree with node splits and range scans — 10,000 keys in a tree four levels deep, verified structurally after every mutation batch:

```cpp
BPlusTree tree(pool, disk);
tree.insert("AAPL:20260730:0930", record_id);
tree.scan("AAPL:20260730:0900", "AAPL:20260730:1000", visit);   // one descent, then a leaf walk
tree.validate();                                                 // sorted, balanced, correctly threaded
```

**It survives a crash.** `--appendonly yes` turns on the WAL:

```bash
./build/bin/bourse-server --appendonly yes --dir ./data &
redis-cli -p 6380 SET survivor "still here"
kill -9 %1                                   # no clean shutdown, no flush
./build/bin/bourse-server --appendonly yes --dir ./data &
redis-cli -p 6380 GET survivor               # "still here"
```

**It has a live dashboard.** Open <http://localhost:8080> — depth ladder, trade tape, latency histogram, command console, keyspace browser. One self-contained HTML page compiled into the binary.

---

## Status

| Layer | Component | State |
|---|---|:--|
| **L0** `core/` | RAII `File`/`Socket`, `ByteBuffer`, bounded `ThreadPool`, lock-free `SpscRing`, `ObjectPool`, `Arena`, async `Logger`, HDR-style `Histogram` | ✅ |
| **L1** `storage/` | `WriteAheadLog` with CRC-32 framing, torn-tail recovery, 3 fsync policies; atomic checksummed `Snapshot`; `DiskManager` + pinning `BufferPool` + on-disk `BPlusTree` | ✅ |
| **L2** `cache/` | 16-shard `Keyspace`, `Value` variant, lazy + active TTL expiry, sampled LRU/LFU/Random/NoEviction | ✅ |
| **L3a** `exec/` | `Command` interface, dispatch registry, 57 verbs, protocol-independent `Reply`, `PubSub`, journal hook | ✅ |
| **L3b** `sql/` | Lexer → recursive-descent parser → AST → **Visitor** → volcano iterators | ✅ |
| **L4** `match/` | Order book, price-time priority, LIMIT/MARKET/IOC/FOK, amend, Observer market data | ✅ |
| **L5** `net/` | `Poller` (epoll + poll), `EventLoop`, acceptor + N reactors, `RespCodec`, `HttpCodec`, `Router`, middleware | ✅ |
| **L6** `dashboard/` | Depth ladder, trade tape, latency bars, command console, keyspace browser | ✅ |
| — | Wiring the B+ tree in behind the SQL row store (the tree is built and tested; the executor still reads from memory) | 🚧 |
| — | WebSocket streaming (the dashboard polls once a second instead) | 🚧 |

---

## Quick start

### Prerequisites

Linux, or Windows with WSL2. One command provisions everything:

```bash
wsl -d Ubuntu --user root -- bash scripts/setup-wsl.sh    # from Windows
sudo bash scripts/setup-wsl.sh                            # on native Linux
```

Installs `g++`, `cmake`, `ninja`, `libgtest-dev`, `redis-tools` and `curl`.

### Opening it in an editor

The toolchain lives in WSL, so the folder must be opened **in WSL mode** — not
as a plain Windows folder. Otherwise the C++ extension finds no compiler and
every include shows a red squiggle.

```bash
code --remote wsl+Ubuntu /mnt/c/Users/ayush/Github/Bourse
```

Or from an already-open VS Code window: `Ctrl+Shift+P` → **WSL: Reopen Folder in WSL**.

The bottom-left corner should read `WSL: Ubuntu`. Once it does, `.vscode/` wires
up the rest:

| Shortcut | What it does |
|---|---|
| `Ctrl+Shift+B` | build |
| `Ctrl+Shift+P` → *Run Task* | test suite, smoke suites, sanitizers, benchmark, demo |
| `F5` | debug the tests or the server under gdb, with breakpoints |

IntelliSense reads `build/compile_commands.json`, so it uses the real compiler
flags rather than guessing — run a build once and go-to-definition works
across the whole tree.

### Build, run, test

```bash
bash scripts/build.sh                    # RelWithDebInfo + tests
./build/bin/bourse-server                # start it
ctest --test-dir build --output-on-failure
```

```
2026-07-30 09:14:02.118 [INFO ] command registry initialised with 57 verbs
2026-07-30 09:14:02.119 [INFO ] bourse-resp listening on 0.0.0.0:6380 (poller=epoll, io_threads=4)
2026-07-30 09:14:02.119 [INFO ] RESP endpoint ready on port 6380 -- try: redis-cli -p 6380 PING
2026-07-30 09:14:02.121 [INFO ] HTTP endpoint ready on port 8080 -- dashboard at http://localhost:8080/  (16 routes)
```

### Docker

```bash
docker compose up --build -d
redis-cli -p 6380 PING          # PONG
```

Two-stage build: no compiler in the runtime image, non-root user, ~80 MB.

### Options

```
--host <addr>              bind address                     (default 0.0.0.0)
--port <n>                 RESP port                        (default 6380)
--http-port <n>            HTTP/dashboard port              (default 8080)
--no-http                  disable the HTTP listener
--io-threads <n>           I/O event loops, 0 = auto        (default 0)
--shards <n>               keyspace shards, power of two    (default 16)
--maxmemory <size>         eviction budget, e.g. 256mb      (default unlimited)
--maxmemory-policy <name>  allkeys-lru | allkeys-lfu |
                           allkeys-random | noeviction      (default allkeys-lru)
--maxmemory-samples <n>    eviction sample size             (default 5)
--dir <path>               data directory                   (default ./bourse-data)
--appendonly <yes|no>      enable WAL + snapshot recovery   (default no)
--wal-sync <policy>        never | everysec | always        (default everysec)
--save-seconds <n>         snapshot interval, 0 disables    (default 300)
--log-level <level>        trace|debug|info|warn|error      (default info)
```

---

## Testing — how to verify everything, fast

Four independent layers of verification. One command runs them all:

```bash
bash scripts/verify-all.sh
```

Or individually:

| What | Command | Result |
|---|---|---|
| Unit + integration | `./build/bin/bourse_tests` | **228 tests, 41 suites** |
| KV over real `redis-cli` | `bash scripts/smoke-test.sh` | **68 assertions** |
| HTTP + exchange | `bash scripts/smoke-exchange.sh` | **42 assertions** |
| Crash recovery | `bash scripts/smoke-persistence.sh` | **27 assertions** |
| ASan + UBSan + TSan | `bash scripts/check-sanitizers.sh` | **clean** |
| Throughput + latency | `bash scripts/benchmark.sh` | see below |

The tests are not decorative. A representative sample of what they pin down:

- **`RespParser.ReportsIncompleteForEveryProperPrefix`** and its HTTP twin — feed the parser *every* prefix of a valid request and require that it never claims success and never consumes a byte until the frame is whole. The classic "assumed one `read()` = one request" bug, tested exhaustively.
- **`ServerFixture.HandlesRequestsSplitAcrossPackets`** — the same property over a real TCP socket, one byte at a time.
- **`OrderBookTest.FillOrKillIsAllOrNothing`** — a FOK that cannot fill must leave the resting book *byte-for-byte untouched*, having emitted no trades.
- **`OrderBookTest.RepricingLosesTimePriority`** — an amended order must go to the back of a queue it never waited in.
- **`OrderBookTest.SteadyStateOrderEntryStopsAllocating`** — asserts the pool's chunk count stops moving after warm-up, which is the actual property the pool exists for.
- **`WriteAheadLog.TruncatesATornTailAndKeepsGoing`** — half a record from a crash is discarded, everything before it survives, and the log is writable again.
- **`SnapshotTest.RefusesToLoadADamagedImage`** — one flipped byte and recovery *refuses to start* rather than silently serving wrong data.
- **`SqlFixture.LimitStopsPullingRows`** — asserts via the plan's own counters that `LIMIT 5` over 500 rows examines exactly 5.
- **`BTreeFixture.InterleavedInsertAndEraseStayConsistent`** — 3000 randomised inserts and deletes, then a full structural `validate()`: sorted keys in every node, separators consistent with subtree contents, all leaves at equal depth, leaf chain correctly threaded. A subtly wrong B+ tree still answers most lookups correctly, so spot checks are not enough.
- **`BufferPoolTest.RefusesToEvictPinnedPages`** — the pool reports exhaustion rather than pulling a page out from under a caller holding it.
- **`SqlFixture.UpdateEvaluatesAgainstThePreUpdateRow`** — `SET a = b, b = a` must swap, not duplicate.
- **`Keyspace.ConcurrentIncrementsLoseNothing`** — 8 threads × 2000 increments must produce exactly the arithmetic total.
- **`RouterTest.MiddlewareCanShortCircuit`** — a middleware that does not call `next()` must stop the chain before the handler.

---

## Benchmarks

**Machine:** 4-core WSL2 VM, GCC 15.2.0, `RelWithDebInfo`, 200k requests, 50 clients, stock `redis-benchmark`.

| Workload | Throughput | Client p50 |
|---|--:|--:|
| `SET`, no pipelining | 42,992 ops/s | 0.535 ms |
| `GET`, no pipelining | 43,592 ops/s | 0.535 ms |
| `SET`, pipeline depth 16 | **501,253 ops/s** | 0.623 ms |
| `GET`, pipeline depth 16 | **598,802 ops/s** | 0.647 ms |

Server-side, from the built-in HDR histogram over 1,000,005 commands:

```
command_latency_p50_ns:576
command_latency_p99_ns:2304
```

**p50 576 ns, p99 2.3 µs** for the full path: parse → registry lookup → arity check → shard lock → hash probe → mutate → encode.

The unpipelined figures are **client-bound, not server-bound** — in sequential mode each client waits for a reply before sending again, so 43k ops/s measures loopback round-trip, which is exactly why the server's own histogram reports sub-microsecond work for the same commands. The 14× jump under pipelining is that round-trip being amortised away. Full discussion in [docs/benchmarks.md](docs/benchmarks.md).

---

## Design decisions worth defending

The comments in this codebase explain *why*, not *what*. A selection:

**Sixteen shards with a plain `std::mutex`, not a `std::shared_mutex`.** Reads are not read-only — every `GET` updates eviction metadata, so a `shared_lock` would be an outright data race. The alternatives were atomic metadata (cost on every access, still torn across two fields) or exclusive locking with enough shards that it stops mattering. → [`keyspace.hpp`](include/bourse/cache/keyspace.hpp)

**Sampled eviction, not exact LRU.** Exact LRU needs an intrusive list touched on every read — turning every read into a write plus a pointer chase through cold memory. Bourse samples and ranks, which is why `--maxmemory-samples` exists. Random *bucket* probing keeps sampling O(1); `std::advance` from `begin()` would make eviction quadratic. → [`eviction.hpp`](include/bourse/cache/eviction.hpp)

**A 64-bit token in the poller, not a pointer.** A pointer registered with the kernel outlives the C++ object if a connection dies between `epoll_wait` returning and dispatch — dereferencing it is a use-after-free. An integer id forces the dispatcher back through the connection table, where a stale token simply misses. → [`poller.hpp`](include/bourse/net/poller.hpp)

**Commands return `Reply` objects, not bytes.** That one indirection is why RESP, REST and the dashboard cannot drift apart: there is one implementation behind all three, and the test suite inspects results structurally without parsing anything. → [`reply.hpp`](include/bourse/exec/reply.hpp)

**Journalling lives in the registry, not in commands.** One hook after dispatch, gated on `Command::isWrite()`. A new write verb is persisted automatically and both transports are covered by construction. Journalling *after* execution and only on success matters too — logging first would persist commands that were then rejected. → [`command_registry.cpp`](src/exec/command_registry.cpp)

**Prices are integer ticks.** `0.1 + 0.2 != 0.3` in binary floating point, so two orders that should cross at the same price compare unequal. There is a test for exactly that. → [`order.hpp`](include/bourse/match/order.hpp)

**Trades print at the resting order's price.** Price improvement accrues to the side that was patient enough to sit on the book — the incentive every venue wants, and a classic thing to get backwards. → [`order_book.cpp`](src/match/order_book.cpp)

**Fill-or-kill checks liquidity before consuming any.** A partial fill it then had to unwind would already have emitted trades that market-data consumers saw. → [`order_book.cpp`](src/match/order_book.cpp)

**Visitor over the SQL AST.** The node set is closed and stable; the *operations* keep growing — evaluate, print for EXPLAIN, collect referenced columns, type-check. Virtual methods on every node would mean editing four classes per operation. There are two visitors in the tree already, and adding the second required changing no node. → [`ast.hpp`](include/bourse/sql/ast.hpp)

**A volcano-model executor.** Uniform `open`/`next`/`close` means `LIMIT 10` over a million rows stops the scan after ten without any operator knowing about any other. The cost is a virtual call per row per operator — which is exactly why real engines moved to vectorised execution, and worth being able to say out loud. → [`engine.hpp`](include/bourse/sql/engine.hpp)

**NULL is a distinct alternative, not a sentinel.** SQL's three-valued logic is not expressible in-band: `NULL = NULL` is NULL, and `WHERE x = NULL` matches nothing. Making it a type forces every comparison site to decide. → [`ast.cpp`](src/sql/ast.cpp)

**A B+ tree, not a hash index.** A hash index answers point lookups in O(1) and range scans not at all. `WHERE ts BETWEEN a AND b`, `ORDER BY price` and "the next 50 rows" are all range queries, and they are most of what a trading system asks. Values live only in leaves so internal nodes fan out ~62 ways — a test asserts 10,000 keys produce a tree at most 4 levels deep. Leaves are singly linked so a range scan never walks back up. → [`bplus_tree.hpp`](include/bourse/storage/bplus_tree.hpp)

**Pinning is what makes the buffer pool safe.** Eviction only ever considers unpinned frames; if all are pinned the pool reports exhaustion rather than pulling a page from under a caller. Forgetting to unpin therefore leaks a frame instead of causing a use-after-free — the failure mode you want. A `PageGuard` makes every early return in the tree code safe. → [`buffer_pool.hpp`](include/bourse/storage/buffer_pool.hpp)

**CRC-32 on every WAL record.** Not paranoia — it is the only way to tell a torn tail from a crash (truncate, carry on) from corruption mid-file (refuse to start). Without it, replay would feed half a command into the keyspace. → [`wal.cpp`](src/storage/wal.cpp)

**Snapshots are written to a temp file and renamed.** A crash mid-write leaves the previous good snapshot or a stray `.tmp` — never a truncated image that `load()` would accept as complete. → [`snapshot.cpp`](src/storage/snapshot.cpp)

**Cache-line separation in the SPSC ring.** Producer writes `write_`, consumer writes `read_`; sharing a line means every push invalidates the consumer's copy. Each side also caches the *other* cursor so in steady state it never touches the other's line. → [`spsc_ring.hpp`](include/bourse/core/spsc_ring.hpp)

**Inline writes before buffering.** `Connection::send` tries the socket first; buffering and waiting for `EPOLLOUT` would add a full loop iteration to every reply. → [`connection.cpp`](src/net/connection.cpp)

### Bugs the tests actually caught

1. **Inline commands need bare `\n`.** The first RESP parser demanded `\r\n`. Stricter, and wrong — `redis-cli --pipe`, shell heredocs and `netcat` all send LF only, and they hung. Caught because the smoke test drives a *real* client. → `test_resp.cpp::ParsesInlineCommandWithBareLf`
2. **`NOT` bound too tightly in SQL.** Parsed as an ordinary prefix operator, `NOT symbol = 'AAPL'` became `(NOT symbol) = 'AAPL'` and returned zero rows instead of two — wrong answers, no error. SQL precedence is `OR < AND < NOT < comparison`. → `test_sql.cpp::NotBindsLooserThanComparison`
3. **A double unpin in B+ tree deletion.** When the root leaf emptied, an explicit `unpin` ran after the `PageGuard` had already released the frame. The pool rejected it loudly instead of silently corrupting the pin count — which is exactly why `unpin` validates. → `test_btree.cpp::ErasingEverythingLeavesAUsableTree`
4. **A data race in eviction.** `enforceMemoryBudget` compared `shard.memory_bytes` across shards without holding their locks. The fix copies each size out under its own lock.
5. **`std::thread::get_id()` after `join()`** returns the default id, not the thread's — a test failed for entirely the wrong reason.

---

## Request path

<p align="center">
  <img src="docs/img/request-flow.svg" alt="Request path from TCP packet through the event loop, codec, registry, and keyspace shard back to the reply" width="100%">
</p>

---

## Commands (57)

| Group | Commands |
|---|---|
| **Strings** | `SET` (`EX`/`PX`/`NX`/`XX`), `GET`, `SETEX`, `APPEND`, `STRLEN` |
| **Counters** | `INCR`, `DECR`, `INCRBY`, `DECRBY` |
| **Generic** | `DEL`, `EXISTS`, `TYPE`, `KEYS` |
| **Expiry** | `EXPIRE`, `PEXPIRE`, `TTL`, `PTTL`, `PERSIST` |
| **Lists** | `LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `LLEN`, `LRANGE` |
| **Hashes** | `HSET`, `HGET`, `HGETALL`, `HDEL`, `HLEN` |
| **Sets** | `SADD`, `SREM`, `SISMEMBER`, `SMEMBERS`, `SCARD` |
| **Pub/Sub** | `SUBSCRIBE`, `UNSUBSCRIBE`, `PUBLISH`, `PUBSUB` |
| **Exchange** | `ORDER`, `CANCEL`, `AMEND`, `BOOK`, `TRADES`, `SYMBOLS`, `EXCHANGE` |
| **SQL** | `SQL`, `EXPLAIN`, `TABLES`, `DESCRIBE` |
| **Server** | `PING`, `ECHO`, `INFO`, `DBSIZE`, `FLUSHALL`, `COMMAND`, `CONFIG`, `QUIT` |

Every execution is republished to `trades:<SYMBOL>`, so `SUBSCRIBE trades:AAPL` in one terminal shows fills produced by orders typed into another.

## HTTP endpoints (16)

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/` | dashboard (embedded in the binary) |
| `GET` | `/health` | liveness + uptime |
| `GET` | `/metrics` | Prometheus exposition format |
| `GET` | `/api/stats` | keyspace, engine and latency percentiles |
| `GET`/`PUT`/`DELETE` | `/api/keys/:key` | key CRUD |
| `GET` | `/api/keys?pattern=` | glob scan |
| `POST` | `/api/command` | run any verb, JSON reply |
| `POST` | `/api/sql` | run SQL, JSON result set |
| `GET` | `/api/tables` | table list |
| `GET` | `/api/book/:symbol` | depth snapshot |
| `GET` | `/api/trades/:symbol` | recent tape |
| `GET` | `/api/symbols` | traded symbols |
| `POST` | `/api/orders` | submit an order |
| `DELETE` | `/api/orders/:symbol/:id` | cancel |

---

## Project layout

```
Bourse/
├── include/bourse/{core,storage,cache,exec,sql,match,net,server}/
├── src/                     implementations, mirroring include/
├── apps/bourse_server/      the executable
├── tests/                   10 GoogleTest files, 228 tests
├── dashboard/index.html     embedded at build time by cmake/EmbedAsset.cmake
├── scripts/
│   ├── setup-wsl.sh         one-shot toolchain provisioning
│   ├── build.sh             configure + build
│   ├── demo.sh              guided tour of every layer
│   ├── verify-all.sh        every check, one command
│   ├── smoke-test.sh        KV over real redis-cli
│   ├── smoke-exchange.sh    HTTP + matching engine
│   ├── smoke-persistence.sh SIGKILL and recover
│   ├── check-sanitizers.sh  ASan+UBSan and TSan
│   └── benchmark.sh         redis-benchmark + server histogram
├── cmake/                   warnings, sanitizers, asset embedding
├── docs/                    architecture.md, benchmarks.md, diagrams
├── .github/workflows/ci.yml matrix, sanitizers, tidy, Docker
└── Dockerfile               multi-stage, non-root
```

---

## Roadmap

- [x] **v0.1** Redis-compatible server — epoll reactor, RESP2, TTL, eviction, pub/sub
- [x] **v0.2** HTTP — codec, router, middleware chain, `/metrics`, REST API, dashboard
- [x] **v0.3** Durability — WAL with CRC framing and torn-tail recovery, atomic snapshots
- [x] **v0.4** SQL — lexer, parser, AST, Visitor, volcano executor
- [x] **v1.0** Exchange — order book, price-time priority, LIMIT/MARKET/IOC/FOK, market data
- [x] **v1.1** On-disk B+ tree — 4 KiB pager with a free list, pinning buffer pool, node splits, range scans
- [ ] **v1.2** Wire the B+ tree in behind the SQL row store, so tables live on disk
- [ ] **v1.3** WebSocket streaming so the dashboard pushes instead of polling
- [ ] **v1.4** Replication and consistent-hash sharding

---

## Building elsewhere

`Poller` has two implementations: `epoll` on Linux, portable `poll` everywhere else. Windows compiles via the Winsock paths, but Linux is the tested target — `perf`, `valgrind` and `redis-benchmark` all live there.

```bash
bash scripts/syntax-check.sh          # type-check every TU without linking
bash scripts/build.sh Debug address   # + AddressSanitizer
bash scripts/build.sh Debug thread    # + ThreadSanitizer
```

---

## License

MIT — see [LICENSE](LICENSE).
