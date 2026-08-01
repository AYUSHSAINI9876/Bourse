#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "bourse/storage/bplus_tree.hpp"
#include "bourse/storage/buffer_pool.hpp"

using namespace bourse;
using namespace bourse::storage;

namespace {

class TempFile {
 public:
  TempFile() {
    static std::atomic<int> counter{0};
    path_ = (std::filesystem::temp_directory_path() /
             ("bourse-btree-" + std::to_string(counter.fetch_add(1)) + "-" +
              std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".db"))
                .string();
  }
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
};

std::string keyOf(int n) {
  // Zero-padded so lexicographic order matches numeric order, which is what
  // makes the ordering assertions meaningful.
  std::string text = std::to_string(n);
  return std::string(8 - std::min<std::size_t>(8, text.size()), '0') + text;
}

}  // namespace

// ---------------------------------------------------------------------------
// DiskManager
// ---------------------------------------------------------------------------

TEST(DiskManagerTest, AllocatesAndPersistsTheHeader) {
  TempFile file;
  {
    Result<std::unique_ptr<DiskManager>> disk = DiskManager::open(file.path());
    ASSERT_TRUE(disk.ok()) << disk.status().toString();
    EXPECT_EQ(disk.value()->rootPageId(), kInvalidPageId);

    Result<PageId> first = disk.value()->allocatePage();
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value(), 1u) << "page 0 is reserved for the header";

    ASSERT_TRUE(disk.value()->setRootPageId(first.value()).ok());
    ASSERT_TRUE(disk.value()->sync().ok());
  }

  Result<std::unique_ptr<DiskManager>> reopened = DiskManager::open(file.path());
  ASSERT_TRUE(reopened.ok());
  EXPECT_EQ(reopened.value()->rootPageId(), 1u) << "the root id did not survive a reopen";
  EXPECT_EQ(reopened.value()->pageCount(), 2u);
}

TEST(DiskManagerTest, ReusesFreedPagesInsteadOfGrowing) {
  TempFile file;
  Result<std::unique_ptr<DiskManager>> disk = DiskManager::open(file.path());
  ASSERT_TRUE(disk.ok());

  std::vector<PageId> allocated;
  for (int i = 0; i < 5; ++i) {
    Result<PageId> page = disk.value()->allocatePage();
    ASSERT_TRUE(page.ok());
    allocated.push_back(page.value());
  }
  const std::uint32_t high_water = disk.value()->pageCount();

  for (PageId page : allocated) {
    ASSERT_TRUE(disk.value()->freePage(page).ok());
  }
  for (std::size_t i = 0; i < allocated.size(); ++i) {
    Result<PageId> page = disk.value()->allocatePage();
    ASSERT_TRUE(page.ok());
  }

  EXPECT_EQ(disk.value()->pageCount(), high_water)
      << "freed pages were not reused; the file grows without bound";
}

TEST(DiskManagerTest, RejectsAForeignFile) {
  TempFile file;
  ASSERT_TRUE(File::writeWholeFile(file.path(), std::string(kPageSize, 'x')).ok());
  Result<std::unique_ptr<DiskManager>> disk = DiskManager::open(file.path());
  ASSERT_FALSE(disk.ok());
  EXPECT_EQ(disk.status().code(), ErrorCode::kCorruption);
}

// ---------------------------------------------------------------------------
// BufferPool
// ---------------------------------------------------------------------------

TEST(BufferPoolTest, CachesAndCountsHits) {
  TempFile file;
  Result<std::unique_ptr<DiskManager>> disk = DiskManager::open(file.path());
  ASSERT_TRUE(disk.ok());
  BufferPool pool(*disk.value(), 4);

  Result<std::pair<PageId, std::byte*>> page = pool.newPage();
  ASSERT_TRUE(page.ok());
  const PageId id = page.value().first;
  ASSERT_TRUE(pool.unpin(id, true).ok());

  ASSERT_TRUE(pool.fetchPage(id).ok());
  ASSERT_TRUE(pool.unpin(id, false).ok());
  EXPECT_GE(pool.hits(), 1u);
}

TEST(BufferPoolTest, WritesBackDirtyPagesOnEviction) {
  TempFile file;
  Result<std::unique_ptr<DiskManager>> disk = DiskManager::open(file.path());
  ASSERT_TRUE(disk.ok());

  PageId target = kInvalidPageId;
  {
    BufferPool pool(*disk.value(), 2);  // tiny, so eviction is forced

    Result<std::pair<PageId, std::byte*>> first = pool.newPage();
    ASSERT_TRUE(first.ok());
    target = first.value().first;
    first.value().second[0] = std::byte{0xAB};
    ASSERT_TRUE(pool.unpin(target, /*dirty=*/true).ok());

    // Push it out by touching more pages than the pool can hold.
    for (int i = 0; i < 5; ++i) {
      Result<std::pair<PageId, std::byte*>> page = pool.newPage();
      ASSERT_TRUE(page.ok());
      ASSERT_TRUE(pool.unpin(page.value().first, false).ok());
    }
    EXPECT_GT(pool.evictions(), 0u);
    ASSERT_TRUE(pool.flushAll().ok());
  }

  // Read the byte straight off disk through a fresh pool.
  BufferPool verifier(*disk.value(), 2);
  Result<std::byte*> page = verifier.fetchPage(target);
  ASSERT_TRUE(page.ok());
  EXPECT_EQ(page.value()[0], std::byte{0xAB}) << "a dirty page was evicted without being written back";
  ASSERT_TRUE(verifier.unpin(target, false).ok());
}

TEST(BufferPoolTest, RefusesToEvictPinnedPages) {
  TempFile file;
  Result<std::unique_ptr<DiskManager>> disk = DiskManager::open(file.path());
  ASSERT_TRUE(disk.ok());
  BufferPool pool(*disk.value(), 2);

  // Pin both frames and keep them pinned.
  Result<std::pair<PageId, std::byte*>> first = pool.newPage();
  ASSERT_TRUE(first.ok());
  Result<std::pair<PageId, std::byte*>> second = pool.newPage();
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(pool.pinnedFrames(), 2u);

  // A third page has nowhere to go. Reporting exhaustion is the correct
  // behaviour -- evicting a pinned frame would corrupt a page someone holds.
  Result<std::pair<PageId, std::byte*>> third = pool.newPage();
  EXPECT_FALSE(third.ok());
  EXPECT_EQ(third.status().code(), ErrorCode::kOutOfMemory);

  ASSERT_TRUE(pool.unpin(first.value().first, false).ok());
  ASSERT_TRUE(pool.unpin(second.value().first, false).ok());
}

TEST(BufferPoolTest, DirtyFlagIsSticky) {
  TempFile file;
  Result<std::unique_ptr<DiskManager>> disk = DiskManager::open(file.path());
  ASSERT_TRUE(disk.ok());
  BufferPool pool(*disk.value(), 4);

  Result<std::pair<PageId, std::byte*>> page = pool.newPage();
  ASSERT_TRUE(page.ok());
  const PageId id = page.value().first;
  page.value().second[0] = std::byte{0x7F};
  ASSERT_TRUE(pool.unpin(id, /*dirty=*/true).ok());

  // A second reader unpinning clean must not undo the first writer's dirty
  // flag, or the change is silently lost.
  ASSERT_TRUE(pool.fetchPage(id).ok());
  ASSERT_TRUE(pool.unpin(id, /*dirty=*/false).ok());
  ASSERT_TRUE(pool.flushAll().ok());

  BufferPool verifier(*disk.value(), 2);
  Result<std::byte*> reloaded = verifier.fetchPage(id);
  ASSERT_TRUE(reloaded.ok());
  EXPECT_EQ(reloaded.value()[0], std::byte{0x7F});
  ASSERT_TRUE(verifier.unpin(id, false).ok());
}

TEST(BufferPoolTest, RejectsUnbalancedUnpin) {
  TempFile file;
  Result<std::unique_ptr<DiskManager>> disk = DiskManager::open(file.path());
  ASSERT_TRUE(disk.ok());
  BufferPool pool(*disk.value(), 4);

  Result<std::pair<PageId, std::byte*>> page = pool.newPage();
  ASSERT_TRUE(page.ok());
  ASSERT_TRUE(pool.unpin(page.value().first, false).ok());
  EXPECT_FALSE(pool.unpin(page.value().first, false).ok()) << "double unpin must be refused";
}

// ---------------------------------------------------------------------------
// B+ tree
// ---------------------------------------------------------------------------

namespace {

class BTreeFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    Result<std::unique_ptr<DiskManager>> disk = DiskManager::open(file_.path());
    ASSERT_TRUE(disk.ok()) << disk.status().toString();
    disk_ = std::move(disk).value();
    pool_ = std::make_unique<BufferPool>(*disk_, 64);
    tree_ = std::make_unique<BPlusTree>(*pool_, *disk_);
  }

  void expectValid() {
    const Status status = tree_->validate();
    EXPECT_TRUE(status.ok()) << "tree invariants broken: " << status.toString();
  }

  std::vector<std::string> collectKeys() {
    std::vector<std::string> keys;
    EXPECT_TRUE(tree_->forEach([&](std::string_view key, RecordId) {
                       keys.emplace_back(key);
                       return true;
                     })
                    .ok());
    return keys;
  }

  TempFile file_;
  std::unique_ptr<DiskManager> disk_;
  std::unique_ptr<BufferPool> pool_;
  std::unique_ptr<BPlusTree> tree_;
};

}  // namespace

TEST_F(BTreeFixture, EmptyTreeFindsNothing) {
  EXPECT_TRUE(tree_->empty());
  Result<std::optional<RecordId>> found = tree_->find("anything");
  ASSERT_TRUE(found.ok());
  EXPECT_FALSE(found.value().has_value());
  EXPECT_EQ(tree_->size().value(), 0u);
}

TEST_F(BTreeFixture, InsertAndFind) {
  ASSERT_TRUE(tree_->insert("alpha", 1).ok());
  ASSERT_TRUE(tree_->insert("beta", 2).ok());
  ASSERT_TRUE(tree_->insert("gamma", 3).ok());
  expectValid();

  EXPECT_EQ(tree_->find("alpha").value().value(), 1u);
  EXPECT_EQ(tree_->find("beta").value().value(), 2u);
  EXPECT_EQ(tree_->find("gamma").value().value(), 3u);
  EXPECT_FALSE(tree_->find("delta").value().has_value());
  EXPECT_EQ(tree_->size().value(), 3u);
}

TEST_F(BTreeFixture, InsertOverwritesAnExistingKey) {
  ASSERT_TRUE(tree_->insert("k", 1).ok());
  ASSERT_TRUE(tree_->insert("k", 999).ok());
  EXPECT_EQ(tree_->find("k").value().value(), 999u);
  EXPECT_EQ(tree_->size().value(), 1u) << "overwrite must not duplicate the key";
}

TEST_F(BTreeFixture, RejectsOversizedAndEmptyKeys) {
  EXPECT_FALSE(tree_->insert("", 1).ok());
  // Truncating an oversized key would make two distinct keys collide, so it is
  // refused rather than silently accepted.
  EXPECT_FALSE(tree_->insert(std::string(kMaxKeySize + 1, 'x'), 1).ok());
  EXPECT_TRUE(tree_->insert(std::string(kMaxKeySize, 'x'), 1).ok());
}

TEST_F(BTreeFixture, SplitsLeavesAndGrowsTaller) {
  const int count = static_cast<int>(BPlusTree::kMaxLeafEntries) * 4;
  for (int i = 0; i < count; ++i) {
    ASSERT_TRUE(tree_->insert(keyOf(i), static_cast<RecordId>(i)).ok()) << "insert " << i;
  }
  expectValid();

  EXPECT_EQ(tree_->size().value(), static_cast<std::size_t>(count));
  EXPECT_GT(tree_->height().value(), 1u) << "enough inserts to overflow a leaf did not grow the tree";

  for (int i = 0; i < count; ++i) {
    Result<std::optional<RecordId>> found = tree_->find(keyOf(i));
    ASSERT_TRUE(found.ok());
    ASSERT_TRUE(found.value().has_value()) << "lost key " << i;
    EXPECT_EQ(found.value().value(), static_cast<RecordId>(i));
  }
}

TEST_F(BTreeFixture, KeepsEverythingSortedRegardlessOfInsertOrder) {
  // Random order is the case that exercises splits at every position.
  std::vector<int> order(1000);
  for (int i = 0; i < 1000; ++i) {
    order[static_cast<std::size_t>(i)] = i;
  }
  std::mt19937 rng(1234);
  std::shuffle(order.begin(), order.end(), rng);

  for (int value : order) {
    ASSERT_TRUE(tree_->insert(keyOf(value), static_cast<RecordId>(value)).ok());
  }
  expectValid();

  const std::vector<std::string> keys = collectKeys();
  ASSERT_EQ(keys.size(), 1000u);
  EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end())) << "leaf chain is not in key order";
  EXPECT_EQ(keys.front(), keyOf(0));
  EXPECT_EQ(keys.back(), keyOf(999));
}

TEST_F(BTreeFixture, DescendingInsertOrderAlsoWorks) {
  // The pathological case for a naive split: every insert lands at position 0.
  for (int i = 500; i >= 0; --i) {
    ASSERT_TRUE(tree_->insert(keyOf(i), static_cast<RecordId>(i)).ok());
  }
  expectValid();
  EXPECT_EQ(tree_->size().value(), 501u);
  // Bind to a local first: collectKeys() returns by value, so calling it twice
  // would compare iterators from two different vectors.
  const std::vector<std::string> keys = collectKeys();
  EXPECT_TRUE(std::is_sorted(keys.begin(), keys.end()));
  EXPECT_EQ(keys.size(), 501u);
}

TEST_F(BTreeFixture, RangeScanReturnsExactlyTheRange) {
  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(tree_->insert(keyOf(i), static_cast<RecordId>(i)).ok());
  }

  std::vector<std::string> range;
  ASSERT_TRUE(tree_->scan(keyOf(100), keyOf(200), [&](std::string_view key, RecordId) {
                    range.emplace_back(key);
                    return true;
                  })
                  .ok());

  ASSERT_EQ(range.size(), 100u) << "range is half-open: [100, 200)";
  EXPECT_EQ(range.front(), keyOf(100));
  EXPECT_EQ(range.back(), keyOf(199));
  EXPECT_TRUE(std::is_sorted(range.begin(), range.end()));
}

TEST_F(BTreeFixture, ScanStopsWhenTheVisitorSaysSo) {
  for (int i = 0; i < 500; ++i) {
    ASSERT_TRUE(tree_->insert(keyOf(i), static_cast<RecordId>(i)).ok());
  }
  int seen = 0;
  ASSERT_TRUE(tree_->forEach([&](std::string_view, RecordId) {
                    ++seen;
                    return seen < 10;
                  })
                  .ok());
  EXPECT_EQ(seen, 10) << "the scan ignored the visitor's early exit";
}

TEST_F(BTreeFixture, ScanFromAKeyThatDoesNotExist) {
  for (int i = 0; i < 100; i += 2) {
    ASSERT_TRUE(tree_->insert(keyOf(i), static_cast<RecordId>(i)).ok());
  }
  std::vector<std::string> range;
  ASSERT_TRUE(tree_->scan(keyOf(51), keyOf(60), [&](std::string_view key, RecordId) {
                    range.emplace_back(key);
                    return true;
                  })
                  .ok());
  ASSERT_FALSE(range.empty());
  EXPECT_EQ(range.front(), keyOf(52)) << "a scan from an absent key must start at the next present one";
}

TEST_F(BTreeFixture, EraseRemovesOnlyTheNamedKey) {
  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(tree_->insert(keyOf(i), static_cast<RecordId>(i)).ok());
  }

  Result<bool> erased = tree_->erase(keyOf(50));
  ASSERT_TRUE(erased.ok());
  EXPECT_TRUE(erased.value());
  expectValid();

  EXPECT_FALSE(tree_->find(keyOf(50)).value().has_value());
  EXPECT_TRUE(tree_->find(keyOf(49)).value().has_value());
  EXPECT_TRUE(tree_->find(keyOf(51)).value().has_value());
  EXPECT_EQ(tree_->size().value(), 199u);

  Result<bool> again = tree_->erase(keyOf(50));
  ASSERT_TRUE(again.ok());
  EXPECT_FALSE(again.value()) << "erasing an absent key must report false, not fail";
}

TEST_F(BTreeFixture, ErasingEverythingLeavesAUsableTree) {
  const int count = static_cast<int>(BPlusTree::kMaxLeafEntries) * 3;
  for (int i = 0; i < count; ++i) {
    ASSERT_TRUE(tree_->insert(keyOf(i), static_cast<RecordId>(i)).ok());
  }
  for (int i = 0; i < count; ++i) {
    Result<bool> erased = tree_->erase(keyOf(i));
    ASSERT_TRUE(erased.ok()) << "erase " << i << ": " << erased.status().toString();
    EXPECT_TRUE(erased.value()) << "erase " << i << " reported the key was absent";
  }
  expectValid();
  EXPECT_EQ(tree_->size().value(), 0u);

  // And it must still accept new data afterwards.
  ASSERT_TRUE(tree_->insert("fresh", 7).ok());
  EXPECT_EQ(tree_->find("fresh").value().value(), 7u);
  expectValid();
}

TEST_F(BTreeFixture, InterleavedInsertAndEraseStayConsistent) {
  std::mt19937 rng(99);
  std::vector<int> live;

  for (int round = 0; round < 3000; ++round) {
    if (live.empty() || (rng() % 3) != 0) {
      const int value = static_cast<int>(rng() % 5000);
      if (std::find(live.begin(), live.end(), value) == live.end()) {
        ASSERT_TRUE(tree_->insert(keyOf(value), static_cast<RecordId>(value)).ok());
        live.push_back(value);
      }
    } else {
      const std::size_t index = rng() % live.size();
      const int value = live[index];
      Result<bool> erased = tree_->erase(keyOf(value));
      ASSERT_TRUE(erased.ok());
      EXPECT_TRUE(erased.value());
      live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
    }
  }

  expectValid();
  EXPECT_EQ(tree_->size().value(), live.size());

  std::sort(live.begin(), live.end());
  std::vector<std::string> expected;
  expected.reserve(live.size());
  for (int value : live) {
    expected.push_back(keyOf(value));
  }
  EXPECT_EQ(collectKeys(), expected);
}

TEST_F(BTreeFixture, SurvivesAReopen) {
  const int count = static_cast<int>(BPlusTree::kMaxLeafEntries) * 3;
  for (int i = 0; i < count; ++i) {
    ASSERT_TRUE(tree_->insert(keyOf(i), static_cast<RecordId>(i * 10)).ok());
  }
  ASSERT_TRUE(pool_->flushAll().ok());

  // Drop everything and reopen from the file alone.
  tree_.reset();
  pool_.reset();
  disk_.reset();

  Result<std::unique_ptr<DiskManager>> disk = DiskManager::open(file_.path());
  ASSERT_TRUE(disk.ok());
  BufferPool pool(*disk.value(), 32);
  BPlusTree reopened(pool, *disk.value());

  EXPECT_TRUE(reopened.validate().ok());
  EXPECT_EQ(reopened.size().value(), static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    Result<std::optional<RecordId>> found = reopened.find(keyOf(i));
    ASSERT_TRUE(found.ok());
    ASSERT_TRUE(found.value().has_value()) << "key " << i << " did not survive the reopen";
    EXPECT_EQ(found.value().value(), static_cast<RecordId>(i * 10));
  }
}

TEST_F(BTreeFixture, FanOutKeepsTheTreeShallow) {
  // The point of storing values only in leaves: with ~62-way fan-out, ten
  // thousand keys should need very few levels, so a lookup is a handful of
  // page reads at worst.
  for (int i = 0; i < 10000; ++i) {
    ASSERT_TRUE(tree_->insert(keyOf(i), static_cast<RecordId>(i)).ok());
  }
  expectValid();
  const std::size_t levels = tree_->height().value();
  EXPECT_LE(levels, 4u) << "10k keys produced a tree " << levels << " levels deep";
  EXPECT_EQ(tree_->size().value(), 10000u);
}
