#include "parquet_writer.h"
#include <spdlog/spdlog.h>

namespace charmvz {

ParquetWriter::ParquetWriter(std::shared_ptr<arrow::Schema> schema,
                             const std::string &file_path,
                             parquet::Compression::type compression)
    : schema_(std::move(schema)) {
  auto out_result = arrow::io::FileOutputStream::Open(file_path);
  if (!out_result.ok()) {
    spdlog::error("Failed to open output file {}: {}", file_path,
                  out_result.status().ToString());
    throw std::runtime_error("Could not open file writer");
  }
  out_stream_ = *out_result;

  // store_schema() serialises the Arrow schema into the file, which is the only
  // way schema-level key-value metadata survives the write. Arrow defaults it
  // off, and without it `schema::execution()`'s papi_event_N -> counter-name
  // mapping is silently dropped, leaving papi_delta_0..5 unidentifiable.
  auto arrow_props =
      parquet::ArrowWriterProperties::Builder().store_schema()->build();

  // Parquet's own default is Compression::UNCOMPRESSED; see
  // kDefaultCompression. The codec's default level is deliberate: on this data
  // ZSTD level 9 was measured at only 1% smaller than the default for
  // appreciably more CPU.
  auto writer_props =
      parquet::WriterProperties::Builder().compression(compression)->build();

  auto writer_result =
      parquet::arrow::FileWriter::Open(*schema_, arrow::default_memory_pool(),
                                       out_stream_, writer_props, arrow_props);
  if (!writer_result.ok()) {
    spdlog::error("Failed to open parquet writer for {}: {}", file_path,
                  writer_result.status().ToString());
    throw std::runtime_error("Could not create parquet file writer");
  }
  writer_ = std::move(*writer_result);
}

ParquetWriter::~ParquetWriter() { Close(); }

void ParquetWriter::WriteBatch(std::shared_ptr<arrow::RecordBatch> batch) {
  if (closed_)
    return;
  auto table_result = arrow::Table::FromRecordBatches({batch});
  if (!table_result.ok()) {
    spdlog::error("Failed to create table from batch: {}",
                  table_result.status().ToString());
    return;
  }
  auto status = writer_->WriteTable(**table_result, batch->num_rows());
  if (!status.ok()) {
    spdlog::error("Failed to write batch to parquet: {}", status.ToString());
  }
}

void ParquetWriter::Close() {
  if (closed_)
    return;
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
