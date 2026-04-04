#pragma once
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <memory>
#include <parquet/arrow/writer.h>
#include <string>

namespace charmvz {

class ParquetWriter {
public:
  ParquetWriter(std::shared_ptr<arrow::Schema> schema,
                const std::string &file_path);
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
