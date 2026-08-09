#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "bourse/core/file.hpp"
#include "bourse/core/result.hpp"
#include "bourse/storage/page.hpp"

namespace bourse::storage {

/// Owns the file and hands out raw pages by id.
///
/// Deliberately dumb: it knows about page numbers and a free list, and nothing
/// about what a page contains. The B+ tree layer above it never touches the
/// file, and the buffer pool between them never touches tree structure.
class DiskManager {
 public:
  static Result<std::unique_ptr<DiskManager>> open(const std::string& path);

  Status readPage(PageId id, void* buffer);
  Status writePage(PageId id, const void* buffer);

  /// Reuses a page from the free list if there is one, otherwise extends the
  /// file. Reuse matters: without it, a workload that inserts and deletes in a
  /// loop grows the file without bound.
  Result<PageId> allocatePage();
  Status freePage(PageId id);

  Status sync();

  [[nodiscard]] std::uint32_t pageCount() const noexcept { return page_count_; }

  [[nodiscard]] PageId rootPageId() const noexcept { return root_page_id_; }

  Status setRootPageId(PageId id);

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

  static constexpr std::uint32_t kMagic = 0x42545245;  // "BTRE"
  static constexpr std::uint32_t kVersion = 1;

 private:
  DiskManager(File file, std::string path) : file_(std::move(file)), path_(std::move(path)) {}

  Status writeHeader();
  Status readHeader();

  File file_;
  std::string path_;
  std::uint32_t page_count_ = 1;  ///< page 0 is the header
  PageId root_page_id_ = kInvalidPageId;
  PageId free_list_head_ = kInvalidPageId;
  mutable std::mutex mutex_;
};

/// Fixed-size frame cache in front of the DiskManager.
///
/// **Pinning, not just caching.** A frame that some caller is reading must not
/// be evicted underneath it, so every fetch pins and the caller must unpin.
/// Eviction only ever considers unpinned frames; if every frame is pinned the
/// pool reports exhaustion rather than corrupting someone's page. Forgetting to
/// unpin therefore leaks a frame instead of causing a use-after-free, which is
/// the failure mode you want.
///
/// **Write-back, not write-through.** A dirtied page is flushed on eviction or
/// on an explicit flush, not on every mutation. A B+ tree insert can touch the
/// same node many times before it settles; writing through would turn one
/// logical insert into a dozen disk writes.
///
/// Replacement is LRU over unpinned frames. LRU-K would resist the sequential
/// scan that evicts a hot root, and is the documented next step -- but it needs
/// a second access-history structure, and LRU is the honest baseline to beat.
class BufferPool {
 public:
  BufferPool(DiskManager& disk, std::size_t frame_count = 128);

  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;

  /// Returns a pinned page. The caller must `unpin` exactly once.
  Result<std::byte*> fetchPage(PageId id);
  /// Allocates a fresh page, zero-filled and pinned.
  Result<std::pair<PageId, std::byte*>> newPage();
  Status unpin(PageId id, bool dirty);
  Status deletePage(PageId id);

  Status flushPage(PageId id);
  Status flushAll();

  [[nodiscard]] std::size_t hits() const noexcept { return hits_; }

  [[nodiscard]] std::size_t misses() const noexcept { return misses_; }

  [[nodiscard]] std::size_t evictions() const noexcept { return evictions_; }

  [[nodiscard]] std::size_t pinnedFrames() const;

  [[nodiscard]] std::size_t frameCount() const noexcept { return frames_.size(); }

 private:
  struct Frame {
    PageId page_id = kInvalidPageId;
    int pin_count = 0;
    bool dirty = false;
    std::vector<std::byte> data;
  };

  /// Caller must hold `mutex_`. Returns -1 when every frame is pinned.
  int findVictim();

  DiskManager& disk_;
  std::vector<Frame> frames_;
  std::unordered_map<PageId, std::size_t> table_;
  /// Most-recently-used at the front; only unpinned frames are candidates.
  std::list<std::size_t> lru_;
  std::unordered_map<std::size_t, std::list<std::size_t>::iterator> lru_index_;

  mutable std::mutex mutex_;
  std::size_t hits_ = 0;
  std::size_t misses_ = 0;
  std::size_t evictions_ = 0;
};

}  // namespace bourse::storage
