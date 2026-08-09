#pragma once

// Shared fixtures for the tests that drive the pipeline end to end: a
// throwaway trace directory to write records into, and a reader that asserts
// against the Parquet file that was actually produced rather than an in-memory
// intermediate.

#include <catch2/catch_test_macros.hpp>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include <cstdint>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace charmvz::test {

// An STS file, one log file per PE, and a place for the pipeline to write its
// Parquet output. Removed on scope exit.
class TempTrace {
public:
  explicit TempTrace(const std::string &sts_contents) {
    // The pid is part of the name because meson runs the test binaries in
    // parallel and they share this counter only within a process. Without it,
    // two suites racing on "charmvz_log_test_0" delete each other's fixtures.
    root_ = std::filesystem::temp_directory_path() /
            ("charmvz_log_test_" + std::to_string(::getpid()) + "_" +
             std::to_string(counter_++));
    std::filesystem::create_directories(root_ / "logs");
    std::filesystem::create_directories(root_ / "out");
    std::ofstream sts(root_ / "logs" / "test.sts");
    sts << sts_contents;
  }

  ~TempTrace() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  TempTrace(const TempTrace &) = delete;
  auto operator=(const TempTrace &) -> TempTrace & = delete;

  // Writes one PE's log. The first line is the record-count header that
  // process_logs skips, so callers pass only the records themselves.
  void add_log(int pe, const std::string &records) {
    auto path = root_ / "logs" / ("test." + std::to_string(pe) + ".log");
    std::ofstream out(path);
    out << "PROJECTIONS-RECORD 0\n" << records;
    log_paths_.push_back(path.string());
  }

  [[nodiscard]] auto sts_path() const -> std::string {
    return (root_ / "logs" / "test.sts").string();
  }
  [[nodiscard]] auto log_paths() const -> const std::vector<std::string> & {
    return log_paths_;
  }
  [[nodiscard]] auto out_dir() const -> std::string {
    return (root_ / "out").string();
  }

private:
  std::filesystem::path root_;
  std::vector<std::string> log_paths_;
  static inline int counter_ = 0;
};

// Reads a Parquet file into a column-name -> values map, with std::nullopt for
// NULL.
class ParquetTable {
public:
  explicit ParquetTable(const std::string &path) {
    auto infile = arrow::io::ReadableFile::Open(path).ValueOrDie();
    auto reader = parquet::arrow::OpenFile(infile, arrow::default_memory_pool())
                      .ValueOrDie();
    REQUIRE(reader->ReadTable(&table_).ok());
  }

  [[nodiscard]] auto rows() const -> int64_t { return table_->num_rows(); }

  [[nodiscard]] auto ints(const std::string &column) const
      -> std::vector<std::optional<int64_t>> {
    auto chunked = table_->GetColumnByName(column);
    REQUIRE(chunked != nullptr);
    std::vector<std::optional<int64_t>> values;
    for (const auto &chunk : chunked->chunks()) {
      for (int64_t i = 0; i < chunk->length(); ++i) {
        if (chunk->IsNull(i)) {
          values.emplace_back(std::nullopt);
          continue;
        }
        if (chunk->type_id() == arrow::Type::INT32) {
          values.emplace_back(
              std::static_pointer_cast<arrow::Int32Array>(chunk)->Value(i));
        } else {
          values.emplace_back(
              std::static_pointer_cast<arrow::Int64Array>(chunk)->Value(i));
        }
      }
    }
    return values;
  }

  [[nodiscard]] auto doubles(const std::string &column) const
      -> std::vector<std::optional<double>> {
    auto chunked = table_->GetColumnByName(column);
    REQUIRE(chunked != nullptr);
    std::vector<std::optional<double>> values;
    for (const auto &chunk : chunked->chunks()) {
      auto array = std::static_pointer_cast<arrow::DoubleArray>(chunk);
      for (int64_t i = 0; i < array->length(); ++i) {
        if (array->IsNull(i)) {
          values.emplace_back(std::nullopt);
        } else {
          values.emplace_back(array->Value(i));
        }
      }
    }
    return values;
  }

  [[nodiscard]] auto strings(const std::string &column) const
      -> std::vector<std::optional<std::string>> {
    auto chunked = table_->GetColumnByName(column);
    REQUIRE(chunked != nullptr);
    std::vector<std::optional<std::string>> values;
    for (const auto &chunk : chunked->chunks()) {
      auto array = std::static_pointer_cast<arrow::StringArray>(chunk);
      for (int64_t i = 0; i < array->length(); ++i) {
        if (array->IsNull(i)) {
          values.emplace_back(std::nullopt);
        } else {
          values.emplace_back(array->GetString(i));
        }
      }
    }
    return values;
  }

private:
  std::shared_ptr<arrow::Table> table_;
};

} // namespace charmvz::test
