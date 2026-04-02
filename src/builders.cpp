#include "builders.h"
#include "schema.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <limits>

namespace charmvz::builders {

ExecutionBuilder::ExecutionBuilder(ParquetWriter& writer, std::shared_ptr<arrow::Schema> schema, int32_t total_papi_events)
    : writer_(writer), schema_(std::move(schema)), total_papi_events_(std::clamp(total_papi_events, 0, static_cast<int32_t>(NUMPAPIEVENTS))) {}

void ExecutionBuilder::Append(const LogEntry& begin, const LogEntry& end, int32_t execution_pe_id, int64_t global_start_us, int64_t inst_id) {
    PARQUET_THROW_NOT_OK(pe_id.Append(execution_pe_id));
    PARQUET_THROW_NOT_OK(event.Append(begin.event));
    if (inst_id >= 0) {
        PARQUET_THROW_NOT_OK(instance_id.Append(inst_id));
    } else {
        PARQUET_THROW_NOT_OK(instance_id.AppendNull());
    }
    PARQUET_THROW_NOT_OK(ep_id.Append(begin.eIdx));
    PARQUET_THROW_NOT_OK(src_pe.Append(begin.pe));
    PARQUET_THROW_NOT_OK(msg_idx.Append(begin.mIdx));
    PARQUET_THROW_NOT_OK(msg_len.Append(begin.msglen));

    int64_t aligned_start = static_cast<int64_t>(begin.itime) - global_start_us;
    int64_t aligned_end = static_cast<int64_t>(end.itime) - global_start_us;
    PARQUET_THROW_NOT_OK(start_time_us.Append(aligned_start));
    if (begin.irecvtime == std::numeric_limits<uint64_t>::max()) {
        PARQUET_THROW_NOT_OK(recv_time_us.AppendNull());
    } else {
        PARQUET_THROW_NOT_OK(recv_time_us.Append(static_cast<int64_t>(begin.irecvtime) - global_start_us));
    }
    PARQUET_THROW_NOT_OK(start_cpu_us.Append(begin.icputime));

    PARQUET_THROW_NOT_OK(end_time_us.Append(aligned_end));
    PARQUET_THROW_NOT_OK(end_cpu_us.Append(end.icputime));

    auto append_papi_triplet = [&](arrow::Int64Builder& begin_builder, arrow::Int64Builder& end_builder, arrow::Int64Builder& delta_builder, size_t index) {
        if (static_cast<int32_t>(index) < total_papi_events_) {
            const int64_t begin_value = static_cast<int64_t>(begin.papiValues[index]);
            const int64_t end_value = static_cast<int64_t>(end.papiValues[index]);
            PARQUET_THROW_NOT_OK(begin_builder.Append(begin_value));
            PARQUET_THROW_NOT_OK(end_builder.Append(end_value));
            PARQUET_THROW_NOT_OK(delta_builder.Append(end_value - begin_value));
        } else {
            PARQUET_THROW_NOT_OK(begin_builder.AppendNull());
            PARQUET_THROW_NOT_OK(end_builder.AppendNull());
            PARQUET_THROW_NOT_OK(delta_builder.AppendNull());
        }
    };

    append_papi_triplet(papi_begin_0, papi_end_0, papi_delta_0, 0);
    append_papi_triplet(papi_begin_1, papi_end_1, papi_delta_1, 1);
    append_papi_triplet(papi_begin_2, papi_end_2, papi_delta_2, 2);
    append_papi_triplet(papi_begin_3, papi_end_3, papi_delta_3, 3);
    append_papi_triplet(papi_begin_4, papi_end_4, papi_delta_4, 4);
    append_papi_triplet(papi_begin_5, papi_end_5, papi_delta_5, 5);

    PARQUET_THROW_NOT_OK(wall_duration_us.Append(static_cast<int64_t>(end.itime) - static_cast<int64_t>(begin.itime)));
    PARQUET_THROW_NOT_OK(cpu_duration_us.Append(static_cast<int64_t>(end.icputime) - static_cast<int64_t>(begin.icputime)));
    if (begin.irecvtime == std::numeric_limits<uint64_t>::max()) {
        PARQUET_THROW_NOT_OK(queue_wait_us.AppendNull());
    } else {
        PARQUET_THROW_NOT_OK(queue_wait_us.Append(static_cast<int64_t>(begin.itime) - static_cast<int64_t>(begin.irecvtime)));
    }

    TryFlush();
}

void ExecutionBuilder::Flush() {
    if (pe_id.length() == 0) return;
    std::shared_ptr<arrow::Array> arr_pe_id, arr_event, arr_instance_id, arr_ep_id, arr_src_pe, arr_msg_idx, arr_msg_len;
    std::shared_ptr<arrow::Array> arr_start_time_us, arr_recv_time_us, arr_start_cpu_us, arr_end_time_us, arr_end_cpu_us;
    std::shared_ptr<arrow::Array> arr_pb0, arr_pb1, arr_pb2, arr_pb3, arr_pb4, arr_pb5;
    std::shared_ptr<arrow::Array> arr_pe0, arr_pe1, arr_pe2, arr_pe3, arr_pe4, arr_pe5;
    std::shared_ptr<arrow::Array> arr_wall, arr_cpu, arr_queue, arr_pd0, arr_pd1, arr_pd2, arr_pd3, arr_pd4, arr_pd5;
    
    PARQUET_THROW_NOT_OK(pe_id.Finish(&arr_pe_id));
    PARQUET_THROW_NOT_OK(event.Finish(&arr_event));
    PARQUET_THROW_NOT_OK(instance_id.Finish(&arr_instance_id));
    PARQUET_THROW_NOT_OK(ep_id.Finish(&arr_ep_id));
    PARQUET_THROW_NOT_OK(src_pe.Finish(&arr_src_pe));
    PARQUET_THROW_NOT_OK(msg_idx.Finish(&arr_msg_idx));
    PARQUET_THROW_NOT_OK(msg_len.Finish(&arr_msg_len));
    PARQUET_THROW_NOT_OK(start_time_us.Finish(&arr_start_time_us));
    PARQUET_THROW_NOT_OK(recv_time_us.Finish(&arr_recv_time_us));
    PARQUET_THROW_NOT_OK(start_cpu_us.Finish(&arr_start_cpu_us));
    PARQUET_THROW_NOT_OK(end_time_us.Finish(&arr_end_time_us));
    PARQUET_THROW_NOT_OK(end_cpu_us.Finish(&arr_end_cpu_us));
    
    PARQUET_THROW_NOT_OK(papi_begin_0.Finish(&arr_pb0));
    PARQUET_THROW_NOT_OK(papi_begin_1.Finish(&arr_pb1));
    PARQUET_THROW_NOT_OK(papi_begin_2.Finish(&arr_pb2));
    PARQUET_THROW_NOT_OK(papi_begin_3.Finish(&arr_pb3));
    PARQUET_THROW_NOT_OK(papi_begin_4.Finish(&arr_pb4));
    PARQUET_THROW_NOT_OK(papi_begin_5.Finish(&arr_pb5));
    
    PARQUET_THROW_NOT_OK(papi_end_0.Finish(&arr_pe0));
    PARQUET_THROW_NOT_OK(papi_end_1.Finish(&arr_pe1));
    PARQUET_THROW_NOT_OK(papi_end_2.Finish(&arr_pe2));
    PARQUET_THROW_NOT_OK(papi_end_3.Finish(&arr_pe3));
    PARQUET_THROW_NOT_OK(papi_end_4.Finish(&arr_pe4));
    PARQUET_THROW_NOT_OK(papi_end_5.Finish(&arr_pe5));

    PARQUET_THROW_NOT_OK(wall_duration_us.Finish(&arr_wall));
    PARQUET_THROW_NOT_OK(cpu_duration_us.Finish(&arr_cpu));
    PARQUET_THROW_NOT_OK(queue_wait_us.Finish(&arr_queue));

    PARQUET_THROW_NOT_OK(papi_delta_0.Finish(&arr_pd0));
    PARQUET_THROW_NOT_OK(papi_delta_1.Finish(&arr_pd1));
    PARQUET_THROW_NOT_OK(papi_delta_2.Finish(&arr_pd2));
    PARQUET_THROW_NOT_OK(papi_delta_3.Finish(&arr_pd3));
    PARQUET_THROW_NOT_OK(papi_delta_4.Finish(&arr_pd4));
    PARQUET_THROW_NOT_OK(papi_delta_5.Finish(&arr_pd5));

    std::vector<std::shared_ptr<arrow::Array>> arrays = {
        arr_pe_id, arr_event, arr_instance_id, arr_ep_id, arr_src_pe, arr_msg_idx, arr_msg_len,
        arr_start_time_us, arr_recv_time_us, arr_start_cpu_us, arr_end_time_us, arr_end_cpu_us,
        arr_pb0, arr_pb1, arr_pb2, arr_pb3, arr_pb4, arr_pb5,
        arr_pe0, arr_pe1, arr_pe2, arr_pe3, arr_pe4, arr_pe5,
        arr_wall, arr_cpu, arr_queue,
        arr_pd0, arr_pd1, arr_pd2, arr_pd3, arr_pd4, arr_pd5
    };

    auto batch = arrow::RecordBatch::Make(schema_, arr_pe_id->length(), arrays);
    writer_.WriteBatch(batch);
}

IdleIntervalBuilder::IdleIntervalBuilder(ParquetWriter& writer) 
    : writer_(writer), schema_(charmvz::schema::idle_interval()) {}

void IdleIntervalBuilder::Append(const LogEntry& begin, const LogEntry& end, int64_t global_start_us) {
    PARQUET_THROW_NOT_OK(pe_id.Append(begin.pe));
    int64_t start_align = begin.itime - global_start_us;
    int64_t end_align = end.itime - global_start_us;
    PARQUET_THROW_NOT_OK(start_time_us.Append(start_align));
    PARQUET_THROW_NOT_OK(end_time_us.Append(end_align));
    PARQUET_THROW_NOT_OK(duration_us.Append(end_align - start_align));
    TryFlush();
}

void IdleIntervalBuilder::Flush() {
    if (pe_id.length() == 0) return;
    std::shared_ptr<arrow::Array> arr_pe_id, arr_start, arr_end, arr_dur;
    PARQUET_THROW_NOT_OK(pe_id.Finish(&arr_pe_id));
    PARQUET_THROW_NOT_OK(start_time_us.Finish(&arr_start));
    PARQUET_THROW_NOT_OK(end_time_us.Finish(&arr_end));
    PARQUET_THROW_NOT_OK(duration_us.Finish(&arr_dur));
    
    auto batch = arrow::RecordBatch::Make(schema_, arr_pe_id->length(), {arr_pe_id, arr_start, arr_end, arr_dur});
    writer_.WriteBatch(batch);
}

ChareInstanceBuilder::ChareInstanceBuilder(ParquetWriter& writer) 
    : writer_(writer), schema_(charmvz::schema::chare_instance()) {}

void ChareInstanceBuilder::Append(const ChareInstanceRecord& instance) {
    PARQUET_THROW_NOT_OK(instance_id.Append(instance.instance_id));
    PARQUET_THROW_NOT_OK(collection_id.Append(instance.collection_id));
    PARQUET_THROW_NOT_OK(index_0.Append(instance.index_0));
    PARQUET_THROW_NOT_OK(index_1.Append(instance.index_1));
    PARQUET_THROW_NOT_OK(index_2.Append(instance.index_2));
    PARQUET_THROW_NOT_OK(index_3.Append(instance.index_3));
    TryFlush();
}

void ChareInstanceBuilder::Flush() {
    if (instance_id.length() == 0) return;
    std::shared_ptr<arrow::Array> arr_id, arr_cid, arr_i0, arr_i1, arr_i2, arr_i3;
    PARQUET_THROW_NOT_OK(instance_id.Finish(&arr_id));
    PARQUET_THROW_NOT_OK(collection_id.Finish(&arr_cid));
    PARQUET_THROW_NOT_OK(index_0.Finish(&arr_i0));
    PARQUET_THROW_NOT_OK(index_1.Finish(&arr_i1));
    PARQUET_THROW_NOT_OK(index_2.Finish(&arr_i2));
    PARQUET_THROW_NOT_OK(index_3.Finish(&arr_i3));
    
    auto batch = arrow::RecordBatch::Make(schema_, arr_id->length(), {arr_id, arr_cid, arr_i0, arr_i1, arr_i2, arr_i3});
    writer_.WriteBatch(batch);
}

} // namespace charmvz::builders
