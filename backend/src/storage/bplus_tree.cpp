#include "bourse/storage/bplus_tree.hpp"

#include <algorithm>
#include <cstring>

#include "bourse/core/logger.hpp"

namespace bourse::storage {
namespace {

// ---------------------------------------------------------------------------
// Raw page accessors
//
// Every node is an array of fixed-width entries, so there is no slot directory
// and no fragmentation: a binary search is a subscript, and a split is one
// memcpy of the upper half.
// ---------------------------------------------------------------------------

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

void putU64(std::byte* out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<std::byte>((value >> (i * 8)) & 0xFF);
  }
}

std::uint64_t getU64(const std::byte* in) {
  std::uint64_t value = 0;
  for (int i = 7; i >= 0; --i) {
    value = (value << 8) | static_cast<std::uint8_t>(in[i]);
  }
  return value;
}

PageType typeOf(const std::byte* page) { return static_cast<PageType>(page[0]); }
void setType(std::byte* page, PageType type) { page[0] = static_cast<std::byte>(type); }

std::uint32_t countOf(const std::byte* page) { return getU32(page + 1); }
void setCount(std::byte* page, std::uint32_t count) { putU32(page + 1, count); }

PageId nextLeafOf(const std::byte* page) { return getU32(page + 5); }
void setNextLeaf(std::byte* page, PageId id) { putU32(page + 5, id); }

std::byte* leafEntry(std::byte* page, std::size_t index) {
  return page + BPlusTree::kLeafHeaderSize + index * BPlusTree::kLeafEntrySize;
}
const std::byte* leafEntry(const std::byte* page, std::size_t index) {
  return page + BPlusTree::kLeafHeaderSize + index * BPlusTree::kLeafEntrySize;
}

std::string_view entryKey(const std::byte* entry) {
  const auto length = static_cast<std::size_t>(std::to_integer<std::uint8_t>(entry[0]));
  return {reinterpret_cast<const char*>(entry + 1), length};
}

void setEntryKey(std::byte* entry, std::string_view key) {
  entry[0] = static_cast<std::byte>(static_cast<std::uint8_t>(key.size()));
  std::memcpy(entry + 1, key.data(), key.size());
  // Zero the tail so a shorter key overwriting a longer one leaves no residue
  // that a hex dump or a future format change could misread.
  if (key.size() < kMaxKeySize) {
    std::memset(entry + 1 + key.size(), 0, kMaxKeySize - key.size());
  }
}

RecordId leafValue(const std::byte* entry) { return getU64(entry + 1 + kMaxKeySize); }
void setLeafValue(std::byte* entry, RecordId value) { putU64(entry + 1 + kMaxKeySize, value); }

/// Internal layout: [header 16][child0 u32][key0 child1][key1 child2]...
std::byte* internalChild0(std::byte* page) { return page + BPlusTree::kLeafHeaderSize; }
const std::byte* internalChild0(const std::byte* page) { return page + BPlusTree::kLeafHeaderSize; }

std::byte* internalEntry(std::byte* page, std::size_t index) {
  return page + BPlusTree::kInternalHeaderSize + index * BPlusTree::kInternalEntrySize;
}
const std::byte* internalEntry(const std::byte* page, std::size_t index) {
  return page + BPlusTree::kInternalHeaderSize + index * BPlusTree::kInternalEntrySize;
}

PageId internalChild(const std::byte* page, std::size_t index) {
  if (index == 0) {
    return getU32(internalChild0(page));
  }
  return getU32(internalEntry(page, index - 1) + 1 + kMaxKeySize);
}

void setInternalChild(std::byte* page, std::size_t index, PageId id) {
  if (index == 0) {
    putU32(internalChild0(page), id);
    return;
  }
  putU32(internalEntry(page, index - 1) + 1 + kMaxKeySize, id);
}

int compareKeys(std::string_view a, std::string_view b) {
  const int ordering = a.compare(b);
  return ordering < 0 ? -1 : (ordering > 0 ? 1 : 0);
}

/// RAII pin. Every early return in this file would otherwise be a leaked frame,
/// and a leaked frame eventually exhausts the pool -- a bug that shows up far
/// from its cause.
class PageGuard {
 public:
  PageGuard(BufferPool& pool, PageId id, std::byte* data) : pool_(&pool), id_(id), data_(data) {}
  ~PageGuard() {
    if (pool_ != nullptr) {
      (void)pool_->unpin(id_, dirty_);
    }
  }
  PageGuard(const PageGuard&) = delete;
  PageGuard& operator=(const PageGuard&) = delete;
  PageGuard(PageGuard&& other) noexcept
      : pool_(other.pool_), id_(other.id_), data_(other.data_), dirty_(other.dirty_) {
    other.pool_ = nullptr;
  }
  PageGuard& operator=(PageGuard&&) = delete;

  [[nodiscard]] std::byte* data() const noexcept { return data_; }
  void markDirty() noexcept { dirty_ = true; }

 private:
  BufferPool* pool_;
  PageId id_;
  std::byte* data_;
  bool dirty_ = false;
};

Result<PageGuard> pin(BufferPool& pool, PageId id) {
  Result<std::byte*> page = pool.fetchPage(id);
  if (!page.ok()) {
    return page.status();
  }
  return PageGuard(pool, id, page.value());
}

/// First index whose key is >= `key`. Binary search, so a 59-entry leaf costs
/// six comparisons rather than a linear walk.
std::size_t lowerBoundInLeaf(const std::byte* page, std::string_view key) {
  std::size_t low = 0;
  std::size_t high = countOf(page);
  while (low < high) {
    const std::size_t mid = low + (high - low) / 2;
    if (compareKeys(entryKey(leafEntry(page, mid)), key) < 0) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return low;
}

/// Index of the child to descend into: the first separator strictly greater
/// than `key` selects the child to its left.
std::size_t childIndexFor(const std::byte* page, std::string_view key) {
  const std::size_t keys = countOf(page);
  std::size_t low = 0;
  std::size_t high = keys;
  while (low < high) {
    const std::size_t mid = low + (high - low) / 2;
    if (compareKeys(entryKey(internalEntry(page, mid)), key) <= 0) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return low;
}

}  // namespace

// ---------------------------------------------------------------------------
// Node creation
// ---------------------------------------------------------------------------

Result<PageId> BPlusTree::createLeaf() {
  Result<std::pair<PageId, std::byte*>> page = pool_.newPage();
  if (!page.ok()) {
    return page.status();
  }
  setType(page.value().second, PageType::kLeaf);
  setCount(page.value().second, 0);
  setNextLeaf(page.value().second, kInvalidPageId);
  BOURSE_TRY(pool_.unpin(page.value().first, true));
  return page.value().first;
}

Result<PageId> BPlusTree::createInternal() {
  Result<std::pair<PageId, std::byte*>> page = pool_.newPage();
  if (!page.ok()) {
    return page.status();
  }
  setType(page.value().second, PageType::kInternal);
  setCount(page.value().second, 0);
  BOURSE_TRY(pool_.unpin(page.value().first, true));
  return page.value().first;
}

// ---------------------------------------------------------------------------
// Descent
// ---------------------------------------------------------------------------

Result<PageId> BPlusTree::descendToLeaf(std::string_view key, std::vector<PageId>* path) {
  PageId current = disk_.rootPageId();
  if (current == kInvalidPageId) {
    return Status::notFound("tree is empty");
  }

  for (;;) {
    Result<PageGuard> guard = pin(pool_, current);
    if (!guard.ok()) {
      return guard.status();
    }
    const std::byte* page = guard.value().data();

    if (typeOf(page) == PageType::kLeaf) {
      return current;
    }
    if (typeOf(page) != PageType::kInternal) {
      return Status::corruption("page " + std::to_string(current) + " has an unexpected type");
    }

    if (path != nullptr) {
      path->push_back(current);
    }
    current = internalChild(page, childIndexFor(page, key));
    if (current == kInvalidPageId) {
      return Status::corruption("internal node points at an invalid child");
    }
  }
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------

Result<std::optional<RecordId>> BPlusTree::find(std::string_view key) {
  if (key.size() > kMaxKeySize) {
    return Status::invalidArgument("key exceeds the maximum index key size");
  }
  if (disk_.rootPageId() == kInvalidPageId) {
    return std::optional<RecordId>{};
  }

  Result<PageId> leaf_id = descendToLeaf(key, nullptr);
  if (!leaf_id.ok()) {
    return leaf_id.status();
  }
  Result<PageGuard> guard = pin(pool_, leaf_id.value());
  if (!guard.ok()) {
    return guard.status();
  }

  const std::byte* page = guard.value().data();
  const std::size_t index = lowerBoundInLeaf(page, key);
  if (index < countOf(page) && compareKeys(entryKey(leafEntry(page, index)), key) == 0) {
    return std::optional<RecordId>{leafValue(leafEntry(page, index))};
  }
  return std::optional<RecordId>{};
}

// ---------------------------------------------------------------------------
// Insert
// ---------------------------------------------------------------------------

Status BPlusTree::insert(std::string_view key, RecordId value) {
  if (key.empty()) {
    return Status::invalidArgument("index key must not be empty");
  }
  if (key.size() > kMaxKeySize) {
    // Truncating would make two distinct keys collide, which silently corrupts
    // the index. Refusing is the only safe answer.
    return Status::invalidArgument("key of " + std::to_string(key.size()) + " bytes exceeds the " +
                                   std::to_string(kMaxKeySize) + "-byte index key limit");
  }

  if (disk_.rootPageId() == kInvalidPageId) {
    Result<PageId> root = createLeaf();
    if (!root.ok()) {
      return root.status();
    }
    BOURSE_TRY(disk_.setRootPageId(root.value()));
  }

  std::vector<PageId> path;
  Result<PageId> leaf_id = descendToLeaf(key, &path);
  if (!leaf_id.ok()) {
    return leaf_id.status();
  }

  Split split;
  BOURSE_TRY(insertIntoLeaf(leaf_id.value(), key, value, &split));
  if (!split.happened) {
    return Status::success();
  }
  return insertIntoParent(path, path.size(), split.separator, split.right, &split);
}

Status BPlusTree::insertIntoLeaf(PageId leaf_id, std::string_view key, RecordId value, Split* split) {
  Result<PageGuard> guard = pin(pool_, leaf_id);
  if (!guard.ok()) {
    return guard.status();
  }
  std::byte* page = guard.value().data();
  guard.value().markDirty();

  const std::uint32_t count = countOf(page);
  const std::size_t index = lowerBoundInLeaf(page, key);

  // Overwrite in place when the key already exists.
  if (index < count && compareKeys(entryKey(leafEntry(page, index)), key) == 0) {
    setLeafValue(leafEntry(page, index), value);
    return Status::success();
  }

  if (count < kMaxLeafEntries) {
    std::memmove(leafEntry(page, index + 1), leafEntry(page, index), (count - index) * kLeafEntrySize);
    setEntryKey(leafEntry(page, index), key);
    setLeafValue(leafEntry(page, index), value);
    setCount(page, count + 1);
    return Status::success();
  }

  // ---- split ------------------------------------------------------------
  // Gather the full set, insert into it, then deal the halves out. Splitting
  // in place while also inserting is where off-by-ones live.
  std::vector<std::pair<std::string, RecordId>> entries;
  entries.reserve(count + 1);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::byte* entry = leafEntry(page, i);
    entries.emplace_back(std::string(entryKey(entry)), leafValue(entry));
  }
  entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(index), {std::string(key), value});

  const std::size_t left_count = entries.size() / 2;

  Result<PageId> right_id = createLeaf();
  if (!right_id.ok()) {
    return right_id.status();
  }
  Result<PageGuard> right_guard = pin(pool_, right_id.value());
  if (!right_guard.ok()) {
    return right_guard.status();
  }
  std::byte* right = right_guard.value().data();
  right_guard.value().markDirty();

  for (std::size_t i = 0; i < left_count; ++i) {
    setEntryKey(leafEntry(page, i), entries[i].first);
    setLeafValue(leafEntry(page, i), entries[i].second);
  }
  setCount(page, static_cast<std::uint32_t>(left_count));

  for (std::size_t i = left_count; i < entries.size(); ++i) {
    const std::size_t slot = i - left_count;
    setEntryKey(leafEntry(right, slot), entries[i].first);
    setLeafValue(leafEntry(right, slot), entries[i].second);
  }
  setCount(right, static_cast<std::uint32_t>(entries.size() - left_count));

  // Thread the new leaf into the chain so range scans still walk in order.
  setNextLeaf(right, nextLeafOf(page));
  setNextLeaf(page, right_id.value());

  split->happened = true;
  // In a B+ tree the separator is *copied* up, not moved: the key still lives
  // in the right leaf, because leaves hold every key.
  split->separator = entries[left_count].first;
  split->right = right_id.value();
  return Status::success();
}

Status BPlusTree::insertIntoParent(const std::vector<PageId>& path, std::size_t level,
                                   std::string_view separator, PageId right, Split* split) {
  if (level == 0) {
    // The root split: grow a new level. This is the only way a B+ tree gets
    // taller, and it is why every leaf stays at the same depth.
    Result<PageId> new_root = createInternal();
    if (!new_root.ok()) {
      return new_root.status();
    }
    Result<PageGuard> guard = pin(pool_, new_root.value());
    if (!guard.ok()) {
      return guard.status();
    }
    std::byte* page = guard.value().data();
    guard.value().markDirty();

    setInternalChild(page, 0, disk_.rootPageId());
    setEntryKey(internalEntry(page, 0), separator);
    setInternalChild(page, 1, right);
    setCount(page, 1);

    return disk_.setRootPageId(new_root.value());
  }

  const PageId parent_id = path[level - 1];
  Result<PageGuard> guard = pin(pool_, parent_id);
  if (!guard.ok()) {
    return guard.status();
  }
  std::byte* page = guard.value().data();
  guard.value().markDirty();

  const std::uint32_t keys = countOf(page);
  const std::size_t index = childIndexFor(page, separator);

  if (keys < kMaxInternalKeys) {
    // Shift keys and the children to their right up by one slot.
    for (std::size_t i = keys; i > index; --i) {
      std::memcpy(internalEntry(page, i), internalEntry(page, i - 1), kInternalEntrySize);
    }
    setEntryKey(internalEntry(page, index), separator);
    setInternalChild(page, index + 1, right);
    setCount(page, keys + 1);
    return Status::success();
  }

  // ---- split the internal node -----------------------------------------
  std::vector<std::string> node_keys;
  std::vector<PageId> children;
  node_keys.reserve(keys + 1);
  children.reserve(keys + 2);

  children.push_back(internalChild(page, 0));
  for (std::uint32_t i = 0; i < keys; ++i) {
    node_keys.emplace_back(entryKey(internalEntry(page, i)));
    children.push_back(internalChild(page, i + 1));
  }
  node_keys.insert(node_keys.begin() + static_cast<std::ptrdiff_t>(index), std::string(separator));
  children.insert(children.begin() + static_cast<std::ptrdiff_t>(index) + 1, right);

  const std::size_t middle = node_keys.size() / 2;
  // Unlike a leaf split, the middle key *moves* up rather than being copied:
  // internal nodes hold separators, not data, so keeping it here too would
  // duplicate it in the index.
  const std::string promoted = node_keys[middle];

  Result<PageId> sibling_id = createInternal();
  if (!sibling_id.ok()) {
    return sibling_id.status();
  }
  Result<PageGuard> sibling_guard = pin(pool_, sibling_id.value());
  if (!sibling_guard.ok()) {
    return sibling_guard.status();
  }
  std::byte* sibling = sibling_guard.value().data();
  sibling_guard.value().markDirty();

  setInternalChild(page, 0, children[0]);
  for (std::size_t i = 0; i < middle; ++i) {
    setEntryKey(internalEntry(page, i), node_keys[i]);
    setInternalChild(page, i + 1, children[i + 1]);
  }
  setCount(page, static_cast<std::uint32_t>(middle));

  setInternalChild(sibling, 0, children[middle + 1]);
  std::size_t slot = 0;
  for (std::size_t i = middle + 1; i < node_keys.size(); ++i) {
    setEntryKey(internalEntry(sibling, slot), node_keys[i]);
    setInternalChild(sibling, slot + 1, children[i + 1]);
    ++slot;
  }
  setCount(sibling, static_cast<std::uint32_t>(slot));

  return insertIntoParent(path, level - 1, promoted, sibling_id.value(), split);
}

// ---------------------------------------------------------------------------
// Erase
// ---------------------------------------------------------------------------

Result<bool> BPlusTree::erase(std::string_view key) {
  if (key.size() > kMaxKeySize || disk_.rootPageId() == kInvalidPageId) {
    return false;
  }

  std::vector<PageId> path;
  Result<PageId> leaf_id = descendToLeaf(key, &path);
  if (!leaf_id.ok()) {
    return leaf_id.status();
  }

  bool became_empty = false;
  {
    Result<PageGuard> guard = pin(pool_, leaf_id.value());
    if (!guard.ok()) {
      return guard.status();
    }
    std::byte* page = guard.value().data();

    const std::uint32_t count = countOf(page);
    const std::size_t index = lowerBoundInLeaf(page, key);
    if (index >= count || compareKeys(entryKey(leafEntry(page, index)), key) != 0) {
      return false;
    }

    guard.value().markDirty();
    std::memmove(leafEntry(page, index), leafEntry(page, index + 1),
                 (count - index - 1) * kLeafEntrySize);
    setCount(page, count - 1);
    became_empty = count - 1 == 0;
  }

  // An empty leaf is unlinked from its parent. Keys are not redistributed
  // between under-full siblings -- see the class comment.
  if (became_empty && !path.empty()) {
    BOURSE_TRY(removeFromParent(path, path.size(), leaf_id.value()));
  } else if (became_empty && path.empty()) {
    // The root leaf emptied: drop the tree so the next insert starts fresh
    // rather than descending into a zero-entry root.
    //
    // The PageGuard above has already unpinned by the time this runs -- an
    // explicit unpin here would be a second one, which the pool correctly
    // rejects and which would leave the frame's pin count negative.
    BOURSE_TRY(pool_.flushPage(leaf_id.value()));
    BOURSE_TRY(disk_.setRootPageId(kInvalidPageId));
    BOURSE_TRY(pool_.deletePage(leaf_id.value()));
    return true;
  }
  return true;
}

Status BPlusTree::removeFromParent(const std::vector<PageId>& path, std::size_t level, PageId child) {
  if (level == 0) {
    return Status::success();
  }

  const PageId parent_id = path[level - 1];
  bool parent_collapsed = false;

  {
    Result<PageGuard> guard = pin(pool_, parent_id);
    if (!guard.ok()) {
      return guard.status();
    }
    std::byte* page = guard.value().data();
    const std::uint32_t keys = countOf(page);

    std::size_t position = kMaxInternalKeys + 1;
    for (std::size_t i = 0; i <= keys; ++i) {
      if (internalChild(page, i) == child) {
        position = i;
        break;
      }
    }
    if (position > keys) {
      return Status::corruption("child page not found in its parent");
    }

    guard.value().markDirty();

    // Removing child i takes separator i-1 with it; removing child 0 takes
    // separator 0. Either way one key and one child leave together, which is
    // what keeps the (keys + 1 == children) invariant.
    if (position == 0) {
      setInternalChild(page, 0, internalChild(page, 1));
      for (std::size_t i = 0; i + 1 < keys; ++i) {
        std::memcpy(internalEntry(page, i), internalEntry(page, i + 1), kInternalEntrySize);
      }
    } else {
      for (std::size_t i = position - 1; i + 1 < keys; ++i) {
        std::memcpy(internalEntry(page, i), internalEntry(page, i + 1), kInternalEntrySize);
      }
    }
    setCount(page, keys - 1);
    parent_collapsed = keys - 1 == 0;
  }

  BOURSE_TRY(pool_.deletePage(child));

  if (!parent_collapsed) {
    return Status::success();
  }

  // The parent is down to a single child. If it is the root, that child becomes
  // the new root and the tree gets shorter -- the mirror image of a root split.
  if (level == 1) {
    PageId only_child = kInvalidPageId;
    {
      Result<PageGuard> guard = pin(pool_, parent_id);
      if (!guard.ok()) {
        return guard.status();
      }
      only_child = internalChild(guard.value().data(), 0);
    }  // unpin before freeing, or deletePage refuses
    BOURSE_TRY(disk_.setRootPageId(only_child));
    // Reclaim the old root. Leaving it allocated would slowly leak pages
    // across repeated grow/shrink cycles.
    BOURSE_TRY(pool_.deletePage(parent_id));
    return Status::success();
  }

  // A non-root node with one child and no keys is still correct -- every
  // descent through it goes to child 0 -- so it is left in place rather than
  // triggering a cascade.
  return Status::success();
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

Status BPlusTree::scan(std::string_view begin, std::string_view end,
                       const std::function<bool(std::string_view, RecordId)>& visit) {
  if (disk_.rootPageId() == kInvalidPageId) {
    return Status::success();
  }

  PageId leaf_id = kInvalidPageId;
  if (begin.empty()) {
    // Walk to the leftmost leaf.
    PageId current = disk_.rootPageId();
    for (;;) {
      Result<PageGuard> guard = pin(pool_, current);
      if (!guard.ok()) {
        return guard.status();
      }
      if (typeOf(guard.value().data()) == PageType::kLeaf) {
        leaf_id = current;
        break;
      }
      current = internalChild(guard.value().data(), 0);
    }
  } else {
    Result<PageId> found = descendToLeaf(begin, nullptr);
    if (!found.ok()) {
      return found.status();
    }
    leaf_id = found.value();
  }

  std::size_t index = 0;
  bool first_leaf = true;

  // The leaf chain is what makes this sequential: no re-descent per row.
  while (leaf_id != kInvalidPageId) {
    Result<PageGuard> guard = pin(pool_, leaf_id);
    if (!guard.ok()) {
      return guard.status();
    }
    const std::byte* page = guard.value().data();
    const std::uint32_t count = countOf(page);

    if (first_leaf && !begin.empty()) {
      index = lowerBoundInLeaf(page, begin);
      first_leaf = false;
    }

    for (std::size_t i = index; i < count; ++i) {
      const std::byte* entry = leafEntry(page, i);
      const std::string_view key = entryKey(entry);
      if (!end.empty() && compareKeys(key, end) >= 0) {
        return Status::success();
      }
      if (!visit(key, leafValue(entry))) {
        return Status::success();
      }
    }

    leaf_id = nextLeafOf(page);
    index = 0;
  }
  return Status::success();
}

Result<std::size_t> BPlusTree::size() {
  std::size_t total = 0;
  BOURSE_TRY(forEach([&](std::string_view, RecordId) {
    ++total;
    return true;
  }));
  return total;
}

Result<std::size_t> BPlusTree::height() {
  PageId current = disk_.rootPageId();
  if (current == kInvalidPageId) {
    return std::size_t{0};
  }
  std::size_t levels = 1;
  for (;;) {
    Result<PageGuard> guard = pin(pool_, current);
    if (!guard.ok()) {
      return guard.status();
    }
    if (typeOf(guard.value().data()) == PageType::kLeaf) {
      return levels;
    }
    current = internalChild(guard.value().data(), 0);
    ++levels;
  }
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

Status BPlusTree::validate() {
  if (disk_.rootPageId() == kInvalidPageId) {
    return Status::success();
  }
  int leaf_depth = -1;
  std::string previous_key;
  BOURSE_TRY(validateNode(disk_.rootPageId(), 0, &leaf_depth, &previous_key, {}, {}));

  // Independently walk the leaf chain and confirm it is globally sorted. A tree
  // can be locally consistent at every node and still have a mis-threaded leaf
  // chain, which only range scans would notice.
  std::string last;
  bool first = true;
  bool ordered = true;
  BOURSE_TRY(scan({}, {}, [&](std::string_view key, RecordId) {
    if (!first && compareKeys(last, key) >= 0) {
      ordered = false;
      return false;
    }
    last.assign(key);
    first = false;
    return true;
  }));
  if (!ordered) {
    return Status::corruption("leaf chain is not in ascending key order");
  }
  return Status::success();
}

Status BPlusTree::validateNode(PageId id, int depth, int* leaf_depth, std::string* previous_key,
                               std::string_view lower, std::string_view upper) {
  Result<PageGuard> guard = pin(pool_, id);
  if (!guard.ok()) {
    return guard.status();
  }
  const std::byte* page = guard.value().data();
  const std::uint32_t count = countOf(page);

  if (typeOf(page) == PageType::kLeaf) {
    if (*leaf_depth == -1) {
      *leaf_depth = depth;
    } else if (*leaf_depth != depth) {
      return Status::corruption("leaves are at differing depths: " + std::to_string(*leaf_depth) +
                                " and " + std::to_string(depth));
    }
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::string_view key = entryKey(leafEntry(page, i));
      if (i > 0 && compareKeys(entryKey(leafEntry(page, i - 1)), key) >= 0) {
        return Status::corruption("leaf keys are not sorted");
      }
      if (!lower.empty() && compareKeys(key, lower) < 0) {
        return Status::corruption("leaf key '" + std::string(key) + "' is below its separator");
      }
      if (!upper.empty() && compareKeys(key, upper) >= 0) {
        return Status::corruption("leaf key '" + std::string(key) + "' is at or above its separator");
      }
      previous_key->assign(key);
    }
    return Status::success();
  }

  if (typeOf(page) != PageType::kInternal) {
    return Status::corruption("page " + std::to_string(id) + " has an unexpected type");
  }

  for (std::uint32_t i = 1; i < count; ++i) {
    if (compareKeys(entryKey(internalEntry(page, i - 1)), entryKey(internalEntry(page, i))) >= 0) {
      return Status::corruption("internal separators are not sorted");
    }
  }

  for (std::uint32_t i = 0; i <= count; ++i) {
    const std::string_view child_lower =
        i == 0 ? lower : entryKey(internalEntry(page, i - 1));
    const std::string_view child_upper =
        i == count ? upper : entryKey(internalEntry(page, i));
    BOURSE_TRY(validateNode(internalChild(page, i), depth + 1, leaf_depth, previous_key, child_lower,
                            child_upper));
  }
  return Status::success();
}

}  // namespace bourse::storage
