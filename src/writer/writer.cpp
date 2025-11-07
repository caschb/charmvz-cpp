#include "writer.h"
#include "src/utils/timeline.h"
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/status.h>
#include <arrow/type.h>
#include <arrow/type_fwd.h>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <parquet/arrow/writer.h>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

namespace {
auto CreateInt64Array(const std::vector<int64_t> &values,
                      std::shared_ptr<arrow::Array> *out_array) {
  arrow::Int64Builder builder{};
  ARROW_RETURN_NOT_OK(builder.Reserve(values.size()));
  ARROW_RETURN_NOT_OK(builder.AppendValues(values));

  return builder.Finish(out_array);
}

auto CreateStringArray(const std::vector<std::string> &values,
                       std::shared_ptr<arrow::Array> *out_array) {
  arrow::StringBuilder builder{};
  ARROW_RETURN_NOT_OK(builder.Reserve(values.size()));
  ARROW_RETURN_NOT_OK(builder.AppendValues(values));

  return builder.Finish(out_array);
}
} // namespace

// Writes the timeline as a parquet file
void write_timeline(const Timeline &timeline) {
  try {
    // Create the output filename
    std::string filename =
        "timeline_" + std::to_string(timeline.log_id) + ".parquet";

    spdlog::info("Writing timeline to parquet file: {}", filename);

    // Prepare data vectors for the main timeline events
    std::vector<int64_t> log_ids;
    std::vector<int64_t> event_indices;
    std::vector<int64_t> begin_times;
    std::vector<int64_t> end_times;
    std::vector<int64_t> recv_times;
    std::vector<int64_t> event_ids;
    std::vector<int64_t> pes;
    std::vector<int64_t> entry_points;
    std::vector<int64_t> message_sizes;
    std::vector<int64_t> cpu_begins;
    std::vector<int64_t> cpu_ends;
    std::vector<int64_t> mtypes;
    std::vector<std::string> user_event_names;
    std::vector<int64_t> message_counts;
    std::vector<int64_t> pack_time_counts;

    // Extract data from timeline events
    for (size_t i = 0; i < timeline.events.size(); ++i) {
      const auto &event = timeline.events[i];

      log_ids.push_back(timeline.log_id);
      event_indices.push_back(static_cast<int64_t>(i));
      begin_times.push_back(event.begin_time);
      end_times.push_back(event.end_time);
      recv_times.push_back(event.recv_time);
      event_ids.push_back(event.event_id);
      pes.push_back(event.pe);
      entry_points.push_back(event.entry_point);
      message_sizes.push_back(event.message_size);
      cpu_begins.push_back(event.cpu_begin);
      cpu_ends.push_back(event.cpu_end);
      mtypes.push_back(event.mtype);
      user_event_names.push_back(event.user_event_name);
      message_counts.push_back(static_cast<int64_t>(event.messages.size()));
      pack_time_counts.push_back(static_cast<int64_t>(event.pack_times.size()));
    }

    // Create Arrow arrays
    // Helper lambda to create int64 arrays and handle errors
    auto make_int64_array = [&](const std::vector<int64_t> &values,
                                std::shared_ptr<arrow::Array> &array,
                                const char *name) -> bool {
      auto status = CreateInt64Array(values, &array);
      if (!status.ok()) {
        spdlog::error("Failed to create {} array", name);
        return false;
      }
      return true;
    };

    std::shared_ptr<arrow::Array> log_id_array;
    std::shared_ptr<arrow::Array> event_index_array;
    std::shared_ptr<arrow::Array> begin_time_array;
    std::shared_ptr<arrow::Array> end_time_array;
    std::shared_ptr<arrow::Array> recv_time_array;
    std::shared_ptr<arrow::Array> event_id_array;
    std::shared_ptr<arrow::Array> pe_array;
    std::shared_ptr<arrow::Array> entry_point_array;
    std::shared_ptr<arrow::Array> message_size_array;
    std::shared_ptr<arrow::Array> cpu_begin_array;
    std::shared_ptr<arrow::Array> cpu_end_array;
    std::shared_ptr<arrow::Array> mtype_array;
    std::shared_ptr<arrow::Array> message_count_array;
    std::shared_ptr<arrow::Array> pack_time_count_array;

    if (!make_int64_array(log_ids, log_id_array, "log_id") ||
        !make_int64_array(event_indices, event_index_array, "event_index") ||
        !make_int64_array(begin_times, begin_time_array, "begin_time") ||
        !make_int64_array(end_times, end_time_array, "end_time") ||
        !make_int64_array(recv_times, recv_time_array, "recv_time") ||
        !make_int64_array(event_ids, event_id_array, "event_id") ||
        !make_int64_array(pes, pe_array, "pe") ||
        !make_int64_array(entry_points, entry_point_array, "entry_point") ||
        !make_int64_array(message_sizes, message_size_array, "message_size") ||
        !make_int64_array(cpu_begins, cpu_begin_array, "cpu_begin") ||
        !make_int64_array(cpu_ends, cpu_end_array, "cpu_end") ||
        !make_int64_array(mtypes, mtype_array, "mtype") ||
        !make_int64_array(message_counts, message_count_array,
                          "message_count") ||
        !make_int64_array(pack_time_counts, pack_time_count_array,
                          "pack_time_count")) {
      return;
    }

    std::shared_ptr<arrow::Array> user_event_name_array;
    auto status = CreateStringArray(user_event_names, &user_event_name_array);
    if (!status.ok()) {
      spdlog::error("Failed to create user_event_name array");
      return;
    }

    // Create schema
    auto schema =
        arrow::schema({arrow::field("log_id", arrow::int64()),
                       arrow::field("event_index", arrow::int64()),
                       arrow::field("begin_time", arrow::int64()),
                       arrow::field("end_time", arrow::int64()),
                       arrow::field("recv_time", arrow::int64()),
                       arrow::field("event_id", arrow::int64()),
                       arrow::field("pe", arrow::int64()),
                       arrow::field("entry_point", arrow::int64()),
                       arrow::field("message_size", arrow::int64()),
                       arrow::field("cpu_begin", arrow::int64()),
                       arrow::field("cpu_end", arrow::int64()),
                       arrow::field("mtype", arrow::int64()),
                       arrow::field("user_event_name", arrow::utf8()),
                       arrow::field("message_count", arrow::int64()),
                       arrow::field("pack_time_count", arrow::int64())});

    // Create record batch
    auto record_batch = arrow::RecordBatch::Make(
        schema, static_cast<int64_t>(timeline.events.size()),
        {log_id_array, event_index_array, begin_time_array, end_time_array,
         recv_time_array, event_id_array, pe_array, entry_point_array,
         message_size_array, cpu_begin_array, cpu_end_array, mtype_array,
         user_event_name_array, message_count_array, pack_time_count_array});

    // Create table
    const auto &table_result = arrow::Table::FromRecordBatches({record_batch});
    if (!table_result.ok()) {
      spdlog::error("Failed to create Arrow table: {}",
                    table_result.status().ToString());
      return;
    }
    auto table = *table_result;

    // Write to parquet file
    auto output_result = arrow::io::FileOutputStream::Open(filename);
    if (!output_result.ok()) {
      spdlog::error("Failed to open output file {}: {}", filename,
                    output_result.status().ToString());
      return;
    }
    const auto &output = *output_result;
    constexpr auto default_chunk_size = 1024;
    auto write_result = parquet::arrow::WriteTable(
        *table, arrow::default_memory_pool(), output, default_chunk_size);
    if (!write_result.ok()) {
      spdlog::error("Failed to write parquet file: {}",
                    write_result.ToString());
      return;
    }

    auto close_result = output->Close();
    if (!close_result.ok()) {
      spdlog::error("Failed to close output file: {}", close_result.ToString());
      return;
    }

    spdlog::info("Successfully wrote {} events to parquet file: {}",
                 timeline.events.size(), filename);

    // Log statistics
    spdlog::info("Timeline statistics - Total events processed: {}, "
                 "Timeline events created: {}, Events filtered by duration: {}",
                 timeline.total_events_processed,
                 timeline.timeline_events_created,
                 timeline.events_filtered_by_duration);

  } catch (const std::exception &e) {
    spdlog::error("Exception while writing timeline to parquet: {}", e.what());
  }
}
