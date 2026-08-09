#pragma once
#include "log_parser.h"
#include "parquet_writer.h"
#include "utils/log_entry.h"
#include <arrow/api.h>
#include <memory>

namespace charmvz::builders {

const int ROW_GROUP_SIZE = 100000;

class ExecutionBuilder {
public:
  ExecutionBuilder(ParquetWriter &writer, std::shared_ptr<arrow::Schema> schema,
                   int32_t total_papi_events);
  void Append(const LogEntry &begin, const LogEntry &end, int32_t pe_id,
              int64_t global_start_us, int64_t instance_id);
  void Flush();
  void TryFlush() {
    if (pe_id.length() >= ROW_GROUP_SIZE)
      Flush();
  }

private:
  ParquetWriter &writer_;
  std::shared_ptr<arrow::Schema> schema_;
  int32_t total_papi_events_;

  arrow::Int32Builder pe_id;
  arrow::Int32Builder event;
  arrow::Int64Builder instance_id;
  arrow::Int32Builder ep_id;
  arrow::Int32Builder src_pe;
  arrow::Int32Builder msg_idx;
  arrow::Int32Builder msg_len;
  arrow::Int64Builder start_time_us;
  arrow::Int64Builder recv_time_us;
  arrow::Int64Builder start_cpu_us;
  arrow::Int64Builder end_time_us;
  arrow::Int64Builder end_cpu_us;
  arrow::Int64Builder papi_begin_0;
  arrow::Int64Builder papi_begin_1;
  arrow::Int64Builder papi_begin_2;
  arrow::Int64Builder papi_begin_3;
  arrow::Int64Builder papi_begin_4;
  arrow::Int64Builder papi_begin_5;
  arrow::Int64Builder papi_end_0;
  arrow::Int64Builder papi_end_1;
  arrow::Int64Builder papi_end_2;
  arrow::Int64Builder papi_end_3;
  arrow::Int64Builder papi_end_4;
  arrow::Int64Builder papi_end_5;
  arrow::Int64Builder wall_duration_us;
  arrow::Int64Builder cpu_duration_us;
  arrow::Int64Builder queue_wait_us;
  arrow::Int64Builder papi_delta_0;
  arrow::Int64Builder papi_delta_1;
  arrow::Int64Builder papi_delta_2;
  arrow::Int64Builder papi_delta_3;
  arrow::Int64Builder papi_delta_4;
  arrow::Int64Builder papi_delta_5;
};

class IdleIntervalBuilder {
public:
  IdleIntervalBuilder(ParquetWriter &writer);
  void Append(const LogEntry &begin, const LogEntry &end,
              int64_t global_start_us);
  void Flush();
  void TryFlush() {
    if (pe_id.length() >= ROW_GROUP_SIZE)
      Flush();
  }

private:
  ParquetWriter &writer_;
  std::shared_ptr<arrow::Schema> schema_;

  arrow::Int32Builder pe_id;
  arrow::Int64Builder start_time_us;
  arrow::Int64Builder end_time_us;
  arrow::Int64Builder duration_us;
};

class ChareInstanceBuilder {
public:
  ChareInstanceBuilder(ParquetWriter &writer);
  void Append(const ChareInstanceRecord &instance);
  void Flush();
  void TryFlush() {
    if (instance_id.length() >= ROW_GROUP_SIZE)
      Flush();
  }

private:
  ParquetWriter &writer_;
  std::shared_ptr<arrow::Schema> schema_;

  arrow::Int64Builder instance_id;
  arrow::Int32Builder collection_id;
  arrow::Int32Builder index_0;
  arrow::Int32Builder index_1;
  arrow::Int32Builder index_2;
  arrow::Int32Builder index_3;
  arrow::Int32Builder index_4;
  arrow::Int32Builder index_5;
};

class UserEventBuilder {
public:
  UserEventBuilder(ParquetWriter &writer);
  void Append(const UserEventOccurrence &occurrence);
  void Flush();
  void TryFlush() {
    if (pe_id.length() >= ROW_GROUP_SIZE)
      Flush();
  }

private:
  ParquetWriter &writer_;
  std::shared_ptr<arrow::Schema> schema_;

  arrow::Int32Builder pe_id;
  arrow::Int32Builder record_type;
  arrow::Int32Builder user_event_id;
  arrow::StringBuilder name;
  arrow::Int32Builder event;
  arrow::Int32Builder nested_id;
  arrow::Int64Builder start_time_us;
  arrow::Int64Builder end_time_us;
  arrow::Int64Builder duration_us;
  arrow::Int32Builder user_supplied_int;
  arrow::StringBuilder note;
};

class UserStatBuilder {
public:
  UserStatBuilder(ParquetWriter &writer);
  void Append(const UserStatSample &sample);
  void Flush();
  void TryFlush() {
    if (pe_id.length() >= ROW_GROUP_SIZE)
      Flush();
  }

private:
  ParquetWriter &writer_;
  std::shared_ptr<arrow::Schema> schema_;

  arrow::Int32Builder pe_id;
  arrow::Int32Builder stat_id;
  arrow::StringBuilder name;
  arrow::Int64Builder time_us;
  arrow::DoubleBuilder stat_value;
  arrow::DoubleBuilder user_time_s;
};

class MemorySampleBuilder {
public:
  MemorySampleBuilder(ParquetWriter &writer);
  void Append(const MemorySample &sample);
  void Flush();
  void TryFlush() {
    if (pe_id.length() >= ROW_GROUP_SIZE)
      Flush();
  }

private:
  ParquetWriter &writer_;
  std::shared_ptr<arrow::Schema> schema_;

  arrow::Int32Builder pe_id;
  arrow::Int64Builder time_us;
  arrow::Int64Builder bytes;
};

} // namespace charmvz::builders
