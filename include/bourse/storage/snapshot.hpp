#pragma once

#include <cstdint>
#include <string>

#include "bourse/cache/keyspace.hpp"
#include "bourse/core/result.hpp"

namespace bourse::storage {

struct SnapshotStats {
  std::uint64_t keys = 0;
  std::uint64_t bytes = 0;
  std::int64_t duration_ms = 0;
};

/// Point-in-time-ish keyspace image, the RDB half of the persistence story.
///
/// **Why both a snapshot and a WAL.** The log alone is correct but replay time
/// grows without bound. A snapshot alone loses everything since the last dump.
/// Together, recovery is "load the newest snapshot, then replay only the log
/// records written after it" -- bounded restart time and a bounded loss window.
///
/// **Writes are atomic.** The image goes to `<path>.tmp`, is fsynced, and is
/// then renamed over the real path. A crash mid-write therefore leaves either
/// the previous good snapshot or a stray temp file, never a half-written image
/// that load would happily accept.
///
/// **It is not a single instant.** `Keyspace::forEachEntry` takes one shard
/// lock at a time so a save does not stop the world; the cost is that shards
/// are captured at slightly different moments. For a cache that is the right
/// trade, and the WAL is what makes recovery correct regardless.
class Snapshot {
 public:
  static Result<SnapshotStats> save(const cache::Keyspace& keyspace, const std::string& path);
  static Result<SnapshotStats> load(cache::Keyspace& keyspace, const std::string& path);

  [[nodiscard]] static bool exists(const std::string& path);

  /// "BRSNAP01" -- version is part of the magic so a future format change is a
  /// clean refusal to load rather than a misparse.
  static constexpr std::string_view kMagic = "BRSNAP01";
};

}  // namespace bourse::storage
