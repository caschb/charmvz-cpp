#pragma once

// External sorting for the event-proportional state Stage 2 retains. Each
// collection is pushed into an ExternalSorter; when a memory budget is set,
// a sorter's buffer can be sorted and written to disk as a run, and finish()
// merges the runs back into one stream in the sorter's canonical order. With
// no budget nothing is written and finish() sorts the buffer in memory: the
// consumers see the same stream either way.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace charmvz {

// Records are written field by field in host byte order. Runs never leave the
// machine that wrote them, but this keeps the format's stated byte order
// honest.
static_assert(std::endian::native == std::endian::little,
              "spill runs are defined as little-endian");

// Bytes a record owns beyond sizeof(T). Records with a heap part overload
// this in their own namespace; it is found by argument-dependent lookup.
template <class T> auto record_heap_bytes(const T & /*record*/) -> std::size_t {
  return 0;
}

// A uniquely named directory for one conversion's runs,
// `<root>/charmvz-spill-<pid>-<n>`. Removed with everything in it on
// destruction, including during exception unwinding, unless `keep` is set.
class SpillDirectory {
public:
  static constexpr const char *kPrefix = "charmvz-spill-";

  // Throws std::runtime_error when the directory cannot be created or a file
  // cannot be written in it, so an unusable root fails before any parsing.
  SpillDirectory(const std::filesystem::path &root, bool keep);
  ~SpillDirectory();
  SpillDirectory(const SpillDirectory &) = delete;
  auto operator=(const SpillDirectory &) -> SpillDirectory & = delete;

  [[nodiscard]] auto path() const -> const std::filesystem::path & {
    return path_;
  }
  [[nodiscard]] auto keep() const -> bool { return keep_; }
  auto next_run_path() -> std::filesystem::path;

private:
  std::filesystem::path path_;
  bool keep_;
  uint64_t next_run_ = 0;
};

// Removes `charmvz-spill-<pid>-*` directories under `root` whose pid is not a
// running process: what a killed conversion leaves behind. Returns how many
// were removed.
auto remove_stale_spill_directories(const std::filesystem::path &root) -> int;

struct SpillStreamStats {
  int64_t runs_written = 0;
  uint64_t bytes_written = 0;
  int64_t merge_passes = 0;
};

struct SpillDiagnostics {
  SpillStreamStats creations;
  SpillStreamStats receivers;
  SpillStreamStats locations;
  // Times Stage 2 relieved memory pressure by spilling a buffer.
  int64_t spill_events = 0;
  // The largest the accounted memory got in Stage 2: spillable buffers plus
  // the Arrow pool plus the estimates for state that is never spilled.
  uint64_t peak_accounted_bytes = 0;
  uint64_t peak_arrow_bytes = 0;
  // Records in the largest receiver group Stage 3 held at once.
  uint64_t largest_receiver_group = 0;
  // The state that cannot be spilled alone exceeded the budget.
  bool floor_exceeded = false;

  [[nodiscard]] auto runs_written() const -> int64_t {
    return creations.runs_written + receivers.runs_written +
           locations.runs_written;
  }
  [[nodiscard]] auto bytes_written() const -> uint64_t {
    return creations.bytes_written + receivers.bytes_written +
           locations.bytes_written;
  }
};

void log_spill_summary(const SpillDiagnostics &diagnostics);

// Everything a sorter needs to spill: where runs go, how they are buffered
// and merged, and where to count what was done.
class SpillContext {
public:
  struct Settings {
    std::size_t io_buffer_bytes = std::size_t{1} << 20;
    // Runs merged at once. More runs than this cost a merge pass.
    std::size_t fan_in = 64;
    // Receiver groups larger than this are not held in memory. 0: no limit.
    uint64_t max_group_bytes = 0;
    // Test hook: every run write fails once this many bytes have been
    // written in total. 0: never.
    uint64_t fail_writes_after_bytes = 0;
  };

  SpillContext(std::unique_ptr<SpillDirectory> directory, Settings settings)
      : directory_(std::move(directory)), settings_(settings) {}

  [[nodiscard]] auto settings() const -> const Settings & { return settings_; }
  [[nodiscard]] auto can_spill() const -> bool { return directory_ != nullptr; }
  // Throws std::logic_error when no directory was set up.
  auto directory() -> SpillDirectory &;

  // Counts bytes about to be written and applies the fault hook.
  void account_write(std::size_t bytes);

  SpillDiagnostics diagnostics;

private:
  std::unique_ptr<SpillDirectory> directory_;
  Settings settings_;
  uint64_t bytes_written_ = 0;
};

// One run file: a 24-byte header (magic, format version, record type id,
// record count) followed by the records as their RunCodec wrote them.
class RunWriter {
public:
  RunWriter(std::filesystem::path path, uint32_t type_id,
            SpillContext &context);
  ~RunWriter();
  RunWriter(const RunWriter &) = delete;
  auto operator=(const RunWriter &) -> RunWriter & = delete;

  template <class V> void put(V value) {
    static_assert(std::is_trivially_copyable_v<V>);
    put_bytes(&value, sizeof value);
  }
  void put_bytes(const void *data, std::size_t size);
  void end_record() { ++count_; }
  // Flushes, writes the record count into the header and closes the file.
  // Returns the file's size.
  auto finish() -> uint64_t;

private:
  void flush();

  std::filesystem::path path_;
  SpillContext &context_;
  int fd_ = -1;
  std::vector<char> buffer_;
  std::size_t used_ = 0;
  uint64_t count_ = 0;
  uint64_t size_ = 0;
};

class RunReader {
public:
  // Throws std::runtime_error if the file is missing, its header is not a
  // run header of `type_id`, or it cannot be read.
  RunReader(std::filesystem::path path, uint32_t type_id,
            std::size_t buffer_bytes, bool remove_at_end);
  ~RunReader();
  RunReader(const RunReader &) = delete;
  auto operator=(const RunReader &) -> RunReader & = delete;

  [[nodiscard]] auto remaining() const -> uint64_t { return remaining_; }
  template <class V> auto get() -> V {
    static_assert(std::is_trivially_copyable_v<V>);
    V value;
    get_bytes(&value, sizeof value);
    return value;
  }
  // Throws std::runtime_error on a short read: the run was truncated.
  void get_bytes(void *data, std::size_t size);
  // Called once per record decoded. After the last one, checks that nothing
  // follows it, closes the file and, unless kept, deletes it, so disk is
  // reclaimed as the merge proceeds.
  void end_record();

private:
  auto fill() -> bool;
  void close();

  std::filesystem::path path_;
  bool remove_at_end_;
  int fd_ = -1;
  std::vector<char> buffer_;
  std::size_t pos_ = 0;
  std::size_t end_ = 0;
  uint64_t remaining_ = 0;
};

// How a record type is laid out in a run. Specialised next to each record:
//   static constexpr uint32_t type_id;
//   static void encode(RunWriter &, const T &);
//   static void decode(RunReader &, T &);
template <class T> struct RunCodec;

// Records in the canonical order of their sorter, consumed front to back:
// either a sorted vector, or a k-way merge of run files.
template <class T, class Less> class SortedStream {
public:
  using value_type = T;

  SortedStream() = default;
  explicit SortedStream(std::vector<T> sorted) : records_(std::move(sorted)) {}
  SortedStream(const std::vector<std::filesystem::path> &runs,
               std::size_t buffer_bytes, bool remove_at_end)
      : merging_(true) {
    for (const auto &run : runs) {
      auto source = std::make_unique<Source>(run, RunCodec<T>::type_id,
                                             buffer_bytes, remove_at_end);
      if (source->reader.remaining() == 0) {
        source->reader.end_record();
        continue;
      }
      RunCodec<T>::decode(source->reader, source->current);
      source->reader.end_record();
      sources_.push_back(std::move(source));
      heap_.push_back(sources_.size() - 1);
      std::push_heap(heap_.begin(), heap_.end(), heap_order());
    }
  }

  [[nodiscard]] auto done() const -> bool {
    return merging_ ? heap_.empty() : next_ >= records_.size();
  }
  [[nodiscard]] auto peek() const -> const T & {
    return merging_ ? sources_[heap_.front()]->current : records_[next_];
  }
  auto take() -> T {
    if (!merging_)
      return std::move(records_[next_++]);
    std::pop_heap(heap_.begin(), heap_.end(), heap_order());
    const std::size_t index = heap_.back();
    auto &source = *sources_[index];
    T record = std::move(source.current);
    if (source.reader.remaining() == 0) {
      heap_.pop_back();
      sources_[index].reset();
    } else {
      source.current = T{};
      RunCodec<T>::decode(source.reader, source.current);
      source.reader.end_record();
      std::push_heap(heap_.begin(), heap_.end(), heap_order());
    }
    return record;
  }

private:
  struct Source {
    Source(const std::filesystem::path &path, uint32_t type_id,
           std::size_t buffer_bytes, bool remove_at_end)
        : reader(path, type_id, buffer_bytes, remove_at_end) {}
    RunReader reader;
    T current{};
  };

  // std::*_heap keep the greatest element first, so "greater" is "comes
  // later". Ties go to the earlier run, though under a total order a tie is
  // two indistinguishable records.
  auto heap_order() const {
    return [this](std::size_t a, std::size_t b) {
      const T &x = sources_[a]->current;
      const T &y = sources_[b]->current;
      if (Less{}(y, x))
        return true;
      if (Less{}(x, y))
        return false;
      return a > b;
    };
  }

  std::vector<T> records_;
  std::size_t next_ = 0;
  bool merging_ = false;
  std::vector<std::unique_ptr<Source>> sources_;
  std::vector<std::size_t> heap_;
};

// Collects records in any order and hands them back sorted by `Less`, which
// must be a total order on everything the consumer observes: the output is
// then the same however the input was split into runs.
template <class T, class Less> class ExternalSorter {
public:
  void attach(SpillContext *context, SpillStreamStats *stats) {
    context_ = context;
    stats_ = stats;
  }

  void push(T record) {
    heap_bytes_ += record_heap_bytes(record);
    buffer_.push_back(std::move(record));
  }

  [[nodiscard]] auto size() const -> std::size_t { return buffer_.size(); }

  // What the buffered records cost, counted by capacity: the vector's spare
  // slots are as resident as its used ones.
  [[nodiscard]] auto footprint() const -> std::size_t {
    return (buffer_.capacity() * sizeof(T)) + heap_bytes_;
  }

  // Bytes the next push allocates: a full vector reallocates, and the old
  // and new blocks are both live while the elements move.
  [[nodiscard]] auto growth_bytes() const -> std::size_t {
    if (buffer_.size() < buffer_.capacity())
      return 0;
    return std::max<std::size_t>(1, 2 * buffer_.capacity()) * sizeof(T);
  }

  [[nodiscard]] auto has_runs() const -> bool { return !runs_.empty(); }

  // Sorts the buffer, writes it as one run and frees it.
  void spill() {
    if (buffer_.empty())
      return;
    std::sort(buffer_.begin(), buffer_.end(), Less{});
    write_run(buffer_);
    // Not `buffer_ = {}`: that is the initializer-list assignment, which
    // empties the vector but keeps its capacity, so nothing would be freed.
    std::vector<T>().swap(buffer_);
    heap_bytes_ = 0;
  }

  // Leaves the sorter empty. Once anything has been spilled the remainder is
  // spilled too, so the merge holds only its read buffers.
  auto finish() -> SortedStream<T, Less> {
    if (runs_.empty()) {
      std::sort(buffer_.begin(), buffer_.end(), Less{});
      heap_bytes_ = 0;
      return SortedStream<T, Less>(std::exchange(buffer_, {}));
    }
    spill();
    const auto &settings = context_->settings();
    const std::size_t fan_in = std::max<std::size_t>(2, settings.fan_in);
    while (runs_.size() > fan_in) {
      std::vector<std::filesystem::path> merged;
      for (std::size_t first = 0; first < runs_.size(); first += fan_in) {
        const std::size_t last = std::min(runs_.size(), first + fan_in);
        std::vector<std::filesystem::path> group(runs_.begin() + first,
                                                 runs_.begin() + last);
        SortedStream<T, Less> stream(group, settings.io_buffer_bytes,
                                     !context_->directory().keep());
        merged.push_back(write_stream(stream));
      }
      runs_ = std::move(merged);
      ++stats_->merge_passes;
    }
    return SortedStream<T, Less>(std::exchange(runs_, {}),
                                 settings.io_buffer_bytes,
                                 !context_->directory().keep());
  }

private:
  void write_run(const std::vector<T> &records) {
    auto path = context_->directory().next_run_path();
    RunWriter writer(path, RunCodec<T>::type_id, *context_);
    for (const auto &record : records) {
      RunCodec<T>::encode(writer, record);
      writer.end_record();
    }
    stats_->bytes_written += writer.finish();
    ++stats_->runs_written;
    runs_.push_back(std::move(path));
  }

  auto write_stream(SortedStream<T, Less> &stream) -> std::filesystem::path {
    auto path = context_->directory().next_run_path();
    RunWriter writer(path, RunCodec<T>::type_id, *context_);
    while (!stream.done()) {
      RunCodec<T>::encode(writer, stream.take());
      writer.end_record();
    }
    stats_->bytes_written += writer.finish();
    ++stats_->runs_written;
    return path;
  }

  std::vector<T> buffer_;
  std::size_t heap_bytes_ = 0;
  std::vector<std::filesystem::path> runs_;
  SpillContext *context_ = nullptr;
  SpillStreamStats *stats_ = nullptr;
};

// Walks a sorted stream one run of equal keys at a time, so a consumer holds
// a single group rather than the whole stream.
template <class Stream, class KeyOf> class GroupCursor {
public:
  using T = typename Stream::value_type;
  using Key = decltype(KeyOf{}(std::declval<const T &>()));

  explicit GroupCursor(Stream &stream) : stream_(stream) {}

  [[nodiscard]] auto done() const -> bool { return stream_.done(); }
  [[nodiscard]] auto key() const -> Key { return KeyOf{}(stream_.peek()); }

  // Replaces `group` with the current group, or with its first `limit`
  // records when it is longer; returns false in that case, leaving the rest
  // of the group unread.
  auto take(std::vector<T> &group,
            std::size_t limit = static_cast<std::size_t>(-1)) -> bool {
    group.clear();
    const Key current = key();
    while (!stream_.done() && KeyOf{}(stream_.peek()) == current) {
      if (group.size() == limit)
        return false;
      group.push_back(stream_.take());
    }
    return true;
  }

  // Discards the rest of the current group; returns how many records that
  // was.
  auto skip() -> std::size_t {
    const Key current = key();
    std::size_t skipped = 0;
    while (!stream_.done() && KeyOf{}(stream_.peek()) == current) {
      stream_.take();
      ++skipped;
    }
    return skipped;
  }

private:
  Stream &stream_;
};

} // namespace charmvz
