# Architecture

This document covers the decisions that are not obvious from reading the code, and the ones where a reasonable engineer would have chosen differently. For the layer map, see [`img/architecture.svg`](img/architecture.svg).

---

## Threading model

```
                    ┌──────────────────┐
   listen(2)  ──►   │  acceptor loop   │   own thread, own epoll fd
                    └────────┬─────────┘
                             │ round-robin, post() at accept time
          ┌──────────────────┼──────────────────┐
          ▼                  ▼                  ▼
   ┌────────────┐     ┌────────────┐     ┌────────────┐
   │ I/O loop 0 │     │ I/O loop 1 │ ... │ I/O loop N │
   └────────────┘     └────────────┘     └────────────┘
    connections        connections         connections
    pinned for life    pinned for life     pinned for life
```

Three models were considered:

| Model | Why not |
|---|---|
| Thread per connection | 10k clients means 10k stacks and a scheduler that spends its time context-switching. |
| Single reactor | No locking anywhere, but one core is the hard ceiling. |
| **Acceptor + N reactors** | Chosen. Each connection is bound to one loop for its whole life, so per-connection state still needs no locking, and throughput scales with cores. |

The only shared structure is the connection table in `TcpServer`. It is touched on accept, on close, and on cross-loop delivery — never on the per-request path.

**Cross-thread work never touches a connection directly.** Pub/sub delivery and (later) market-data fan-out resolve a connection id under the table's lock, then `post()` a closure to that connection's owning loop. Writing from the publisher's thread would race with the owning loop's own writes to the same output buffer.

---

## Why the keyspace uses a plain mutex per shard

The instinct is `std::shared_mutex`: many readers, occasional writer. It is wrong here.

Reads are not read-only. Every `GET` updates the entry's `EvictionMetadata` — that is how LRU and LFU know anything. Under a `shared_lock` two readers would mutate the same 16 bytes concurrently, which is a data race by definition, not a benign one.

The alternatives were:

1. **Atomic metadata.** Costs an atomic RMW on every read, and `last_access_ms` plus `frequency` cannot be updated atomically together without a wider type or a seqlock.
2. **Exclusive locking, many shards.** Sixteen shards give sixteen-way write concurrency, which saturates well past the point the event loop becomes the bottleneck.

Option 2 won on the grounds that it is obviously correct and measurably sufficient. `--shards` is tunable if a workload ever proves otherwise. `Keyspace.ConcurrentIncrementsLoseNothing` and the TSan run are what keep this honest.

---

## Expiry: why both halves are necessary

- **Passive** — checked on every access, in `Keyspace::lookup`. Free, but leaks memory for keys nobody reads again.
- **Active** — `activeExpireCycle()` samples up to N keys per shard on a timer. Bounded CPU regardless of keyspace size, but cannot keep up alone with a large keyspace.

Neither alone is sufficient; the pair is what Redis does, for the same reason. The cycle runs on the acceptor loop, which is otherwise idle between connection storms, so it costs no extra thread.

---

## Eviction is sampled, not exact

Exact LRU requires an intrusive list node per entry, moved to the head on every read. That turns each read into a write plus a pointer chase through cold memory, and it doubles the per-entry footprint.

Bourse instead draws a small random sample and asks the `EvictionPolicy` to rank only that. The trade-off is explicit and tunable through `--maxmemory-samples`: larger samples approximate true LRU more closely and cost more CPU.

Sampling itself must be O(1) or eviction becomes quadratic. `std::advance(map.begin(), rand() % size)` is the obvious implementation and is O(n); Bourse probes random hash buckets instead.

**LFU needs decay.** A plain frequency counter would pin whatever was hot during start-up forever. The counter increments probabilistically (so it saturates gracefully rather than growing without bound) and halves per elapsed decay interval.

---

## Error handling

Exceptions are used only for genuinely exceptional conditions — `bad_alloc`, and programmer error caught by `assert`. Every *expected* failure travels through `Status` / `Result<T>`:

- a missing key
- a malformed command
- a short read
- a socket that would block

This keeps unwinding tables out of the hot path and, more importantly, makes failure modes visible in every signature. `Result<T>` exists because `std::expected` is C++23 and this project targets C++20.

`IoResult` deserves special mention. A plain `Result<size_t>` would force the event loop to inspect an error string to distinguish "no data right now" (normal — re-arm and return) from "peer closed" (tear down) from "real error" (log and tear down). Making those three states part of the type removes an entire class of bug from the connection state machine.

---

## Durability (planned, L1)

The design is written down now so the interfaces above it do not have to change later:

- **WAL first.** Every mutating command appends to the write-ahead log before it is acknowledged. `Command::isWrite()` already exists for exactly this — the AOF writer asks the command object rather than maintaining a second list of verbs that can drift out of sync.
- **`fsync` policy is the throughput knob.** Per-command `fsync` bounds commit rate at disk latency; per-interval trades a bounded window of writes for orders of magnitude more throughput. Both will be offered; neither is a default that should be picked silently.
- **Snapshots are not point-in-time.** `forEachEntry` takes one shard lock at a time so a snapshot does not stop the world. The cost is that a snapshot is not a single consistent instant across shards. For a cache this is the right trade; for a system of record it would not be, and the WAL is what makes recovery correct regardless.

---

## What is deliberately not here

- **No third-party runtime dependencies.** No Boost, no libuv, no hiredis. The event loop, the protocol codec and the containers are the point of the exercise.
- **No `std::format`.** GCC 11 and 12 lack `<format>`; the logger streams into an `ostringstream` so the project builds against any conforming C++20 library.
- **No `std::hardware_destructive_interference_size`.** libstdc++ warns that its value is ABI-sensitive and may change between GCC releases, which would silently change the layout of `SpscRing`. 64 is hard-coded and documented.
- **No sorted sets, no cluster mode, no replication.** They would add surface area without adding a new idea. The roadmap spends that effort on the storage engine and the matching engine instead.
