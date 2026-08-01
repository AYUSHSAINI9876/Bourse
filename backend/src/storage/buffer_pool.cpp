#include "bourse/storage/buffer_pool.hpp"

#include <algorithm>
#include <cstring>

#include "bourse/core/logger.hpp"

namespace bourse::storage {
namespace {

void putU32(std::byte* out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out[i] = static_cast<std::byte>((value >> (i * 8)) & 0xFF);
  }
}

std::uint32_t getU32(const std::byte* in) {
  std::uint32_t value = 0;
  for (int i = 3; i >= 0; --i) {
    value = (value << 8) | static_cast<std::uint8_t>(in[i]);
  }
  return value;
}

}  // namespace

// ---------------------------------------------------------------------------
// DiskManager
// ---------------------------------------------------------------------------

Result<std::unique_ptr<DiskManager>> DiskManager::open(const std::string& path) {
  Result<File> file = File::open(path, File::Mode::kCreateReadWrite);
  if (!file.ok()) {
    return file.status();
  }

  Result<std::uint64_t> size = file.value().size();
  if (!size.ok()) {
    return size.status();
  }
  const bool fresh = size.value() == 0;

  auto manager = std::unique_ptr<DiskManager>(new DiskManager(std::move(file).value(), path));
  if (fresh) {
    manager->page_count_ = 1;
    manager->root_page_id_ = kInvalidPageId;
    manager->free_list_head_ = kInvalidPageId;
    BOURSE_TRY(manager->writeHeader());
  } else {
    BOURSE_TRY(manager->readHeader());
  }
  return manager;
}

Status DiskManager::writeHeader() {
  std::vector<std::byte> header(kPageSize, std::byte{0});
  putU32(header.data() + 0, kMagic);
  putU32(header.data() + 4, kVersion);
  putU32(header.data() + 8, page_count_);
  putU32(header.data() + 12, root_page_id_);
  putU32(header.data() + 16, free_list_head_);
  return file_.writeAt(0, header.data(), header.size());
}

Status DiskManager::readHeader() {
  std::vector<std::byte> header(kPageSize, std::byte{0});
  BOURSE_TRY(file_.readAt(0, header.data(), header.size()));

  if (getU32(header.data()) != kMagic) {
    return Status::corruption("not a Bourse B+ tree file (magic mismatch)");
  }
  const std::uint32_t version = getU32(header.data() + 4);
  if (version != kVersion) {
    return Status::unsupported("B+ tree file version " + std::to_string(version) + " is not supported");
  }
  page_count_ = getU32(header.data() + 8);
  root_page_id_ = getU32(header.data() + 12);
  free_list_head_ = getU32(header.data() + 16);
  return Status::success();
}

Status DiskManager::readPage(PageId id, void* buffer) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (id >= page_count_) {
    return Status::invalidArgument("read of page " + std::to_string(id) + " beyond end of file");
  }
  return file_.readAt(static_cast<std::uint64_t>(id) * kPageSize, buffer, kPageSize);
}

Status DiskManager::writePage(PageId id, const void* buffer) {
  std::lock_guard<std::mutex> lock(mutex_);
  return file_.writeAt(static_cast<std::uint64_t>(id) * kPageSize, buffer, kPageSize);
}

Result<PageId> DiskManager::allocatePage() {
  std::lock_guard<std::mutex> lock(mutex_);

  // Reuse before growing. A workload that inserts and deletes in a loop would
  // otherwise extend the file forever while most of it sat free.
  if (free_list_head_ != kInvalidPageId) {
    const PageId reused = free_list_head_;
    std::vector<std::byte> page(kPageSize, std::byte{0});
    BOURSE_TRY(file_.readAt(static_cast<std::uint64_t>(reused) * kPageSize, page.data(), page.size()));
    free_list_head_ = getU32(page.data() + 1);  // next pointer lives after the type byte
    BOURSE_TRY(writeHeader());
    return reused;
  }

  const PageId fresh = page_count_++;
  std::vector<std::byte> page(kPageSize, std::byte{0});
  BOURSE_TRY(file_.writeAt(static_cast<std::uint64_t>(fresh) * kPageSize, page.data(), page.size()));
  BOURSE_TRY(writeHeader());
  return fresh;
}

Status DiskManager::freePage(PageId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (id == kHeaderPageId || id >= page_count_) {
    return Status::invalidArgument("cannot free page " + std::to_string(id));
  }

  std::vector<std::byte> page(kPageSize, std::byte{0});
  page[0] = static_cast<std::byte>(PageType::kFree);
  putU32(page.data() + 1, free_list_head_);
  BOURSE_TRY(file_.writeAt(static_cast<std::uint64_t>(id) * kPageSize, page.data(), page.size()));

  free_list_head_ = id;
  return writeHeader();
}

Status DiskManager::setRootPageId(PageId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  root_page_id_ = id;
  return writeHeader();
}

Status DiskManager::sync() {
  std::lock_guard<std::mutex> lock(mutex_);
  return file_.sync();
}

// ---------------------------------------------------------------------------
// BufferPool
// ---------------------------------------------------------------------------

BufferPool::BufferPool(DiskManager& disk, std::size_t frame_count) : disk_(disk) {
  frames_.resize(frame_count == 0 ? 1 : frame_count);
  for (Frame& frame : frames_) {
    frame.data.assign(kPageSize, std::byte{0});
  }
}

int BufferPool::findVictim() {
  // Walk the LRU list from the back (least recently used). Pinned frames are
  // never candidates, which is what makes a pinned pointer safe to hold.
  for (auto it = lru_.rbegin(); it != lru_.rend(); ++it) {
    if (frames_[*it].pin_count == 0) {
      return static_cast<int>(*it);
    }
  }
  return -1;
}

Result<std::byte*> BufferPool::fetchPage(PageId id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto existing = table_.find(id);
  if (existing != table_.end()) {
    const std::size_t index = existing->second;
    ++frames_[index].pin_count;
    ++hits_;
    auto position = lru_index_.find(index);
    if (position != lru_index_.end()) {
      lru_.erase(position->second);
    }
    lru_.push_front(index);
    lru_index_[index] = lru_.begin();
    return frames_[index].data.data();
  }

  ++misses_;

  std::size_t index = 0;
  bool found_free = false;
  for (std::size_t i = 0; i < frames_.size(); ++i) {
    if (frames_[i].page_id == kInvalidPageId) {
      index = i;
      found_free = true;
      break;
    }
  }

  if (!found_free) {
    const int victim = findVictim();
    if (victim < 0) {
      return Status(ErrorCode::kOutOfMemory,
                    "buffer pool exhausted: every frame is pinned (a caller forgot to unpin)");
    }
    index = static_cast<std::size_t>(victim);
    Frame& evicted = frames_[index];
    if (evicted.dirty) {
      BOURSE_TRY(disk_.writePage(evicted.page_id, evicted.data.data()));
    }
    table_.erase(evicted.page_id);
    auto position = lru_index_.find(index);
    if (position != lru_index_.end()) {
      lru_.erase(position->second);
      lru_index_.erase(position);
    }
    ++evictions_;
  }

  Frame& frame = frames_[index];
  BOURSE_TRY(disk_.readPage(id, frame.data.data()));
  frame.page_id = id;
  frame.pin_count = 1;
  frame.dirty = false;

  table_[id] = index;
  lru_.push_front(index);
  lru_index_[index] = lru_.begin();
  return frame.data.data();
}

Result<std::pair<PageId, std::byte*>> BufferPool::newPage() {
  Result<PageId> id = disk_.allocatePage();
  if (!id.ok()) {
    return id.status();
  }
  Result<std::byte*> page = fetchPage(id.value());
  if (!page.ok()) {
    return page.status();
  }
  std::memset(page.value(), 0, kPageSize);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_.find(id.value());
    if (it != table_.end()) {
      frames_[it->second].dirty = true;
    }
  }
  return std::make_pair(id.value(), page.value());
}

Status BufferPool::unpin(PageId id, bool dirty) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = table_.find(id);
  if (it == table_.end()) {
    return Status::notFound("unpin of page " + std::to_string(id) + " which is not resident");
  }
  Frame& frame = frames_[it->second];
  if (frame.pin_count <= 0) {
    return Status::internal("unpin of page " + std::to_string(id) + " which was not pinned");
  }
  --frame.pin_count;
  // Dirty is sticky: one writer marking it dirty must not be undone by another
  // reader unpinning clean.
  frame.dirty = frame.dirty || dirty;
  return Status::success();
}

Status BufferPool::flushPage(PageId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = table_.find(id);
  if (it == table_.end()) {
    return Status::success();  // not resident, nothing buffered to flush
  }
  Frame& frame = frames_[it->second];
  if (!frame.dirty) {
    return Status::success();
  }
  BOURSE_TRY(disk_.writePage(frame.page_id, frame.data.data()));
  frame.dirty = false;
  return Status::success();
}

Status BufferPool::flushAll() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Frame& frame : frames_) {
      if (frame.page_id != kInvalidPageId && frame.dirty) {
        BOURSE_TRY(disk_.writePage(frame.page_id, frame.data.data()));
        frame.dirty = false;
      }
    }
  }
  return disk_.sync();
}

Status BufferPool::deletePage(PageId id) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = table_.find(id);
    if (it != table_.end()) {
      const std::size_t index = it->second;
      if (frames_[index].pin_count > 0) {
        return Status::invalidArgument("cannot delete page " + std::to_string(id) + " while it is pinned");
      }
      frames_[index].page_id = kInvalidPageId;
      frames_[index].dirty = false;
      table_.erase(it);
      auto position = lru_index_.find(index);
      if (position != lru_index_.end()) {
        lru_.erase(position->second);
        lru_index_.erase(position);
      }
    }
  }
  return disk_.freePage(id);
}

std::size_t BufferPool::pinnedFrames() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t pinned = 0;
  for (const Frame& frame : frames_) {
    if (frame.pin_count > 0) {
      ++pinned;
    }
  }
  return pinned;
}

}  // namespace bourse::storage
