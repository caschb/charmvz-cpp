#include "reconstruction.h"
#include "schema.h"
#include "parquet_writer.h"
#include <arrow/builder.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <limits>
#include <map>

namespace charmvz {

void reconstruct_message_and_migration(
    const LogParserResult& log_data,
    const StsData& sts_data,
    const RcData& rc_data,
    const std::string& output_dir
) {
    spdlog::info("Starting Stage 3 reconstruction message and migrations");

    // ProcessingElements
    ParquetWriter pe_writer(charmvz::schema::processing_element(), output_dir + "/processing_element.parquet");
    arrow::Int32Builder pe_pe_id, pe_total_pes;
    arrow::Int64Builder pe_begin, pe_end, pe_global, pe_dur, pe_align;

    for (const auto& pe : log_data.pes) {
        PARQUET_THROW_NOT_OK(pe_pe_id.Append(pe.pe_id));
        PARQUET_THROW_NOT_OK(pe_total_pes.Append(pe.total_pes));
        PARQUET_THROW_NOT_OK(pe_begin.Append(pe.begin_time_us - rc_data.global_start_time_us));
        if (pe.end_time_us > 0) {
            PARQUET_THROW_NOT_OK(pe_end.Append(pe.end_time_us - rc_data.global_start_time_us));
            PARQUET_THROW_NOT_OK(pe_dur.Append(pe.end_time_us - pe.begin_time_us));
        } else {
            PARQUET_THROW_NOT_OK(pe_end.AppendNull());
            PARQUET_THROW_NOT_OK(pe_dur.AppendNull());
        }
        PARQUET_THROW_NOT_OK(pe_global.Append(pe.global_start_us));
        PARQUET_THROW_NOT_OK(pe_align.Append(pe.begin_time_us - pe.global_start_us));
    }
    if (pe_pe_id.length() > 0) {
        std::shared_ptr<arrow::Array> a_pe, a_total, a_b, a_e, a_g, a_d, a_a;
        PARQUET_THROW_NOT_OK(pe_pe_id.Finish(&a_pe));
        PARQUET_THROW_NOT_OK(pe_total_pes.Finish(&a_total));
        PARQUET_THROW_NOT_OK(pe_begin.Finish(&a_b));
        PARQUET_THROW_NOT_OK(pe_end.Finish(&a_e));
        PARQUET_THROW_NOT_OK(pe_global.Finish(&a_g));
        PARQUET_THROW_NOT_OK(pe_dur.Finish(&a_d));
        PARQUET_THROW_NOT_OK(pe_align.Finish(&a_a));
        auto batch = arrow::RecordBatch::Make(charmvz::schema::processing_element(), a_pe->length(), {a_pe, a_total, a_b, a_e, a_g, a_d, a_a});
        pe_writer.WriteBatch(batch);
    }
    
    // Messages
    ParquetWriter msg_writer(charmvz::schema::message(), output_dir + "/message.parquet");
    arrow::Int64Builder m_id, m_send, m_enq, m_recv, m_exec, m_s2e, m_e2e, m_end2end;
    arrow::Int32Builder m_src, m_evt, m_ep, m_idx, m_len, m_fan, m_dst;
    arrow::BooleanBuilder m_bcast;
    
    int64_t msg_count = 0;
    auto flush_msg = [&]() {
        if (m_id.length() == 0) return;
        std::vector<std::shared_ptr<arrow::Array>> arrs(16);
        PARQUET_THROW_NOT_OK(m_id.Finish(&arrs[0]));
        PARQUET_THROW_NOT_OK(m_src.Finish(&arrs[1]));
        PARQUET_THROW_NOT_OK(m_evt.Finish(&arrs[2]));
        PARQUET_THROW_NOT_OK(m_ep.Finish(&arrs[3]));
        PARQUET_THROW_NOT_OK(m_idx.Finish(&arrs[4]));
        PARQUET_THROW_NOT_OK(m_len.Finish(&arrs[5]));
        PARQUET_THROW_NOT_OK(m_send.Finish(&arrs[6]));
        PARQUET_THROW_NOT_OK(m_enq.Finish(&arrs[7]));
        PARQUET_THROW_NOT_OK(m_bcast.Finish(&arrs[8]));
        PARQUET_THROW_NOT_OK(m_fan.Finish(&arrs[9]));
        PARQUET_THROW_NOT_OK(m_dst.Finish(&arrs[10]));
        PARQUET_THROW_NOT_OK(m_recv.Finish(&arrs[11]));
        PARQUET_THROW_NOT_OK(m_exec.Finish(&arrs[12]));
        PARQUET_THROW_NOT_OK(m_s2e.Finish(&arrs[13]));
        PARQUET_THROW_NOT_OK(m_e2e.Finish(&arrs[14]));
        PARQUET_THROW_NOT_OK(m_end2end.Finish(&arrs[15]));
        auto batch = arrow::RecordBatch::Make(charmvz::schema::message(), arrs[0]->length(), arrs);
        msg_writer.WriteBatch(batch);
    };

    for (const auto& kv : log_data.creation_map) {
        msg_count++;
        auto src_pe = std::get<0>(kv.first);
        auto event = std::get<1>(kv.first);
        const auto& cr = kv.second;
        
        PARQUET_THROW_NOT_OK(m_id.Append(msg_count));
        PARQUET_THROW_NOT_OK(m_src.Append(src_pe));
        PARQUET_THROW_NOT_OK(m_evt.Append(event));
        PARQUET_THROW_NOT_OK(m_ep.Append(cr.ep_id));
        PARQUET_THROW_NOT_OK(m_idx.Append(cr.msg_idx));
        PARQUET_THROW_NOT_OK(m_len.Append(cr.msg_len));
        PARQUET_THROW_NOT_OK(m_send.Append(cr.send_time_us - rc_data.global_start_time_us));
        if (cr.enqueue_time_us > 0) {
            PARQUET_THROW_NOT_OK(m_enq.Append(cr.enqueue_time_us - rc_data.global_start_time_us));
        } else {
            PARQUET_THROW_NOT_OK(m_enq.AppendNull());
        }
        PARQUET_THROW_NOT_OK(m_bcast.Append(cr.is_broadcast));
        PARQUET_THROW_NOT_OK(m_fan.Append(cr.broadcast_fanout));
        
        auto bp_it = log_data.begin_processing_map.find(kv.first);
        if (bp_it != log_data.begin_processing_map.end()) {
            PARQUET_THROW_NOT_OK(m_dst.Append(bp_it->second.dst_pe));
            PARQUET_THROW_NOT_OK(m_recv.Append(bp_it->second.recv_time_us - rc_data.global_start_time_us));
            PARQUET_THROW_NOT_OK(m_exec.Append(bp_it->second.exec_start_time_us - rc_data.global_start_time_us));
            PARQUET_THROW_NOT_OK(m_s2e.AppendNull());
            PARQUET_THROW_NOT_OK(m_e2e.AppendNull());
            PARQUET_THROW_NOT_OK(m_end2end.AppendNull());
        } else {
            PARQUET_THROW_NOT_OK(m_dst.AppendNull());
            PARQUET_THROW_NOT_OK(m_recv.AppendNull());
            PARQUET_THROW_NOT_OK(m_exec.AppendNull());
            PARQUET_THROW_NOT_OK(m_s2e.AppendNull());
            PARQUET_THROW_NOT_OK(m_e2e.AppendNull());
            PARQUET_THROW_NOT_OK(m_end2end.AppendNull());
        }

        if (m_id.length() >= 100000) flush_msg();
    }
    flush_msg();
    
    // Migrations MVP (Rule 9 Episode Correlation)
    ParquetWriter mig_writer(charmvz::schema::migration_episode(), output_dir + "/migration_episode.parquet");
    spdlog::info("Writing MigrationEpisode.parquet");

    arrow::Int64Builder mig_id, p_start, p_end, u_start, u_end, inst, p_dur, u_dur, tot_mig, net_trans;
    arrow::Int32Builder mig_src, mig_dst;
    arrow::BooleanBuilder ambig;

    auto packs = log_data.packs;

    std::sort(packs.begin(), packs.end(), [](const MigrationPack& a, const MigrationPack& b) {
        return a.pack_start_us < b.pack_start_us;
    });

    std::multimap<int64_t, MigrationUnpack> available_unpacks;
    for (const auto& u : log_data.unpacks) {
        available_unpacks.insert({u.unpack_start_us, u});
    }

    int64_t migration_id = 0;

    for (const auto& pack : packs) {
        migration_id++;
        PARQUET_THROW_NOT_OK(mig_id.Append(migration_id));
        PARQUET_THROW_NOT_OK(mig_src.Append(pack.src_pe));
        PARQUET_THROW_NOT_OK(p_start.Append(pack.pack_start_us - rc_data.global_start_time_us));
        PARQUET_THROW_NOT_OK(p_end.Append(pack.pack_end_us - rc_data.global_start_time_us));
        
        auto up_it = available_unpacks.lower_bound(pack.pack_end_us);
        
        if (up_it != available_unpacks.end()) {
            const auto& up_ack = up_it->second;
            PARQUET_THROW_NOT_OK(mig_dst.Append(up_ack.dst_pe));
            PARQUET_THROW_NOT_OK(u_start.Append(up_ack.unpack_start_us - rc_data.global_start_time_us));
            PARQUET_THROW_NOT_OK(u_end.Append(up_ack.unpack_end_us - rc_data.global_start_time_us));
            PARQUET_THROW_NOT_OK(u_dur.Append(up_ack.unpack_end_us - up_ack.unpack_start_us));
            PARQUET_THROW_NOT_OK(net_trans.Append(up_ack.unpack_start_us - pack.pack_end_us));
            PARQUET_THROW_NOT_OK(tot_mig.Append(up_ack.unpack_end_us - pack.pack_start_us));
            
            available_unpacks.erase(up_it);
        } else {
            PARQUET_THROW_NOT_OK(mig_dst.AppendNull());
            PARQUET_THROW_NOT_OK(u_start.AppendNull());
            PARQUET_THROW_NOT_OK(u_end.AppendNull());
            PARQUET_THROW_NOT_OK(u_dur.AppendNull());
            PARQUET_THROW_NOT_OK(net_trans.AppendNull());
            PARQUET_THROW_NOT_OK(tot_mig.AppendNull());
        }
        PARQUET_THROW_NOT_OK(p_dur.Append(pack.pack_end_us - pack.pack_start_us));
        PARQUET_THROW_NOT_OK(inst.AppendNull()); // Inference optional
        PARQUET_THROW_NOT_OK(ambig.Append(true)); // Marked ambiguous since we infer purely by time proximity
    }

    if (mig_id.length() > 0) {
        std::vector<std::shared_ptr<arrow::Array>> arrays(13);
        PARQUET_THROW_NOT_OK(mig_id.Finish(&arrays[0]));
        PARQUET_THROW_NOT_OK(mig_src.Finish(&arrays[1]));
        PARQUET_THROW_NOT_OK(p_start.Finish(&arrays[2]));
        PARQUET_THROW_NOT_OK(p_end.Finish(&arrays[3]));
        PARQUET_THROW_NOT_OK(mig_dst.Finish(&arrays[4]));
        PARQUET_THROW_NOT_OK(u_start.Finish(&arrays[5]));
        PARQUET_THROW_NOT_OK(u_end.Finish(&arrays[6]));
        PARQUET_THROW_NOT_OK(inst.Finish(&arrays[7]));
        PARQUET_THROW_NOT_OK(ambig.Finish(&arrays[8]));
        PARQUET_THROW_NOT_OK(p_dur.Finish(&arrays[9]));
        PARQUET_THROW_NOT_OK(u_dur.Finish(&arrays[10]));
        PARQUET_THROW_NOT_OK(tot_mig.Finish(&arrays[11]));
        PARQUET_THROW_NOT_OK(net_trans.Finish(&arrays[12]));
        auto batch = arrow::RecordBatch::Make(charmvz::schema::migration_episode(), arrays[0]->length(), arrays);
        mig_writer.WriteBatch(batch);
    }
}

} // namespace charmvz
