#include "parquet_writer.h"
#include <spdlog/spdlog.h>

namespace charmvz {

ParquetWriter::ParquetWriter(std::shared_ptr<arrow::Schema> schema, const std::string& file_path)
    : schema_(std::move(schema)) {
  auto out_result = arrow::io::FileOutputStream::Open(file_path);
  if (!out_result.ok()) {
    spdlog::error("Failed to open output file {}: {}", file_path, out_result.status().ToString());
    throw std::runtime_error("Could not open file writer");
  }
  out_stream_ = *out_result;

  auto writer_result = parquet::arrow::FileWriter::Open(
      *schema_, arrow::default_memory_pool(), out_stream_);
  if (!writer_result.ok()) {
    spdlog::error("Failed to open parquet writer for {}: {}", file_path, writer_result.status().ToString());
    throw std::runtime_error("Could not create parquet file writer");
  }
  writer_ = std::move(*writer_result);
}

ParquetWriter::~ParquetWriter() {
  Close();
}

void ParquetWriter::WriteBatch(std::shared_ptr<arrow::RecordBatch> batch) {
  if (closed_) return;
  auto table_result = arrow::Table::FromRecordBatches({batch});
  if (!table_result.ok()) {
    spdlog::error("Failed to create table from batch: {}", table_result.status().ToString());
    return;
  }
  auto status = writer_->WriteTable(**table_result, batch->num_rows());
  if (!status.ok()) {
    spdlog::error("Failed to write batch to parquet: {}", status.ToString());
  }
}

void ParquetWriter::Close() {
  if (closed_) return;
  if (writer_) {
    auto status = writer_->Close();
    if (!status.ok()) {
      spdlog::error("Failed to close parquet writer: {}", status.ToString());
    }
  }
  if (out_stream_) {
    auto status = out_stream_->Close();
    if (!status.ok()) {
      spdlog::error("Failed to close output stream: {}", status.ToString());
    }
  }
  closed_ = true;
}

} // namespace charmvz
