# Bourse

**A from-scratch trading exchange with its own storage engine, cache, and query layer.** Modern C++20, no third-party runtime dependencies, an `epoll` reactor written by hand, and wire compatibility with `redis-cli`.

<p>
  <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white">
  <img alt="CMake" src="https://img.shields.io/badge/build-CMake%20%2B%20Ninja-064F8C?logo=cmake&logoColor=white">
  <img alt="Tests" src="https://img.shields.io/badge/tests-105%20passing-2ea043">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Linux%20%C2%B7%20WSL%20%C2%B7%20Docker-333">
  <img alt="License" src="https://img.shields.io/badge/license-MIT-blue">
</p>

---

## What this is

A real exchange is not one system, it is five stacked on top of each other: a low-latency matching core, a durable journal, an in-memory state store, a query layer for historical data, and multi-protocol client access. Bourse builds that stack from the bottom up, in one coherent codebase, with every layer written from scratch — no Boost, no libuv, no hiredis.

**Right now the bottom half is real and running.** You can point `redis-cli` at it and it answers, correctly, at speed, with 46 commands and full pipelining support. The upper layers are designed and scaffolded but not yet implemented; the [roadmap](#roadmap) says exactly where the line is, and the architecture diagram draws it.

<p align="center">
  <img src="docs/img/architecture.svg" alt="Bourse layered architecture: L0 core, L1 storage, L2 cache, L3 exec, L4 match, L5 net, L6 dashboard" width="100%">
</p>

---

## Status: honest scorecard

| Layer | Component | State |
|---|---|:--|
| **L0** `core/` | RAII `File` / `Socket`, `ByteBuffer`, bounded `ThreadPool`, lock-free `SpscRing`, `ObjectPool`, `Arena`, async `Logger`, HDR-style `Histogram` | ✅ **Shipped** |
| **L2** `cache/` | 16-shard `Keyspace`, `Value` variant, lazy + active TTL expiry, sampled LRU/LFU/Random/NoEviction | ✅ **Shipped** |
| **L3a** `exec/` | `Command` interface, dispatch registry, 46 verbs, protocol-independent `Reply`, `PubSub` | ✅ **Shipped** |
| **L5** `net/` | `Poller` abstraction (epoll + poll), `EventLoop`, acceptor + N I/O reactors, `RespCodec` | ✅ **Shipped** |
| **L1** `storage/` | Pager, buffer pool, B+ tree, WAL + crash recovery, snapshots | 🚧 Planned |
| **L3b** `sql/` | Lexer → parser → AST → Visitor → volcano executor | 🚧 Planned |
| **L4** `match/` | Order book, price-time priority, LIMIT/MARKET/IOC/FOK | 🚧 Planned |
| **L5+** `net/` | HTTP codec, router, middleware chain, WebSocket | 🚧 Planned |
| **L6** `dashboard/` | Depth chart, trade tape, latency histogram, SQL console | 🚧 Planned |

Nothing in the ✅ rows is a stub. Every one is covered by tests that fail if you break it.

---

## Quick start

### Prerequisites

Linux, or Windows with WSL2. One command provisions everything:

```bash
wsl -d Ubuntu --user root -- bash scripts/setup-wsl.sh
```

That installs `g++`, `cmake`, `ninja`, `libgtest-dev`, and `redis-tools` (for the demo). On a native Linux box, run it with `sudo bash scripts/setup-wsl.sh`.

### Build and run

```bash
bash scripts/build.sh              # RelWithDebInfo + tests
./build/bin/bourse-server --port 6380
```

```
2026-07-29 17:32:44.235 [INFO ] command registry initialised with 46 verbs
2026-07-29 17:32:44.236 [INFO ] bourse-resp listening on 0.0.0.0:6380 (poller=epoll, io_threads=8)
2026-07-29 17:32:44.236 [INFO ] RESP endpoint ready on port 6380 -- try: redis-cli -p 6380 PING
```

### Talk to it with a client nobody here wrote

```console
$ redis-cli -p 6380
127.0.0.1:6380> PING
PONG
127.0.0.1:6380> SET user:1 ayush EX 300
OK
127.0.0.1:6380> GET user:1
"ayush"
127.0.0.1:6380> TTL user:1
(integer) 300
127.0.0.1:6380> INCR pageviews
(integer) 1
127.0.0.1:6380> INCRBY pageviews 99
(integer) 100
127.0.0.1:6380> RPUSH queue a b c
(integer) 3
127.0.0.1:6380> LRANGE queue 0 -1
1) "a"
2) "b"
3) "c"
127.0.0.1:6380> HSET session:9 user ayush role admin
(integer) 2
127.0.0.1:6380> HGETALL session:9
1) "user"
2) "ayush"
3) "role"
4) "admin"
127.0.0.1:6380> GET queue
(error) WRONGTYPE Operation against a key holding the wrong kind of value
127.0.0.1:6380> INFO
# Server
bourse_version:1.0.0
uptime_in_seconds:42
...
```

Pipelining works too — 1000 commands in one stream:

```bash
for i in $(seq 1 1000); do echo "SET pipe:$i $i"; done | redis-cli -p 6380 --pipe
# All data transferred. Waiting for the last reply...
# Last reply received from server.
# errors: 0, replies: 1000
```

### Docker

```bash
docker compose up --build -d
redis-cli -p 6380 PING          # PONG
docker compose down
```

The runtime image is a two-stage build — no compiler, no build tree, ~80 MB.

### Command-line options

```
--host <addr>              bind address                     (default 0.0.0.0)
--port <n>                 RESP port                        (default 6380)
--io-threads <n>           I/O event loops, 0 = auto        (default 0)
--shards <n>               keyspace shards, power of two    (default 16)
--maxmemory <size>         eviction budget, e.g. 256mb      (default unlimited)
--maxmemory-policy <name>  allkeys-lru | allkeys-lfu |
                           allkeys-random | noeviction      (default allkeys-lru)
--maxmemory-samples <n>    eviction sample size             (default 5)
--log-level <level>        trace|debug|info|warn|error      (default info)
```

---

## Testing

Three independent layers of verification. All three are wired into CI.

### 1. Unit and integration suite — 105 tests, 23 suites

```bash
bash scripts/build.sh
./build/bin/bourse_tests
# or, through CTest:
ctest --test-dir build --output-on-failure
```

```
[==========] 105 tests from 23 test suites ran. (516 ms total)
[  PASSED  ] 105 tests.
```

These are not smoke tests. A sample of what they actually pin down:

- **`RespParser.ReportsIncompleteForEveryProperPrefix`** — feeds the parser *every* prefix of a valid command and requires that it never claims success and never consumes a byte until the frame is whole. This is the classic "assumed one `read()` = one request" bug, tested exhaustively.
- **`ServerFixture.HandlesRequestsSplitAcrossPackets`** — the same property over a real TCP socket, one byte at a time with sleeps in between.
- **`Keyspace.ConcurrentIncrementsLoseNothing`** — 8 threads × 2000 increments must produce exactly the arithmetic total.
- **`SpscRing.SingleProducerSingleConsumerDeliversEverything`** — 200k items across a lock-free ring with ordering assertions on every one.
- **`ObjectPool.SteadyStateStopsAllocating`** — asserts the chunk count stops moving after warm-up, which is the actual property the pool exists for.
- **`Keyspace.NoEvictionRejectsWritesInsteadOfDroppingData`** — proves the policy refuses writes rather than silently discarding your data.

### 2. End-to-end smoke test — 68 assertions via real `redis-cli`

```bash
bash scripts/smoke-test.sh
```

```
==> starting bourse-server on port 6390
  ok   PING                                       -> PONG
  ok   GET greeting (appended)                    -> hello world
  ok   INCR on non-numeric                        -> ERR value is not an integer or out of range
  ok   GET on a list (WRONGTYPE)                  -> WRONGTYPE Operation against a key holding the wrong kind of value
  ok   redis-cli --pipe reports 0 errors          -> 1
  ...
-------------------------------------------
passed: 68   failed: 0
-------------------------------------------
```

Wire compatibility is demonstrated, not claimed — the client is stock `redis-cli`.

### 3. Sanitizers — ASan, UBSan and TSan all clean

```bash
bash scripts/check-sanitizers.sh
```

Builds and runs the whole suite twice: once under **ASan + UBSan** (with `detect_leaks=1`), once under **TSan**. ASan and TSan are mutually exclusive, hence two build trees.

```
===================================================================
  AddressSanitizer + UndefinedBehaviorSanitizer
===================================================================
[==========] 105 tests from 23 test suites ran. (836 ms total)
[  PASSED  ] 105 tests.
PASS: AddressSanitizer + UndefinedBehaviorSanitizer

===================================================================
  ThreadSanitizer
===================================================================
[==========] 105 tests from 23 test suites ran. (1115 ms total)
[  PASSED  ] 105 tests.
PASS: ThreadSanitizer

===================================================================
  all sanitizer suites clean
===================================================================
```

The concurrency tests are the point here: TSan is exercising 8-thread keyspace contention, the lock-free SPSC ring, the multi-reactor server with 32 concurrent clients, and cross-loop pub/sub delivery. Clean under those is a meaningful result, not a formality.

---

## Design decisions worth defending

The comments in this codebase explain *why*, not *what*. A representative selection:

**Sixteen shards with a plain `std::mutex`, not a `std::shared_mutex`.**
Reads are not read-only — every `GET` updates the entry's eviction metadata, so a `shared_lock` would be an outright data race. The options were atomic metadata (extra cost on every access, still torn across two fields) or exclusive locking with enough shards that contention stops mattering. The simple, obviously-correct option won. → [`keyspace.hpp`](include/bourse/cache/keyspace.hpp)

**Sampled eviction, not exact LRU.**
Exact LRU needs an intrusive list touched on every read — turning every read into a write plus a pointer chase through cold memory. Bourse draws a small random sample and ranks only that, which is why `--maxmemory-samples` exists. Random bucket probing keeps sampling O(1); `std::advance` from `begin()` would have made eviction quadratic. → [`eviction.hpp`](include/bourse/cache/eviction.hpp)

**A 64-bit token in the poller, not a pointer.**
A pointer registered with the kernel outlives the C++ object if a connection is destroyed between `epoll_wait` returning and the dispatch — dereferencing it is a use-after-free. An integer id forces the dispatcher back through the connection table, where a stale token simply misses. → [`poller.hpp`](include/bourse/net/poller.hpp)

**Commands return `Reply` objects, not bytes.**
That indirection is what lets one command set serve three transports: RESP encodes it, the REST layer will render it as JSON, and tests inspect it structurally without parsing anything. Returning pre-encoded RESP — the obvious shortcut — would weld the command set to one wire format. → [`reply.hpp`](include/bourse/exec/reply.hpp)

**A bounded thread-pool queue.**
An unbounded queue converts overload into unbounded memory growth and then an OOM kill. A bounded queue converts it into backpressure, which is what a server actually wants. → [`thread_pool.hpp`](include/bourse/core/thread_pool.hpp)

**Cache-line separation in the SPSC ring.**
The producer writes `write_`, the consumer writes `read_`. Sharing a line means every push invalidates the consumer's copy — textbook false sharing, roughly 4× throughput on x86. Each side also caches the *other* cursor so that in steady state it never touches the other's line at all. → [`spsc_ring.hpp`](include/bourse/core/spsc_ring.hpp)

**Inline writes before buffering.**
`Connection::send` tries the socket first. Buffering and waiting for `EPOLLOUT` would add a full loop iteration of latency to every single reply; only a partial write parks the remainder and arms writability. → [`connection.cpp`](src/net/connection.cpp)

**`std::string_view` over the read buffer.**
The RESP parser never copies to tokenise. `ByteBuffer` compacts by `memmove`-ing only the *unread* region before it will reallocate, so a long-lived connection reaches a steady state and stops allocating entirely. → [`byte_buffer.cpp`](src/core/byte_buffer.cpp)

### A bug this design caught

The first parser required `\r\n` to terminate an inline command. Stricter, and wrong: Redis accepts a bare `\n` too, so every client producing Unix line endings — `redis-cli --pipe`, shell heredocs, `netcat` from a file — hung waiting for a reply that never came. The smoke test caught it because it drives a real client rather than a hand-rolled one. Fix and regression test: [`test_resp.cpp`](tests/test_resp.cpp) → `ParsesInlineCommandWithBareLf`.

---

## Request path

<p align="center">
  <img src="docs/img/request-flow.svg" alt="Request path from TCP packet through the event loop, codec, registry, and keyspace shard back to the reply" width="100%">
</p>

---

## Supported commands (46)

| Group | Commands |
|---|---|
| **Strings** | `SET` (with `EX`/`PX`/`NX`/`XX`), `GET`, `SETEX`, `APPEND`, `STRLEN` |
| **Counters** | `INCR`, `DECR`, `INCRBY`, `DECRBY` |
| **Generic** | `DEL`, `EXISTS`, `TYPE`, `KEYS` (glob) |
| **Expiry** | `EXPIRE`, `PEXPIRE`, `TTL`, `PTTL`, `PERSIST` |
| **Lists** | `LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `LLEN`, `LRANGE` |
| **Hashes** | `HSET`, `HGET`, `HGETALL`, `HDEL`, `HLEN` |
| **Sets** | `SADD`, `SREM`, `SISMEMBER`, `SMEMBERS`, `SCARD` |
| **Pub/Sub** | `SUBSCRIBE`, `UNSUBSCRIBE`, `PUBLISH`, `PUBSUB` |
| **Server** | `PING`, `ECHO`, `INFO`, `DBSIZE`, `FLUSHALL`, `COMMAND`, `CONFIG`, `QUIT` |

Integer-encoded values are a real internal optimisation — `INCR` on a counter never parses or re-serialises a string — and are invisible to clients, exactly as in Redis.

---

## Benchmarks

`redis-benchmark` ships with `redis-tools`, so the standard tool measures Bourse unmodified:

```bash
bash scripts/benchmark.sh
```

### Measured results

**Machine:** 4-core WSL2 VM (kernel 6.6.87.2-microsoft-standard-WSL2), GCC 15.2.0, `RelWithDebInfo`, 200,000 requests, 50 concurrent clients.

| Workload | Throughput | Client-observed p50 |
|---|--:|--:|
| `SET`, no pipelining | 42,992 ops/s | 0.535 ms |
| `GET`, no pipelining | 43,592 ops/s | 0.535 ms |
| `INCR`, no pipelining | 42,114 ops/s | 0.527 ms |
| `SET`, pipeline depth 16 | **501,253 ops/s** | 0.623 ms |
| `GET`, pipeline depth 16 | **598,802 ops/s** | 0.647 ms |

Server-side command latency, straight from the built-in HDR histogram after 1,000,005 commands:

```
total_commands_processed:1000005
command_latency_p50_ns:576
command_latency_p99_ns:2304
```

**p50 576 ns, p99 2.3 µs** for the full dispatch path: parse → registry lookup → arity check → shard lock → hash probe → mutate → encode reply.

### Reading these honestly

The unpipelined figures are **client-bound, not server-bound**. Sequential mode means each client waits for a reply before sending again, so ~43k ops/s is measuring round-trip time over WSL2 loopback, not the server's capacity — which is exactly why the server-side histogram reports sub-microsecond work for the same commands. The pipelined figures are the ones that actually load the server, and the 14× jump between them is the round-trip cost being amortised away.

WSL2 loopback also understates native Linux noticeably. Run it on your own hardware and record *that* number with the machine spec beside it — quoting someone else's throughput figure is worth nothing in an interview, and quoting your own without the machine is worth little more.

---

## Project layout

```
Bourse/
├── include/bourse/          public headers, one directory per layer
│   ├── core/                RAII wrappers, buffers, concurrency, metrics
│   ├── cache/               keyspace, value type, eviction policies
│   ├── exec/                command interface, registry, reply, pub/sub
│   ├── net/                 poller, event loop, connection, server, codecs
│   └── server/              configuration and top-level assembly
├── src/                     implementations, mirroring include/
├── apps/bourse_server/      the executable
├── tests/                   GoogleTest suites (105 tests)
├── scripts/
│   ├── setup-wsl.sh         one-shot toolchain provisioning
│   ├── build.sh             configure + build
│   ├── syntax-check.sh      fast type-check without linking
│   ├── smoke-test.sh        end-to-end via real redis-cli
│   └── check-sanitizers.sh  ASan+UBSan and TSan runs
├── cmake/                   warnings, sanitizers, asset embedding
├── docs/img/                architecture and request-flow diagrams
├── .github/workflows/ci.yml build matrix, sanitizers, tidy, Docker
├── Dockerfile               multi-stage, non-root, ~80 MB runtime
└── docker-compose.yml
```

---

## Roadmap

Each phase is a complete, demoable release on its own — the project is never in a half-built state.

- [x] **v0.1 — Redis-compatible server.** `epoll` reactor, RESP2, 46 verbs, TTL, eviction, pub/sub, 105 tests.
- [ ] **v0.2 — HTTP.** `HttpCodec`, router with path params, middleware chain (Chain of Responsibility), `/metrics` in Prometheus format, plus a reactor-vs-thread-pool benchmark using the existing `Poller` abstraction.
- [ ] **v0.3 — Storage engine.** Pager, buffer pool with LRU-K, on-disk B+ tree, WAL with crash recovery, snapshot + AOF persistence for the keyspace.
- [ ] **v0.4 — SQL.** Lexer → recursive-descent parser → AST → Visitor-based binder → volcano-model iterators over the B+ tree.
- [ ] **v1.0 — The exchange.** Order book with price-time priority, `LIMIT`/`MARKET`/`IOC`/`FOK`, zero-allocation hot path over the existing `ObjectPool` and `SpscRing`, trades journalled to the WAL, market data over WebSocket.
- [ ] **v1.1 — Dashboard.** Single self-contained HTML page: live depth chart, trade tape, latency histogram, SQL console.

---

## Building on other platforms

The `Poller` interface has two implementations: `epoll` on Linux and portable `poll` everywhere else. The codebase compiles on Windows via the Winsock paths in `socket.cpp` and `poller.cpp`, but Linux is the tested, supported target — `perf`, `valgrind` and `redis-benchmark` all live there, and so does the interesting half of the systems work.

```bash
bash scripts/syntax-check.sh          # type-check every TU without linking
bash scripts/build.sh Debug address   # Debug + AddressSanitizer
bash scripts/build.sh Debug thread    # Debug + ThreadSanitizer
```

---

## License

MIT — see [LICENSE](LICENSE).
