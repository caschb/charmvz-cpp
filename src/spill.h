#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace charmvz {

// Bytes a record owns beyond sizeof(T). Records with a heap part overload
// this in their own namespace; it is found by argument-dependent lookup.
template <class T> auto record_heap_bytes(const T & /*record*/) -> std::size_t {
  return 0;
}

// Records in the canonical order of their sorter, consumed front to back.
template <class T> class SortedStream {
public:
  SortedStream() = default;
  explicit SortedStream(std::vector<T> sorted) : records_(std::move(sorted)) {}

  [[nodiscard]] auto done() const -> bool { return next_ >= records_.size(); }
  [[nodiscard]] auto peek() const -> const T & { return records_[next_]; }
  auto take() -> T { return std::move(records_[next_++]); }

private:
  std::vector<T> records_;
  std::size_t next_ = 0;
};

// Collects records in any order and hands them back sorted by `Less`, which
// must be a total order on everything the consumer observes: the output is
// then the same however the input was split up.
template <class T, class Less> class ExternalSorter {
public:
  void push(T record) {
    heap_bytes_ += record_heap_bytes(record);
    buffer_.push_back(std::move(record));
  }

  [[nodiscard]] auto size() const -> std::size_t { return buffer_.size(); }

  // What the retained records cost, counted by capacity: the vector's spare
  // slots are as resident as its used ones.
  [[nodiscard]] auto footprint() const -> std::size_t {
    return (buffer_.capacity() * sizeof(T)) + heap_bytes_;
  }

  // Leaves the sorter empty.
  auto finish() -> SortedStream<T> {
    std::sort(buffer_.begin(), buffer_.end(), Less{});
    heap_bytes_ = 0;
    return SortedStream<T>(std::exchange(buffer_, {}));
  }

private:
  std::vector<T> buffer_;
  std::size_t heap_bytes_ = 0;
};

// Walks a sorted stream one run of equal keys at a time, so a consumer holds
// a single group rather than the whole stream.
template <class T, class KeyOf> class GroupCursor {
public:
  using Key = decltype(KeyOf{}(std::declval<const T &>()));

  explicit GroupCursor(SortedStream<T> &stream) : stream_(stream) {}

  [[nodiscard]] auto done() const -> bool { return stream_.done(); }
  [[nodiscard]] auto key() const -> Key { return KeyOf{}(stream_.peek()); }

  void take(std::vector<T> &group) {
    group.clear();
    const Key current = key();
    while (!stream_.done() && KeyOf{}(stream_.peek()) == current)
      group.push_back(stream_.take());
  }

  void skip() {
    const Key current = key();
    while (!stream_.done() && KeyOf{}(stream_.peek()) == current)
      stream_.take();
  }

private:
  SortedStream<T> &stream_;
};

} // namespace charmvz
