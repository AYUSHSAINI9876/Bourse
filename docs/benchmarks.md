# Benchmarks

Reproduce everything here with:

```bash
bash scripts/benchmark.sh [port] [requests] [clients]
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

A number without a machine attached to it is not a measurement. WSL2 loopback understates native Linux noticeably, so treat these as a floor.

---

## Throughput

| Workload | Throughput | Client p50 |
|---|--:|--:|
| `SET`, sequential | 42,992 ops/s | 0.535 ms |
| `GET`, sequential | 43,592 ops/s | 0.535 ms |
| `INCR`, sequential | 42,114 ops/s | 0.527 ms |
| `SET`, pipeline depth 16 | 501,253 ops/s | 0.623 ms |
| `GET`, pipeline depth 16 | 598,802 ops/s | 0.647 ms |

## Server-side latency

From the built-in HDR-style histogram, after 1,000,005 commands:

```
command_latency_p50_ns:576
command_latency_p99_ns:2304
```

This covers the whole dispatch path: parse → registry lookup → arity validation → shard lock → hash probe → mutate → encode reply.

---

## Interpreting the 14× pipelining gap

The sequential numbers are **client-bound**. In sequential mode each client blocks for a reply before sending again, so 43k ops/s is measuring loopback round-trip time, not server capacity. The server's own histogram says the same commands take 576 ns of actual work — three orders of magnitude below the 0.535 ms the client observes.

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

## Reproducing with sanitizers

Sanitizer builds are 2–5× slower and are not comparable to the numbers above. They exist to prove correctness, not speed:

```bash
bash scripts/check-sanitizers.sh
```

Current status: **ASan + UBSan clean, TSan clean**, 105/105 tests under both.
