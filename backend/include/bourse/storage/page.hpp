#pragma once

#include <cstddef>
#include <cstdint>

namespace bourse::storage {

using PageId = std::uint32_t;

/// Sentinel for "no page". Zero is a valid page id (the header page), so the
/// sentinel has to live at the other end of the range.
inline constexpr PageId kInvalidPageId = 0xFFFFFFFFu;

/// 4 KiB, matching the page size of every filesystem this will run on.
/// A page that straddles two filesystem pages doubles the I/O for a single
/// read and makes a torn write possible where none should be.
inline constexpr std::size_t kPageSize = 4096;

/// Page 0 is reserved for the file header: magic, version, root page id and
/// the head of the free list.
inline constexpr PageId kHeaderPageId = 0;

enum class PageType : std::uint8_t {
  kInvalid = 0,
  kHeader = 1,
  kInternal = 2,
  kLeaf = 3,
  kFree = 4,
};

/// Keys are fixed-width and inline.
///
/// Variable-length keys would need a slotted page layout with an indirection
/// array, node splits that repack, and a fallback for keys larger than a page.
/// Fixing the width makes every node an array -- binary search is a subscript,
/// a split is a memcpy of half the entries, and there is no fragmentation to
/// compact. Real systems do this too, storing a fixed-width prefix in the index
/// and the full key in the heap; the trade is documented rather than hidden.
inline constexpr std::size_t kMaxKeySize = 60;

}  // namespace bourse::storage
