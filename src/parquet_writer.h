#pragma once
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <memory>
#include <parquet/arrow/writer.h>
#include <string>

namespace charmvz {

// Parquet writes nothing compressed by default, which on trace data is a large
// and needless cost: the tables are wide runs of small integers and timestamps
// that compress heavily. ZSTD is chosen over Snappy for the better ratio at
// negligible extra write time; both are compiled into Arrow and readable by
// pyarrow/polars without extra configuration.
inline constexpr auto kDefaultCompression = parquet::Compression::ZSTD;

class ParquetWriter {
public:
  ParquetWriter(std::shared_ptr<arrow::Schema> schema,
                const std::string &file_path,
                parquet::Compression::type compression = kDefaultCompression);
  ~ParquetWriter();

  void WriteBatch(std::shared_ptr<arrow::RecordBatch> batch);
  void Close();

private:
  std::shared_ptr<arrow::Schema> schema_;
  std::shared_ptr<arrow::io::FileOutputStream> out_stream_;
  std::unique_ptr<parquet::arrow::FileWriter> writer_;
  bool closed_ = false;
};

} // namespace charmvz
