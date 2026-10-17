// The spill mechanism on its own: records written to run files and merged
// back must come out exactly as an in-memory sort would have produced them,
// and a run that cannot be read must abort the conversion without leaving
// the spill directory behind.

#include "log_parser.h"
#include "memory_budget.h"
#include "spill.h"
#include "sts_parser.h"
#include "trace_fixture.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

using charmvz::CreationOrder;
using charmvz::CreationRecord;
using charmvz::ExternalSorter;
using charmvz::SpillContext;
using charmvz::SpillDirectory;

// A scratch root for SpillDirectory, removed on scope exit.
struct ScratchRoot {
  ScratchRoot()
      : path(std::filesystem::temp_directory_path() /
             ("charmvz_spill_test_" + std::to_string(::getpid()) + "_" +
              std::to_string(counter++))) {
    std::filesystem::create_directories(path);
  }
  ~ScratchRoot() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  ScratchRoot(const ScratchRoot &) = delete;
  auto operator=(const ScratchRoot &) -> ScratchRoot & = delete;

  [[nodiscard]] auto entries() const -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> found;
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(path))
      found.push_back(entry.path());
    return found;
  }

  std::filesystem::path path;
  static inline int counter = 0;
};

auto context_in(const ScratchRoot &root) -> std::unique_ptr<SpillContext> {
  SpillContext::Settings settings;
  // Smaller than several records, so every record crosses a buffer refill.
  settings.io_buffer_bytes = 40;
  settings.fan_in = 2;
  return std::make_unique<SpillContext>(
      std::make_unique<SpillDirectory>(root.path, false), settings);
}

// Records sharing a (src_pe, event) pair are pushed in descending seq, so
// only the seq tie-break puts them back in read order; multicast lists range
// from empty to longer than the I/O buffer.
auto shuffled_creations() -> std::vector<CreationRecord> {
  std::vector<CreationRecord> records;
  for (int i = 0; i < 120; ++i) {
    CreationRecord r{};
    r.src_pe = (i * 7) % 3;
    r.event = (i * 5) % 11;
    r.ep_id = i;
    r.msg_idx = i % 4;
    r.msg_len = 64 + i;
    r.send_time_us = 1000 * i;
    r.send_to_enqueue_us = i % 5;
    r.is_broadcast = i % 9 == 0;
    r.broadcast_fanout = r.is_broadcast ? 3 : 0;
    r.seq = 1000 - i;
    for (int d = 0; d < i % 13; ++d)
      r.dst_pes.push_back((i + d) % 17);
    records.push_back(std::move(r));
  }
  return records;
}

auto same(const CreationRecord &a, const CreationRecord &b) -> bool {
  return a.src_pe == b.src_pe && a.event == b.event && a.ep_id == b.ep_id &&
         a.msg_idx == b.msg_idx && a.msg_len == b.msg_len &&
         a.send_time_us == b.send_time_us &&
         a.send_to_enqueue_us == b.send_to_enqueue_us &&
         a.is_broadcast == b.is_broadcast &&
         a.broadcast_fanout == b.broadcast_fanout && a.seq == b.seq &&
         a.dst_pes == b.dst_pes;
}

} // namespace

TEST_CASE("Spilled runs merge back into the canonical order",
          "[spill][sorter]") {
  ScratchRoot root;
  auto context = context_in(root);
  ExternalSorter<CreationRecord, CreationOrder> sorter;
  sorter.attach(context.get(), &context->diagnostics.creations);

  const auto input = shuffled_creations();
  for (size_t i = 0; i < input.size(); ++i) {
    sorter.push(input[i]);
    if (i % 7 == 6) {
      sorter.spill();
      // Spilling exists to give memory back; the budget loop relies on it.
      REQUIRE(sorter.footprint() == 0);
    }
  }

  auto expected = input;
  std::sort(expected.begin(), expected.end(), CreationOrder{});
  auto stream = sorter.finish();
  // 18 runs merged two at a time take several passes.
  CHECK(context->diagnostics.creations.runs_written > 18);
  CHECK(context->diagnostics.creations.merge_passes >= 4);

  size_t n = 0;
  while (!stream.done()) {
    const auto record = stream.take();
    REQUIRE(n < expected.size());
    INFO("record " << n);
    CHECK(same(record, expected[n]));
    ++n;
  }
  CHECK(n == expected.size());
  // Each run is deleted as soon as it has been read to the end.
  CHECK(root.entries().size() == 1);
}

TEST_CASE("A truncated run aborts the merge and the spill directory goes",
          "[spill][failure]") {
  ScratchRoot root;
  {
    auto context = context_in(root);
    ExternalSorter<CreationRecord, CreationOrder> sorter;
    sorter.attach(context.get(), &context->diagnostics.creations);
    const auto input = shuffled_creations();
    for (size_t i = 0; i < input.size(); ++i) {
      sorter.push(input[i]);
      if (i == 59)
        sorter.spill();
    }
    sorter.spill();

    const auto run = context->directory().path() / "run-0.bin";
    REQUIRE(std::filesystem::exists(run));
    std::filesystem::resize_file(run, std::filesystem::file_size(run) / 2);

    CHECK_THROWS_AS(
        [&] {
          auto stream = sorter.finish();
          while (!stream.done())
            stream.take();
        }(),
        std::runtime_error);
  }
  CHECK(root.entries().empty());
}

TEST_CASE("Memory budgets are read with binary suffixes", "[spill][budget]") {
  using charmvz::parse_size;
  CHECK(parse_size("0") == 0);
  CHECK(parse_size("1048576") == 1048576);
  CHECK(parse_size("512M") == uint64_t{512} << 20);
  CHECK(parse_size("4G") == uint64_t{4} << 30);
  CHECK(parse_size("4GiB") == uint64_t{4} << 30);
  CHECK(parse_size("256mb") == uint64_t{256} << 20);
  CHECK(parse_size("2k") == 2048);
  CHECK_THROWS_AS(parse_size(""), std::invalid_argument);
  CHECK_THROWS_AS(parse_size("G"), std::invalid_argument);
  CHECK_THROWS_AS(parse_size("1.5G"), std::invalid_argument);
  CHECK_THROWS_AS(parse_size("12X"), std::invalid_argument);
  CHECK_THROWS_AS(parse_size("99999999999999999999"), std::invalid_argument);
  CHECK_THROWS_AS(parse_size("20000000T"), std::invalid_argument);
}

namespace {

constexpr auto kSts = "PROJECTIONS_ID \n"
                      "VERSION 11.0\n"
                      "PROCESSORS 1\n"
                      "TOTAL_CHARES 1\n"
                      "CHARE 0 \"Array1D\" 1\n"
                      "ENTRY CHARE 11 \"one(dummyMsg*)\" 0 0\n"
                      "TOTAL_EVENTS 0\n"
                      "TOTAL_STATS 0\n"
                      "END\n";

auto stage2(const charmvz::test::TempTrace &trace,
            const charmvz::PipelineOptions &options) {
  const auto sts = charmvz::parse_sts_file(trace.sts_path());
  return charmvz::process_logs(trace.log_paths(), sts, charmvz::RcData{},
                               trace.out_dir(), charmvz::NO_STEP_EVENT,
                               options);
}

} // namespace

TEST_CASE("A failed spill write aborts and leaves no spill directory",
          "[spill][failure]") {
  charmvz::test::TempTrace trace(kSts);
  std::string records;
  for (int i = 0; i < 2000; ++i)
    records +=
        "1 0 11 " + std::to_string(i) + " " + std::to_string(i) + " 0 64 0\n";
  trace.add_log(0, records);

  ScratchRoot root;
  charmvz::PipelineOptions options;
  options.spill_root = root.path;
  options.spill_every_n_records = 100;
  options.fail_spill_writes_after_bytes = 10000;
  CHECK_THROWS_AS(stage2(trace, options), std::runtime_error);
  CHECK(root.entries().empty());
}

TEST_CASE("An unwritable spill root is refused before any parsing",
          "[spill][failure]") {
  if (::geteuid() == 0)
    SKIP("permission bits do not stop root");
  charmvz::test::TempTrace trace(kSts);
  trace.add_log(0, "1 0 11 100 1 0 64 0\n");

  ScratchRoot root;
  std::filesystem::permissions(root.path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_exec);
  charmvz::PipelineOptions options;
  options.spill_root = root.path;
  options.memory_budget = uint64_t{1} << 30;
  CHECK_THROWS_AS(stage2(trace, options), std::runtime_error);
  CHECK_FALSE(std::filesystem::exists(trace.out_dir() + "/execution.parquet"));
  std::filesystem::permissions(root.path, std::filesystem::perms::owner_all);
}
