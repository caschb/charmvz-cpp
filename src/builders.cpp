#include "builders.h"
#include "schema.h"
#include <spdlog/spdlog.h>

namespace charmvz::builders {

ExecutionBuilder::ExecutionBuilder(ParquetWriter& writer) 
    : writer_(writer), schema_(charmvz::schema::execution()) {}

void ExecutionBuilder::Append(const LogEntry& entry, int64_t global_start_us, int64_t inst_id, int64_t q_wait_us) {
    PARQUET_THROW_NOT_OK(pe_id.Append(entry.pe));
    PARQUET_THROW_NOT_OK(event.Append(entry.event));
    if (inst_id >= 0) {
        PARQUET_THROW_NOT_OK(instance_id.Append(inst_id));
    } else {
        PARQUET_THROW_NOT_OK(instance_id.AppendNull());
    }
    PARQUET_THROW_NOT_OK(ep_id.Append(entry.eIdx));
    // src_pe will be appended correctly via Message reconstruction, but Execution also uses log properties
    // In Charm++, we just put log data or default -1
    PARQUET_THROW_NOT_OK(src_pe.Append(-1));
    PARQUET_THROW_NOT_OK(msg_idx.Append(entry.mIdx));
    PARQUET_THROW_NOT_OK(msg_len.Append(entry.msglen));
    
    int64_t aligned_start = entry.itime - global_start_us;
    PARQUET_THROW_NOT_OK(start_time_us.Append(aligned_start));
    PARQUET_THROW_NOT_OK(recv_time_us.AppendNull());
    PARQUET_THROW_NOT_OK(start_cpu_us.Append(entry.icputime));
    
    PARQUET_THROW_NOT_OK(end_time_us.AppendNull());
    PARQUET_THROW_NOT_OK(end_cpu_us.AppendNull());
    PARQUET_THROW_NOT_OK(papi_begin_0.AppendNull());
    PARQUET_THROW_NOT_OK(papi_begin_1.AppendNull());
    PARQUET_THROW_NOT_OK(papi_begin_2.AppendNull());
    PARQUET_THROW_NOT_OK(papi_begin_3.AppendNull());
    PARQUET_THROW_NOT_OK(papi_begin_4.AppendNull());
    PARQUET_THROW_NOT_OK(papi_begin_5.AppendNull());
    PARQUET_THROW_NOT_OK(papi_end_0.AppendNull());
    PARQUET_THROW_NOT_OK(papi_end_1.AppendNull());
    PARQUET_THROW_NOT_OK(papi_end_2.AppendNull());
    PARQUET_THROW_NOT_OK(papi_end_3.AppendNull());
    PARQUET_THROW_NOT_OK(papi_end_4.AppendNull());
    PARQUET_THROW_NOT_OK(papi_end_5.AppendNull());
    PARQUET_THROW_NOT_OK(wall_duration_us.AppendNull());
    PARQUET_THROW_NOT_OK(cpu_duration_us.AppendNull());
    PARQUET_THROW_NOT_OK(queue_wait_us.Append(q_wait_us));
    PARQUET_THROW_NOT_OK(papi_delta_0.AppendNull());
    PARQUET_THROW_NOT_OK(papi_delta_1.AppendNull());
    PARQUET_THROW_NOT_OK(papi_delta_2.AppendNull());
    PARQUET_THROW_NOT_OK(papi_delta_3.AppendNull());
    PARQUET_THROW_NOT_OK(papi_delta_4.AppendNull());
    PARQUET_THROW_NOT_OK(papi_delta_5.AppendNull());

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
