#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGERANGEINDEX_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGERANGEINDEX_H_

// Experimental CPU-only indexes for KYTY-INDEX-001 (Mathlab issue #289).
// No Vulkan dependency. Production integration requires an independent oracle
// and an end-to-end GO decision. Guest ownership is registered at 1 MiB granularity;
// filtering uses exact bytes or 4 KiB tracker pages.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics {

struct ImageRangeRecord {
  uint32_t id = 0;
  uint32_t first_page = 0;
  uint32_t last_page = 0; // exclusive
  uint64_t registered_begin = 0;
  uint64_t registered_end = 0;
  uint64_t effective_begin = 0;
  uint64_t effective_end = 0;
  uint64_t sequence = 0;
};

namespace ImageRangeDetail {
constexpr uint64_t kLimit = uint64_t{1} << 44;
constexpr uint64_t kOwnershipPage = uint64_t{1} << 20;
constexpr uint64_t kTrackerPage = uint64_t{1} << 12;

inline bool Valid(uint64_t begin, uint64_t size) {
  return size != 0 && begin < kLimit && size <= kLimit - begin;
}
inline uint32_t FirstPage(uint64_t begin) {
  return static_cast<uint32_t>(begin / kOwnershipPage);
}
inline uint32_t LastPage(uint64_t end) {
  return static_cast<uint32_t>((end - 1) / kOwnershipPage + 1);
}
inline bool Matches(const ImageRangeRecord& record, uint64_t begin, uint64_t end,
                    bool page_overlap) {
  if (!page_overlap) {
    return record.effective_begin < end && begin < record.effective_end;
  }
  return record.effective_begin / kTrackerPage <= (end - 1) / kTrackerPage &&
         begin / kTrackerPage <= (record.effective_end - 1) / kTrackerPage;
}
inline bool Earlier(const ImageRangeRecord& a, const ImageRangeRecord& b) {
  return a.first_page < b.first_page ||
         (a.first_page == b.first_page && a.sequence < b.sequence);
}
struct Hit {
  uint32_t first_hit;
  uint64_t sequence;
  uint32_t id;
};
inline void AddHit(std::vector<Hit>& out, const ImageRangeRecord& record,
                   uint32_t query_first, uint32_t query_last,
                   uint64_t begin, uint64_t end, bool page_overlap) {
  if (record.first_page >= query_last || record.last_page <= query_first ||
      !Matches(record, begin, end, page_overlap)) {
    return;
  }
  out.push_back({std::max(record.first_page, query_first), record.sequence, record.id});
}
inline std::vector<uint32_t> Finish(std::vector<Hit>& hits) {
  std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
    return a.first_hit < b.first_hit ||
           (a.first_hit == b.first_hit && a.sequence < b.sequence);
  });
  std::vector<uint32_t> result;
  result.reserve(hits.size());
  for (const auto& hit : hits) {
    result.push_back(hit.id);
  }
  return result;
}
inline ImageRangeRecord MakeRecord(uint32_t id, uint64_t begin,
                                   uint64_t size, uint64_t sequence) {
  return {id, FirstPage(begin), LastPage(begin + size), begin, begin + size,
          begin, begin + size, sequence};
}
} // namespace ImageRangeDetail

// Dense sorted records, pruned using per-block maximum end page.
// Update complexity is O(N) due to contiguous insertion, erase and summaries;
// query worst case can be O(N), but offers cache-friendly sequential access.
class BlockedImageRangeIndex {
public:
  explicit BlockedImageRangeIndex(size_t block_size = 64)
      : m_block_size(std::max<size_t>(1, block_size)) {}

  bool Register(uint32_t id, uint64_t begin, uint64_t size) {
    if (!ImageRangeDetail::Valid(begin, size) || m_records.contains(id) ||
        m_next_sequence == std::numeric_limits<uint64_t>::max()) {
      return false;
    }
    const auto record = ImageRangeDetail::MakeRecord(id, begin, size, m_next_sequence++);
    m_records.emplace(id, record);
    auto it = std::lower_bound(m_order.begin(), m_order.end(), record,
                               [](const ImageRangeRecord& a, const ImageRangeRecord& b) {
                                 return ImageRangeDetail::Earlier(a, b);
                               });
    m_order.insert(it, record);
    Rebuild();
    return true;
  }

  bool Unregister(uint32_t id) {
    const auto found = m_records.find(id);
    if (found == m_records.end()) return false;
    const auto key = found->second;
    auto it = std::lower_bound(m_order.begin(), m_order.end(), key,
                               [](const ImageRangeRecord& a, const ImageRangeRecord& b) {
                                 return ImageRangeDetail::Earlier(a, b);
                               });
    if (it == m_order.end() || it->id != id) return false;
    m_order.erase(it);
    m_records.erase(found);
    Rebuild();
    return true;
  }

  // Deliberately modeled independently of the registered 1-MiB footprint:
  // legacy uses original page membership but mutable image.info.data for filtering.
  bool UpdateEffective(uint32_t id, uint64_t begin, uint64_t size) {
    if (!ImageRangeDetail::Valid(begin, size)) return false;
    const auto found = m_records.find(id);
    if (found == m_records.end()) return false;
    const auto key = found->second;
    auto it = std::lower_bound(m_order.begin(), m_order.end(), key,
                               [](const ImageRangeRecord& a, const ImageRangeRecord& b) {
                                 return ImageRangeDetail::Earlier(a, b);
                               });
    if (it == m_order.end() || it->id != id) return false;
    it->effective_begin = found->second.effective_begin = begin;
    it->effective_end = found->second.effective_end = begin + size;
    return true;
  }

  [[nodiscard]] std::vector<uint32_t> Find(uint64_t begin, uint64_t size,
                                            bool page_overlap = false,
                                            size_t* inspected = nullptr) const {
    if (inspected) *inspected = 0;
    if (!ImageRangeDetail::Valid(begin, size)) return {};
    const auto end = begin + size;
    const auto first = ImageRangeDetail::FirstPage(begin);
    const auto last = ImageRangeDetail::LastPage(end);
    std::vector<ImageRangeDetail::Hit> hits;
    for (size_t block = 0; block < m_max_end.size(); ++block) {
      const size_t from = block * m_block_size;
      if (m_order[from].first_page >= last) break;
      if (m_max_end[block] <= first) continue;
      const size_t to = std::min(m_order.size(), from + m_block_size);
      for (size_t i = from; i < to && m_order[i].first_page < last; ++i) {
        if (inspected) ++*inspected;
        ImageRangeDetail::AddHit(hits, m_order[i], first, last, begin, end, page_overlap);
      }
    }
    return ImageRangeDetail::Finish(hits);
  }

  [[nodiscard]] size_t Size() const { return m_order.size(); }
  [[nodiscard]] size_t StorageEstimate() const {
    return m_order.capacity() * sizeof(ImageRangeRecord) +
           m_max_end.capacity() * sizeof(uint32_t) +
           m_records.size() * (sizeof(ImageRangeRecord) + sizeof(uint32_t) + 2 * sizeof(void*));
  }
  [[nodiscard]] bool Validate() const {
    if (m_records.size() != m_order.size() ||
        m_max_end.size() != (m_order.size() + m_block_size - 1) / m_block_size) return false;
    for (size_t i = 0; i < m_order.size(); ++i) {
      if (i && !ImageRangeDetail::Earlier(m_order[i - 1], m_order[i])) return false;
      const auto it = m_records.find(m_order[i].id);
      if (it == m_records.end() || it->second.sequence != m_order[i].sequence ||
          it->second.effective_begin != m_order[i].effective_begin ||
          it->second.effective_end != m_order[i].effective_end) return false;
    }
    for (size_t block = 0; block < m_max_end.size(); ++block) {
      uint32_t maximum = 0;
      for (size_t i = block * m_block_size;
           i < std::min(m_order.size(), (block + 1) * m_block_size); ++i) {
        maximum = std::max(maximum, m_order[i].last_page);
      }
      if (maximum != m_max_end[block]) return false;
    }
    return true;
  }

private:
  void Rebuild() {
    m_max_end.clear();
    for (size_t i = 0; i < m_order.size(); i += m_block_size) {
      uint32_t maximum = 0;
      for (size_t j = i; j < std::min(m_order.size(), i + m_block_size); ++j) {
        maximum = std::max(maximum, m_order[j].last_page);
      }
      m_max_end.push_back(maximum);
    }
  }

  size_t m_block_size;
  uint64_t m_next_sequence = 1;
  std::unordered_map<uint32_t, ImageRangeRecord> m_records;
  std::vector<ImageRangeRecord> m_order;
  std::vector<uint32_t> m_max_end;
};

// Deterministic AVL tree, indexed by (first registered page, registration sequence).
// subtrees contain maximum registered last-page, allowing interval pruning.
class TreeImageRangeIndex {
public:
  bool Register(uint32_t id, uint64_t begin, uint64_t size) {
    if (!ImageRangeDetail::Valid(begin, size) || m_records.contains(id) ||
        m_next_sequence == std::numeric_limits<uint64_t>::max()) return false;
    const auto record = ImageRangeDetail::MakeRecord(id, begin, size, m_next_sequence++);
    m_records.emplace(id, record);
    Insert(m_root, record);
    return true;
  }
  bool Unregister(uint32_t id) {
    const auto it = m_records.find(id);
    if (it == m_records.end()) return false;
    const auto rec = it->second;
    if (!Erase(m_root, rec)) return false;
    m_records.erase(it);
    return true;
  }
  bool UpdateEffective(uint32_t id, uint64_t begin, uint64_t size) {
    if (!ImageRangeDetail::Valid(begin, size)) return false;
    auto it = m_records.find(id);
    if (it == m_records.end()) return false;
    auto* node = Locate(m_root.get(), it->second);
    if (!node) return false;
    node->record.effective_begin = it->second.effective_begin = begin;
    node->record.effective_end = it->second.effective_end = begin + size;
    return true;
  }
  [[nodiscard]] std::vector<uint32_t> Find(uint64_t begin, uint64_t size,
                                            bool page_overlap = false,
                                            size_t* inspected = nullptr) const {
    if (inspected) *inspected = 0;
    if (!ImageRangeDetail::Valid(begin, size)) return {};
    std::vector<ImageRangeDetail::Hit> hits;
    const auto first = ImageRangeDetail::FirstPage(begin);
    const auto last = ImageRangeDetail::LastPage(begin + size);
    Search(m_root.get(), first, last, begin, begin + size, page_overlap, hits, inspected);
    return ImageRangeDetail::Finish(hits);
  }
  [[nodiscard]] size_t Size() const { return m_records.size(); }
  [[nodiscard]] size_t StorageEstimate() const {
    return Size() * (sizeof(Node) + sizeof(ImageRangeRecord) +
                     sizeof(uint32_t) + 2 * sizeof(void*));
  }
  [[nodiscard]] bool Validate() const {
    const ImageRangeRecord* previous = nullptr;
    size_t count = 0;
    const bool ok = Check(m_root.get(), previous, count);
    return ok && count == Size();
  }

private:
  struct Node {
    explicit Node(ImageRangeRecord r) : record(r), max_last_page(r.last_page) {}
    ImageRangeRecord record;
    std::unique_ptr<Node> left, right;
    int height = 1;
    uint32_t max_last_page;
  };

  static int Height(const std::unique_ptr<Node>& node) { return node ? node->height : 0; }
  static uint32_t MaxEnd(const std::unique_ptr<Node>& node) {
    return node ? node->max_last_page : 0;
  }
  static void Refresh(Node& n) {
    n.height = 1 + std::max(Height(n.left), Height(n.right));
    n.max_last_page = std::max({n.record.last_page, MaxEnd(n.left), MaxEnd(n.right)});
  }
  static void RotateLeft(std::unique_ptr<Node>& node) {
    auto right = std::move(node->right);
    node->right = std::move(right->left);
    Refresh(*node);
    right->left = std::move(node);
    Refresh(*right);
    node = std::move(right);
  }
  static void RotateRight(std::unique_ptr<Node>& node) {
    auto left = std::move(node->left);
    node->left = std::move(left->right);
    Refresh(*node);
    left->right = std::move(node);
    Refresh(*left);
    node = std::move(left);
  }
  static void Balance(std::unique_ptr<Node>& node) {
    if (!node) return;
    Refresh(*node);
    if (Height(node->left) - Height(node->right) > 1) {
      if (Height(node->left->right) > Height(node->left->left)) RotateLeft(node->left);
      RotateRight(node);
    } else if (Height(node->right) - Height(node->left) > 1) {
      if (Height(node->right->left) > Height(node->right->right)) RotateRight(node->right);
      RotateLeft(node);
    }
  }
  static void Insert(std::unique_ptr<Node>& node, const ImageRangeRecord& rec) {
    if (!node) { node = std::make_unique<Node>(rec); return; }
    if (ImageRangeDetail::Earlier(rec, node->record)) Insert(node->left, rec);
    else Insert(node->right, rec);
    Balance(node);
  }
  static std::unique_ptr<Node> ExtractMin(std::unique_ptr<Node>& node) {
    if (!node->left) {
      auto result = std::move(node);
      node = std::move(result->right);
      result->right.reset();
      return result;
    }
    auto result = ExtractMin(node->left);
    Balance(node);
    return result;
  }
  static bool Erase(std::unique_ptr<Node>& node, const ImageRangeRecord& key) {
    if (!node) return false;
    bool erased = false;
    if (ImageRangeDetail::Earlier(key, node->record)) erased = Erase(node->left, key);
    else if (ImageRangeDetail::Earlier(node->record, key)) erased = Erase(node->right, key);
    else {
      erased = true;
      if (!node->left) { node = std::move(node->right); return true; }
      if (!node->right) { node = std::move(node->left); return true; }
      auto previous = std::move(node);
      auto replacement = ExtractMin(previous->right);
      replacement->left = std::move(previous->left);
      replacement->right = std::move(previous->right);
      node = std::move(replacement);
    }
    if (erased) Balance(node);
    return erased;
  }
  static Node* Locate(Node* node, const ImageRangeRecord& key) {
    while (node) {
      if (ImageRangeDetail::Earlier(key, node->record)) node = node->left.get();
      else if (ImageRangeDetail::Earlier(node->record, key)) node = node->right.get();
      else return node;
    }
    return nullptr;
  }
  static void Search(const Node* node, uint32_t first, uint32_t last,
                     uint64_t begin, uint64_t end, bool page_overlap,
                     std::vector<ImageRangeDetail::Hit>& hits, size_t* inspected) {
    if (!node || node->max_last_page <= first) return;
    Search(node->left.get(), first, last, begin, end, page_overlap, hits, inspected);
    if (node->record.first_page >= last) return;
    if (inspected) ++*inspected;
    ImageRangeDetail::AddHit(hits, node->record, first, last, begin, end, page_overlap);
    Search(node->right.get(), first, last, begin, end, page_overlap, hits, inspected);
  }
  static bool Check(const Node* node, const ImageRangeRecord*& prev, size_t& count) {
    if (!node) return true;
    if (!Check(node->left.get(), prev, count)) return false;
    if (prev && !ImageRangeDetail::Earlier(*prev, node->record)) return false;
    prev = &node->record; ++count;
    if (!Check(node->right.get(), prev, count)) return false;
    const int left = node->left ? node->left->height : 0;
    const int right = node->right ? node->right->height : 0;
    if (left - right > 1 || right - left > 1 ||
        node->height != 1 + std::max(left, right)) return false;
    const auto max_end = std::max({node->record.last_page, MaxEnd(node->left),
                                    MaxEnd(node->right)});
    return node->max_last_page == max_end;
  }
  std::unique_ptr<Node> m_root;
  std::unordered_map<uint32_t, ImageRangeRecord> m_records;
  uint64_t m_next_sequence = 1;
};

} // namespace Libs::Graphics
#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGERANGEINDEX_H_
