#include "metadata_tables.h"
#include "parquet_writer.h"
#include "schema.h"
#include <arrow/builder.h>

namespace charmvz {

void write_metadata_tables(const StsData &sts_data,
                           const std::string &output_dir) {
  {
    ParquetWriter chare_writer(schema::chare_collection(),
                               output_dir + "/chare_collection.parquet");
    arrow::Int32Builder c_id, ndims;
    arrow::StringBuilder c_name;
    for (const auto &c : sts_data.chares) {
      PARQUET_THROW_NOT_OK(c_id.Append(c.collection_id));
      PARQUET_THROW_NOT_OK(c_name.Append(c.name));
      PARQUET_THROW_NOT_OK(ndims.Append(c.ndims));
    }
    if (c_id.length() > 0) {
      std::shared_ptr<arrow::Array> a_cid, a_name, a_ndims;
      PARQUET_THROW_NOT_OK(c_id.Finish(&a_cid));
      PARQUET_THROW_NOT_OK(c_name.Finish(&a_name));
      PARQUET_THROW_NOT_OK(ndims.Finish(&a_ndims));
      chare_writer.WriteBatch(
          arrow::RecordBatch::Make(schema::chare_collection(), a_cid->length(),
                                   {a_cid, a_name, a_ndims}));
    }
  }
  {
    ParquetWriter ep_writer(schema::entry_method(),
                            output_dir + "/entry_method.parquet");
    arrow::Int32Builder ep_id, c_id_ep, msg_idx;
    arrow::StringBuilder ep_name;
    for (const auto &ep : sts_data.entries) {
      PARQUET_THROW_NOT_OK(ep_id.Append(ep.ep_id));
      PARQUET_THROW_NOT_OK(ep_name.Append(ep.name));
      PARQUET_THROW_NOT_OK(c_id_ep.Append(ep.collection_id));
      PARQUET_THROW_NOT_OK(msg_idx.Append(ep.msg_idx));
    }
    if (ep_id.length() > 0) {
      std::shared_ptr<arrow::Array> a_ep_id, a_ep_name, a_cid_ep, a_msg_idx;
      PARQUET_THROW_NOT_OK(ep_id.Finish(&a_ep_id));
      PARQUET_THROW_NOT_OK(ep_name.Finish(&a_ep_name));
      PARQUET_THROW_NOT_OK(c_id_ep.Finish(&a_cid_ep));
      PARQUET_THROW_NOT_OK(msg_idx.Finish(&a_msg_idx));
      ep_writer.WriteBatch(
          arrow::RecordBatch::Make(schema::entry_method(), a_ep_id->length(),
                                   {a_ep_id, a_ep_name, a_cid_ep, a_msg_idx}));
    }
  }
  {
    ParquetWriter msg_type_writer(schema::message_type(),
                                  output_dir + "/message_type.parquet");
    arrow::Int32Builder mt_idx;
    arrow::Int64Builder mt_size;
    for (const auto &m : sts_data.messages) {
      PARQUET_THROW_NOT_OK(mt_idx.Append(m.msg_idx));
      PARQUET_THROW_NOT_OK(mt_size.Append(static_cast<int64_t>(m.size)));
    }
    if (mt_idx.length() > 0) {
      std::shared_ptr<arrow::Array> a_mt_idx, a_mt_size;
      PARQUET_THROW_NOT_OK(mt_idx.Finish(&a_mt_idx));
      PARQUET_THROW_NOT_OK(mt_size.Finish(&a_mt_size));
      msg_type_writer.WriteBatch(arrow::RecordBatch::Make(
          schema::message_type(), a_mt_idx->length(), {a_mt_idx, a_mt_size}));
    }
  }
}

} // namespace charmvz
