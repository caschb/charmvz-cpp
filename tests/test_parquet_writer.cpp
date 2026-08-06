#include "parquet_writer.h"
#include "schema.h"

#include <catch2/catch_test_macros.hpp>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

// Removes the written Parquet file on scope exit.
class TempParquetPath {
public:
  TempParquetPath() {
    path_ = std::filesystem::temp_directory_path() /
            ("charmvz_writer_test_" + std::to_string(counter_++) + ".parquet");
  }

  ~TempParquetPath() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  TempParquetPath(const TempParquetPath &) = delete;
  auto operator=(const TempParquetPath &) -> TempParquetPath & = delete;

  [[nodiscard]] auto str() const -> std::string { return path_.string(); }

private:
  std::filesystem::path path_;
  static inline int counter_ = 0;
};

// Reads a Parquet file back and returns the Arrow schema it was written with.
auto read_schema(const std::string &path) -> std::shared_ptr<arrow::Schema> {
  auto infile = arrow::io::ReadableFile::Open(path).ValueOrDie();
  auto reader = parquet::arrow::OpenFile(infile, arrow::default_memory_pool())
                    .ValueOrDie();
  std::shared_ptr<arrow::Table> table;
  if (!reader->ReadTable(&table).ok()) {
    return nullptr;
  }
  return table->schema();
}

} // namespace

TEST_CASE("schema::execution attaches PAPI counter names as metadata",
          "[schema][papi]") {
  SECTION("names present") {
    const auto schema =
        charmvz::schema::execution({"PAPI_L2_TCM", "PAPI_L2_TCA"});
    REQUIRE(schema->metadata() != nullptr);
    CHECK(schema->metadata()->Get("papi_event_0").ValueOr("") == "PAPI_L2_TCM");
    CHECK(schema->metadata()->Get("papi_event_1").ValueOr("") == "PAPI_L2_TCA");

    // The columns stay numerically indexed; only the metadata names them.
    CHECK(schema->GetFieldIndex("papi_delta_0") >= 0);
    CHECK(schema->GetFieldIndex("PAPI_L2_TCM") == -1);
  }

  SECTION("no PAPI counters means no metadata") {
    // The LeanMD reference traces have no TOTAL_PAPI_EVENTS at all.
    const auto schema = charmvz::schema::execution({});
    CHECK((schema->metadata() == nullptr || schema->metadata()->size() == 0));
  }
}

TEST_CASE("ParquetWriter preserves schema key-value metadata",
          "[parquet_writer][papi][regression]") {
  // Arrow's ArrowWriterProperties defaults store_schema to false, in which case
  // schema-level metadata is silently dropped on write -- the counter names
  // reached the schema but never the file. Keep this test: the failure is
  // invisible in the data, since every column and row still round-trips.
  TempParquetPath out;

  auto schema = arrow::schema({arrow::field("value", arrow::int64(), false)});
  schema = schema->WithMetadata(std::make_shared<arrow::KeyValueMetadata>(
      std::vector<std::string>{"papi_event_0", "papi_event_1"},
      std::vector<std::string>{"PAPI_L2_TCM", "PAPI_L2_TCA"}));

  {
    charmvz::ParquetWriter writer(schema, out.str());
    arrow::Int64Builder values;
    REQUIRE(values.Append(1).ok());
    REQUIRE(values.Append(2).ok());
    std::shared_ptr<arrow::Array> array;
    REQUIRE(values.Finish(&array).ok());
    writer.WriteBatch(
        arrow::RecordBatch::Make(schema, array->length(), {array}));
  }

  const auto read_back = read_schema(out.str());
  REQUIRE(read_back != nullptr);
  REQUIRE(read_back->metadata() != nullptr);
  CHECK(read_back->metadata()->Get("papi_event_0").ValueOr("") ==
        "PAPI_L2_TCM");
  CHECK(read_back->metadata()->Get("papi_event_1").ValueOr("") ==
        "PAPI_L2_TCA");
}

TEST_CASE("ParquetWriter round-trips rows and column types",
          "[parquet_writer]") {
  TempParquetPath out;

  auto schema = arrow::schema({arrow::field("pe_id", arrow::int32(), false),
                               arrow::field("value", arrow::int64(), true)});

  {
    charmvz::ParquetWriter writer(schema, out.str());
    arrow::Int32Builder pe;
    arrow::Int64Builder value;
    REQUIRE(pe.Append(7).ok());
    REQUIRE(value.AppendNull().ok());
    REQUIRE(pe.Append(9).ok());
    REQUIRE(value.Append(42).ok());
    std::shared_ptr<arrow::Array> pe_array;
    std::shared_ptr<arrow::Array> value_array;
    REQUIRE(pe.Finish(&pe_array).ok());
    REQUIRE(value.Finish(&value_array).ok());
    writer.WriteBatch(arrow::RecordBatch::Make(schema, pe_array->length(),
                                               {pe_array, value_array}));
  }

  const auto read_back = read_schema(out.str());
  REQUIRE(read_back != nullptr);
  CHECK(read_back->field(0)->type()->Equals(arrow::int32()));
  CHECK(read_back->field(1)->type()->Equals(arrow::int64()));
  CHECK(read_back->field(1)->nullable());
}
