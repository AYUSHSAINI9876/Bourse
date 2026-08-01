# Benchmarks

Reproduce everything here with:

```bash
bash backend/scripts/benchmark.sh [port] [requests] [clients]
```

---

## Machine

Every number below was measured on:

| | |
|---|---|
| Host | 4-core WSL2 VM on Windows 11 |
| Kernel | 6.6.87.2-microsoft-standard-WSL2 |
| Compiler | GCC 15.2.0 |
| Build | `RelWithDebInfo`, no sanitizers |
| Client | stock `redis-benchmark` 8.0.5 |
| Load | 200,000 requests, 50 concurrent clients |

A number without a machine attached to it is not a measurement -- and a number from *one run* on a machine this noisy is barely one either. See the spread below.

---

## Throughput

Three runs on the same machine and the same binary, so the spread below is the
measurement environment rather than the code:

| Workload | Run A | Run B | Run C |
|---|--:|--:|--:|
| `SET`, sequential | 42,992 | 21,064 | 19,614 |
| `GET`, sequential | 43,592 | 21,894 | 11,752 |
| `INCR`, sequential | 42,114 | 24,558 | 52,466 |
| `SET`, pipeline depth 16 | 501,253 | 208,117 | **1,342,282** |
| `GET`, pipeline depth 16 | 598,802 | 248,447 | **1,273,885** |

A 5× spread on identical code. Run B started immediately after two sanitizer
builds; run C was on a freshly restarted VM but still showed the host stalling
mid-measurement — one `GET` window fell to 32 ops/s with a 2,993 ms average
before recovering to 60,000 ops/s.

**So do not quote a single number from this table.** A 4-core WSL2 VM on a
laptop, sharing a kernel with Windows and reaching the server over emulated
loopback, is not a measurement platform. Publishing one run from it as *the*
throughput figure would be the kind of benchmark that is technically true and
substantively meaningless.

## Server-side latency

This is the number worth trusting, because it excludes the client, the
loopback and the scheduler entirely — it is the server timing its own work with
the built-in HDR-style histogram, over 1,000,005 commands:

| | Run A | Run B | Run C |
|---|--:|--:|--:|
| `command_latency_p50_ns` | 576 | 576 | 416 |
| `command_latency_p99_ns` | 2,304 | 3,328 | 2,560 |

Tight across runs where the client-side figures moved 5×, which is exactly the
expected shape: the server's own work is stable and the variance lives in
everything around it.

This covers the whole dispatch path: parse → registry lookup → arity validation
→ **permission check** → shard lock → hash probe → mutate → encode reply.

### Does authentication cost anything?

No, when it is off — and off is the default. The check short-circuits on a
pointer and a bool before touching the principal:

```cpp
if (context.server.auth != nullptr && context.server.auth->enabled()) { … }
```

Runs A and B predate the auth layer; run C includes it. p50 went from 576 ns to
416 ns across that change, which is not evidence that auth made the server
faster — it is evidence that the difference is below this machine's noise
floor.

With auth *enabled*, the added work per command is one integer comparison
(`role >= required`). The expensive part of authentication is PBKDF2, and that
runs once per login, never per command — which is why login is rate-limited and
command dispatch is not.

---

## Interpreting the pipelining gap

The sequential numbers are **client-bound**. In sequential mode each client blocks for a reply before sending again, so those figures measure loopback round-trip time, not server capacity. The server's own histogram says the same commands take 416--576 ns of actual work — three orders of magnitude below the ~0.5 ms the client observes.

The pipelined numbers are the ones that actually saturate the server. The gap between them is round-trip cost being amortised away, and it is the single most useful thing this benchmark demonstrates: **for a store this fast, the network dominates unless the client batches.**

---

## Where the throughput came from

Two changes mattered, in order of impact.

### 1. `std::string_view` in the parser

The first version of `RespCodec::onData` built a `std::string` from the read buffer on every iteration so the parser had something to walk. That is one allocation and one copy per command, on the hottest path in the program.

`ByteBuffer::view()` now hands the parser a zero-copy window over the unread region, and `parseRespCommand` takes a `std::string_view`. The parser itself never allocates; only the final `argv` strings are materialised, and those are needed anyway.

This is the change worth describing in an interview, because the reasoning generalises: *the fix was not a faster algorithm, it was removing work that never needed to happen.*

### 2. Batched replies

The codec accumulates every reply from a pipelined batch into one `std::string` and issues a single `Connection::send`. A client that ships 100 commands in one packet gets one `write(2)` back instead of 100. Combined with the inline-write fast path in `send()` — which tries the socket before buffering — a small reply on an idle connection costs exactly one syscall and no `EPOLLOUT` round-trip.

---

## Things that did *not* need optimising

Worth recording, because the instinct to optimise them is strong and would have been wasted effort:

- **Shard count.** 16 was not a bottleneck at any load tested. The event loop saturates first.
- **Eviction sampling.** With the default sample size of 5, `enforceMemoryBudget` does not appear in profiles at all under normal load.
- **The logger.** Level filtering happens before argument formatting, so a disabled `BOURSE_LOG_DEBUG` costs one relaxed atomic load.

---

## Matching engine

The order book is instrumented separately, under `bourse_match_latency_nanos`:

```bash
redis-cli -p 6380 ORDER AAPL SELL LIMIT 100 100.00
redis-cli -p 6380 INFO | grep match
```

Two properties matter more than the raw number and are asserted by tests rather than measured:

- **Steady-state order entry allocates nothing.** `OrderBookTest.SteadyStateOrderEntryStopsAllocating` submits and cancels 19,000 orders after warm-up and requires the `ObjectPool`'s chunk count not to move. An allocation on the hot path is a latency spike waiting to happen, so this is a correctness assertion, not a performance one.
- **Cancel is O(1).** Orders carry their own intrusive list links, so removing one is an unlink with no search through the price level.

---

## Reproducing with sanitizers

Sanitizer builds are 2–5× slower and are not comparable to the numbers above. They exist to prove correctness, not speed:

```bash
bash backend/scripts/check-sanitizers.sh
```

Current status: **ASan + UBSan clean, TSan clean**, 287/287 tests under both.
