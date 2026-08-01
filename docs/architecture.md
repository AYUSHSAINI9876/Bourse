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

## Durability

- **WAL first, journalled centrally.** The hook lives in `CommandRegistry::dispatch`, gated on `Command::isWrite()`. Putting it there rather than in each command means a newly added write verb is persisted automatically, and RESP and REST cannot disagree about what gets logged. It runs *after* execution and only on success — journalling first would persist commands that were then rejected, and replaying those would produce a keyspace the original server never had.
- **CRC-32 per record, and it is load-bearing.** A process killed mid-`write` leaves a torn final record. The checksum is the only way to distinguish that (truncate the tail, carry on) from corruption in the middle of the file (real data loss, refuse to start). Without it, replay would feed half a command into the keyspace. `WriteAheadLog.TruncatesATornTailAndKeepsGoing` and `smoke-persistence.sh` both exercise this with a deliberately mangled log.
- **`fsync` policy is the throughput knob.** `always` bounds commit rate at disk latency; `everysec` trades a bounded loss window for orders of magnitude more throughput; `never` relies on the page cache. All three are offered because none is right for every deployment, and picking one silently would be worse than making the caller choose.
- **Snapshots are written atomically.** Temp file, fsync, rename. A crash mid-write leaves the previous good image or a stray `.tmp`, never a truncated file that `load()` would accept. After a successful snapshot the WAL is reset, which is what bounds its growth.
- **Snapshots are not point-in-time.** `forEachEntry` takes one shard lock at a time so a save does not stop the world; the cost is that shards are captured at slightly different moments. For a cache that is the right trade, and the WAL is what makes recovery correct regardless.
- **Absolute TTLs.** Deadlines are stored as absolute epoch millis, so a key whose expiry passed while the server was down does not come back to life on restart.

---

## The B+ tree

**Why a B+ tree and not a hash index.** A hash index answers point lookups in O(1) and range scans not at all. `WHERE ts BETWEEN a AND b`, `ORDER BY price`, and "the next 50 rows after this one" are all range queries, and they are most of what a trading system asks. A B+ tree keeps keys ordered, so a range scan is one descent followed by a walk along the linked leaf level — sequential I/O, no re-descent per row.

**Why values live only in leaves.** Internal nodes hold separators only, so each one fans out further, so the tree is shallower, so a lookup costs fewer page reads. With 4 KiB pages the fan-out here is ~62; a test asserts that ten thousand keys produce a tree no more than four levels deep.

**Fixed-width keys.** Variable-length keys need a slotted layout with an indirection array, splits that repack, and a fallback for keys larger than a page. Fixing the width at 60 bytes makes every node a plain array: binary search is a subscript, a split is one memcpy of the upper half, and there is no fragmentation to compact. Real systems do the same thing, indexing a fixed-width prefix and keeping the full key in the heap. An oversized key is **rejected, not truncated** — truncation would make two distinct keys collide and silently corrupt the index.

**The separator moves up from an internal split but is copied up from a leaf split.** Leaves hold every key, so the middle key must stay in the right leaf as well as appear in the parent. Internal nodes hold only separators, so keeping the promoted key in both places would duplicate it. Getting this backwards produces a tree that passes point lookups and loses rows on range scans.

**Pinning is what makes the buffer pool safe.** Every `fetchPage` pins; eviction only ever considers unpinned frames. If every frame is pinned the pool reports exhaustion rather than pulling a page out from under a caller — so forgetting to unpin leaks a frame instead of causing a use-after-free, which is the failure mode you want. A `PageGuard` RAII wrapper means no early return in `bplus_tree.cpp` can leak one.

**Write-back, not write-through.** A single logical insert can touch the same node several times before it settles; writing through would turn that into a dozen disk writes. The dirty flag is sticky — a reader unpinning clean cannot undo a writer's dirty mark, which would silently lose the change.

**Known simplification.** Deletion removes the entry and unlinks a node that becomes empty, but does not redistribute keys between under-full siblings. The tree stays correct, ordered and balanced in depth; a delete-heavy workload just leaves nodes less full than a textbook implementation would. `validate()` is run after every mutation batch in the tests precisely because a subtly wrong B+ tree still answers most queries correctly — spot-checking lookups is not enough, so the tests walk the whole structure asserting sorted keys, separator consistency, equal leaf depth, and a correctly threaded leaf chain.

---

## SQL execution

Two different patterns, chosen for two different reasons.

**Visitor over the expression tree.** The node set (literal, column, unary, binary) is closed and stable; the set of *operations* over it keeps growing — evaluate it, print it for `EXPLAIN`, collect the columns it touches, type-check it. Virtual methods on the nodes would mean editing four classes per new operation. There are already two visitors (`Evaluator`, `PrintVisitor`), and adding the second required changing no node class. That is the argument for the pattern, demonstrated rather than asserted.

**A variant over statements.** The opposite situation: a fixed set of operations ("execute it") over a set of alternatives that rarely changes. A parallel class hierarchy would add ceremony without adding information.

**Volcano-model executor.** Every operator exposes `open`/`next`/`close` and pulls one row at a time from its child. The uniformity is the point: `LIMIT 10` over a million rows stops the scan after ten without any operator knowing about any other, and a new operator can be spliced in anywhere. `SqlFixture.LimitStopsPullingRows` asserts this through the plan's own counters rather than trusting it.

The cost is one virtual call per row per operator, which is exactly why production engines moved to vectorised or compiled execution. At this scale clarity wins, and that trade is worth being able to state out loud.

**Three-valued logic is in the type system.** `Datum` has a distinct NULL alternative rather than a sentinel, because `NULL = NULL` is NULL and `WHERE x = NULL` matches nothing. Making it a type forces every comparison site to decide what it means. `ORDER BY` needs a total order, so it uses a separate `orderingCompare` that sorts nulls first.

**`NOT` binds looser than comparison.** SQL precedence is `OR < AND < NOT < comparison < additive < multiplicative`. Treating `NOT` as an ordinary tight-binding prefix operator — the C convention — makes `NOT a = b` parse as `(NOT a) = b`, which returns silently wrong rows rather than failing. This was a real bug caught by a test.

---

## Matching engine

**Prices are integer ticks, never doubles.** `0.1 + 0.2 != 0.3` in binary floating point, so two orders that should cross at the same price compare unequal. Every exchange that has shipped uses scaled integers.

**Trades print at the resting order's price.** Price improvement accrues to the side that was patient enough to sit on the book. Getting this backwards is a classic exchange bug and is directly tested.

**Intrusive FIFO lists per price level.** The links live inside the `Order`, which comes from an `ObjectPool`. A `std::list<Order>` would allocate a node per order and add a pointer chase; here cancel is an O(1) unlink with no search, and steady-state order entry performs zero calls to `operator new` — asserted by watching the pool's chunk count.

**Fill-or-kill checks liquidity before consuming any.** A partial fill it then had to unwind would already have emitted trades that market-data consumers saw.

**Amending loses time priority unless it only shrinks.** Reducing quantity in place cannot disadvantage anyone else in the queue, so priority is kept. Any other change re-queues, because otherwise an order could be repriced to the front of a queue it never waited in.

**Observer for market data.** The engine publishes to registered observers rather than writing to a socket, so it has no dependency on the network layer at all and is unit-testable with no sockets. Observers are invoked outside the lock: holding it across a callback would serialise every order behind the slowest consumer, and would deadlock if an observer called back in.

---

## Authentication

**One authorization decision, shared by both protocols.** The role check lives in `CommandRegistry::dispatch`, next to the journal hook and for the same reason: a verb added tomorrow is covered by default, and RESP and REST cannot drift apart on what a role may do, because there is only one implementation behind both. A handler that forgot its own check would be a silent hole — there is no handler-level check to forget.

**The required role is derived, not declared.** `requiredRole()` is computed from `isNoAuth()`, `isAdmin()` and `isWrite()`, so the default is the safe one and a command opts *down* rather than having to remember to opt in. A test walks the registry and fails if anything reaches `anonymous` outside an explicit four-name allowlist.

**Identity lives where the protocol puts it.** RESP is a session protocol: credentials are presented once with `AUTH` and apply to every later command, so the `Principal` belongs to the `Connection` — and needs no lock, because a connection is pinned to one event loop for its entire life. HTTP is stateless, so every request carries its own credentials and gets its own `Principal` on the `HttpRequest`.

**Sessions are not stored in the keyspace.** The keyspace is right there and has TTL support, but under `--maxmemory` with an `allkeys-*` policy it evicts whatever it likes — so sessions would be dropped at random under load, and the harder the server is hit, the more often users would be logged out. Session storage needs a different eviction rule from cache storage, so it gets its own store with its own sweep.

**Users are deliberately not journalled.** `USER` answers `isWrite()` as `false` even though it mutates state, because replaying `USER ADD alice hunter2` from the write-ahead log would put a plaintext password in a file on disk. Not persisting users is the lesser problem; persisting the *hash* is the correct fix and is what a future version should do.

Full model and its limitations: [security.md](security.md).

## HTTP layer

**One codec, one router, an explicit middleware chain.** `Middleware` receives the rest of the chain as a `next` continuation rather than holding a `next_` pointer, so the same middleware can be registered on several chains. Not calling `next()` short-circuits — which is how the CORS middleware answers a pre-flight without any handler running, and how the bearer-token middleware rejects an unauthenticated request before any handler sees it.

Middleware takes a **mutable** `HttpRequest` while a handler takes a `const` one, and that distinction is the point: middleware exists to *annotate* a request — resolving a bearer token into a `Principal` — before anything acts on it. Registration order matters and is asserted by the deployment smoke test: CORS runs before auth, so a rejected request still carries the headers a browser needs in order to read the 401 rather than reporting an opaque network error.

**404 and 405 are distinguished.** The router remembers when a path matched under a different verb, because telling a client "the URL is right, the method is wrong" is genuinely more useful than a blanket 404.

**`Content-Length` is always emitted.** Without it the client cannot find the end of the body without waiting for a close, which silently defeats keep-alive.

**Chunked bodies are refused, not ignored.** A parser that quietly ignores `Transfer-Encoding` is a request-smuggling bug; this one answers 411.

**Header lookup is case-insensitive by type.** The header map uses a case-insensitive comparator, so `header("Content-Length")` cannot miss a client that sent `content-length`.

---

## What is deliberately not here

- **No third-party runtime dependencies.** No Boost, no libuv, no hiredis. The event loop, the protocol codec and the containers are the point of the exercise.
- **No `std::format`.** GCC 11 and 12 lack `<format>`; the logger streams into an `ostringstream` so the project builds against any conforming C++20 library.
- **No `std::hardware_destructive_interference_size`.** libstdc++ warns that its value is ABI-sensitive and may change between GCC releases, which would silently change the layout of `SpscRing`. 64 is hard-coded and documented.
- **No sorted sets, no cluster mode, no replication.** They would add surface area without adding a new idea. The roadmap spends that effort on the storage engine and the matching engine instead.
