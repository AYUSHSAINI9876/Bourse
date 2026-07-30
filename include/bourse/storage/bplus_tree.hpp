#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bourse/storage/buffer_pool.hpp"
#include "bourse/storage/page.hpp"

namespace bourse::storage {

/// Row identifier: what an index entry points at. Opaque to the tree.
using RecordId = std::uint64_t;

/// On-disk B+ tree with fixed-width keys.
///
/// **Why a B+ tree and not a hash index.** A hash index answers point lookups
/// in O(1) and range scans not at all. `WHERE ts BETWEEN a AND b`,
/// `ORDER BY price`, and "the next 50 rows after this one" are all range
/// queries, and they are most of what a trading system asks. A B+ tree keeps
/// keys ordered, so a range scan is one descent followed by a walk along the
/// linked leaf level -- sequential I/O, no re-descent per row.
///
/// **Why values live only in leaves.** Internal nodes hold separators only, so
/// each one fans out further, so the tree is shallower, so a lookup costs fewer
/// disk reads. With 4 KiB pages the fan-out here is ~62, meaning three levels
/// index roughly a quarter of a million entries and a lookup is at most three
/// page reads -- usually one, since the upper levels stay resident in the
/// buffer pool.
///
/// **Leaves are singly linked.** That is the "+" in B+ tree: a range scan does
/// not walk back up to the parent to find the next leaf.
///
/// **Known simplification.** Deletion removes the entry and collapses a node
/// that becomes empty, but does not redistribute keys between under-full
/// siblings. The tree stays correct and ordered; a delete-heavy workload just
/// leaves nodes less full than a textbook implementation would. Redistribution
/// is a contained change to `removeFromParent` and is deliberately deferred
/// rather than half-implemented.
///
/// Not thread-safe. Concurrency needs latch coupling (crabbing) down the tree,
/// which is a separate piece of work; today one tree is used by one thread.
class BPlusTree {
 public:
  BPlusTree(BufferPool& pool, DiskManager& disk) : pool_(pool), disk_(disk) {}

  /// Inserts or overwrites. Keys longer than `kMaxKeySize` are rejected rather
  /// than silently truncated -- truncation would make two distinct keys collide.
  Status insert(std::string_view key, RecordId value);

  [[nodiscard]] Result<std::optional<RecordId>> find(std::string_view key);

  /// Returns false when the key was not present.
  Result<bool> erase(std::string_view key);

  /// Visits every entry in `[begin, end)` in key order. An empty `end` means
  /// "to the last key".
  Status scan(std::string_view begin, std::string_view end,
              const std::function<bool(std::string_view, RecordId)>& visit);

  /// Visits every entry in the tree, in order.
  Status forEach(const std::function<bool(std::string_view, RecordId)>& visit) {
    return scan({}, {}, visit);
  }

  [[nodiscard]] Result<std::size_t> size();
  [[nodiscard]] Result<std::size_t> height();
  [[nodiscard]] bool empty() const { return disk_.rootPageId() == kInvalidPageId; }

  /// Walks the whole structure asserting the invariants: keys sorted within
  /// every node, separators consistent with subtree contents, all leaves at the
  /// same depth, leaf chain in order. Used by the tests after every mutation
  /// batch -- a B+ tree that is subtly wrong still answers most queries
  /// correctly, so spot-checking lookups is not enough.
  [[nodiscard]] Status validate();

  static constexpr std::size_t kLeafHeaderSize = 16;
  static constexpr std::size_t kInternalHeaderSize = 20;
  static constexpr std::size_t kLeafEntrySize = 1 + kMaxKeySize + 8;
  static constexpr std::size_t kInternalEntrySize = 1 + kMaxKeySize + 4;
  static constexpr std::size_t kMaxLeafEntries = (kPageSize - kLeafHeaderSize) / kLeafEntrySize;
  static constexpr std::size_t kMaxInternalKeys = (kPageSize - kInternalHeaderSize) / kInternalEntrySize;

 private:
  struct Split {
    bool happened = false;
    std::string separator;
    PageId right = kInvalidPageId;
  };

  Result<PageId> createLeaf();
  Result<PageId> createInternal();

  /// Descends from the root to the leaf that would contain `key`, pushing every
  /// internal page visited onto `path`.
  Result<PageId> descendToLeaf(std::string_view key, std::vector<PageId>* path);

  Status insertIntoLeaf(PageId leaf_id, std::string_view key, RecordId value, Split* split);
  Status insertIntoParent(const std::vector<PageId>& path, std::size_t level, std::string_view separator,
                          PageId right, Split* split);
  Status removeFromParent(const std::vector<PageId>& path, std::size_t level, PageId child);

  Status validateNode(PageId id, int depth, int* leaf_depth, std::string* previous_key,
                      std::string_view lower, std::string_view upper);

  BufferPool& pool_;
  DiskManager& disk_;
};

}  // namespace bourse::storage
