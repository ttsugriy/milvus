// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License

#include <random>

#include <algorithm>
#include <numeric>
#include <thread>
#include <queue>

#include <knowhere/index/vector_index/adapter/VectorAdapter.h>
#include <knowhere/index/vector_index/VecIndexFactory.h>
#include <faiss/utils/distances.h>
#include <query/SearchOnSealed.h>
#include "query/generated/ExecPlanNodeVisitor.h"
#include "segcore/SegmentGrowingImpl.h"
#include "query/PlanNode.h"
#include "query/PlanImpl.h"
#include "segcore/Reduce.h"
#include "utils/tools.h"

namespace milvus::segcore {

int64_t
SegmentGrowingImpl::PreInsert(int64_t size) {
    auto reserved_begin = record_.reserved.fetch_add(size);
    return reserved_begin;
}

int64_t
SegmentGrowingImpl::PreDelete(int64_t size) {
    auto reserved_begin = deleted_record_.reserved.fetch_add(size);
    return reserved_begin;
}

auto
SegmentGrowingImpl::get_deleted_bitmap(int64_t del_barrier,
                                       Timestamp query_timestamp,
                                       int64_t insert_barrier,
                                       bool force) -> std::shared_ptr<DeletedRecord::TmpBitmap> {
    auto old = deleted_record_.get_lru_entry();

    if (!force || old->bitmap_ptr->count() == insert_barrier) {
        if (old->del_barrier == del_barrier) {
            return old;
        }
    }

    auto current = old->clone(insert_barrier);
    current->del_barrier = del_barrier;

    auto bitmap = current->bitmap_ptr;
    if (del_barrier < old->del_barrier) {
        for (auto del_index = del_barrier; del_index < old->del_barrier; ++del_index) {
            // get uid in delete logs
            auto uid = deleted_record_.uids_[del_index];
            // map uid to corresponding offsets, select the max one, which should be the target
            // the max one should be closest to query_timestamp, so the delete log should refer to it
            int64_t the_offset = -1;
            auto [iter_b, iter_e] = uid2offset_.equal_range(uid);
            for (auto iter = iter_b; iter != iter_e; ++iter) {
                auto offset = iter->second;
                if (record_.timestamps_[offset] < query_timestamp) {
                    Assert(offset < insert_barrier);
                    the_offset = std::max(the_offset, offset);
                }
            }
            // if not found, skip
            if (the_offset == -1) {
                continue;
            }
            // otherwise, clear the flag
            bitmap->clear(the_offset);
        }
        return current;
    } else {
        for (auto del_index = old->del_barrier; del_index < del_barrier; ++del_index) {
            // get uid in delete logs
            auto uid = deleted_record_.uids_[del_index];
            // map uid to corresponding offsets, select the max one, which should be the target
            // the max one should be closest to query_timestamp, so the delete log should refer to it
            int64_t the_offset = -1;
            auto [iter_b, iter_e] = uid2offset_.equal_range(uid);
            for (auto iter = iter_b; iter != iter_e; ++iter) {
                auto offset = iter->second;
                if (offset >= insert_barrier) {
                    continue;
                }
                if (record_.timestamps_[offset] < query_timestamp) {
                    Assert(offset < insert_barrier);
                    the_offset = std::max(the_offset, offset);
                }
            }

            // if not found, skip
            if (the_offset == -1) {
                continue;
            }

            // otherwise, set the flag
            bitmap->set(the_offset);
        }
        this->deleted_record_.insert_lru_entry(current);
    }
    return current;
}

Status
SegmentGrowingImpl::Insert(int64_t reserved_begin,
                           int64_t size,
                           const int64_t* uids_raw,
                           const Timestamp* timestamps_raw,
                           const RowBasedRawData& entities_raw) {
    Assert(entities_raw.count == size);
    // step 1: check schema if valid
    if (entities_raw.sizeof_per_row != schema_->get_total_sizeof()) {
        std::string msg = "entity length = " + std::to_string(entities_raw.sizeof_per_row) +
                          ", schema length = " + std::to_string(schema_->get_total_sizeof());
        throw std::runtime_error(msg);
    }

    // step 2: sort timestamp
    auto raw_data = reinterpret_cast<const char*>(entities_raw.raw_data);
    auto len_per_row = entities_raw.sizeof_per_row;
    std::vector<std::tuple<Timestamp, idx_t, int64_t>> ordering;
    ordering.resize(size);
    // #pragma omp parallel for
    for (int i = 0; i < size; ++i) {
        ordering[i] = std::make_tuple(timestamps_raw[i], uids_raw[i], i);
    }
    std::sort(ordering.begin(), ordering.end());

    // step 3: and convert row-base data to column base accordingly
    auto sizeof_infos = schema_->get_sizeof_infos();
    std::vector<int> offset_infos(schema_->size() + 1, 0);
    std::partial_sum(sizeof_infos.begin(), sizeof_infos.end(), offset_infos.begin() + 1);
    std::vector<std::vector<char>> entities(schema_->size());

    for (int fid = 0; fid < schema_->size(); ++fid) {
        auto len = sizeof_infos[fid];
        entities[fid].resize(len * size);
    }

    std::vector<idx_t> uids(size);
    std::vector<Timestamp> timestamps(size);
    // #pragma omp parallel for
    for (int index = 0; index < size; ++index) {
        auto [t, uid, order_index] = ordering[index];
        timestamps[index] = t;
        uids[index] = uid;
        for (int fid = 0; fid < schema_->size(); ++fid) {
            auto len = sizeof_infos[fid];
            auto offset = offset_infos[fid];
            auto src = raw_data + offset + order_index * len_per_row;
            auto dst = entities[fid].data() + index * len;
            memcpy(dst, src, len);
        }
    }

    // step 4: fill into Segment.ConcurrentVector
    record_.timestamps_.set_data(reserved_begin, timestamps.data(), size);
    record_.uids_.set_data(reserved_begin, uids.data(), size);
    for (int fid = 0; fid < schema_->size(); ++fid) {
        auto field_offset = FieldOffset(fid);
        record_.get_base_entity(field_offset)->set_data_raw(reserved_begin, entities[fid].data(), size);
    }

    for (int i = 0; i < uids.size(); ++i) {
        auto uid = uids[i];
        // NOTE: this must be the last step, cannot be put above
        uid2offset_.insert(std::make_pair(uid, reserved_begin + i));
    }

    record_.ack_responder_.AddSegment(reserved_begin, reserved_begin + size);
    indexing_record_.UpdateResourceAck(record_.ack_responder_.GetAck() / chunk_size_, record_);
    return Status::OK();
}

Status
SegmentGrowingImpl::Delete(int64_t reserved_begin,
                           int64_t size,
                           const int64_t* uids_raw,
                           const Timestamp* timestamps_raw) {
    std::vector<std::tuple<Timestamp, idx_t>> ordering;
    ordering.resize(size);
    // #pragma omp parallel for
    for (int i = 0; i < size; ++i) {
        ordering[i] = std::make_tuple(timestamps_raw[i], uids_raw[i]);
    }
    std::sort(ordering.begin(), ordering.end());
    std::vector<idx_t> uids(size);
    std::vector<Timestamp> timestamps(size);
    // #pragma omp parallel for
    for (int index = 0; index < size; ++index) {
        auto [t, uid] = ordering[index];
        timestamps[index] = t;
        uids[index] = uid;
    }
    deleted_record_.timestamps_.set_data(reserved_begin, timestamps.data(), size);
    deleted_record_.uids_.set_data(reserved_begin, uids.data(), size);
    deleted_record_.ack_responder_.AddSegment(reserved_begin, reserved_begin + size);
    return Status::OK();
    //    for (int i = 0; i < size; ++i) {
    //        auto key = row_ids[i];
    //        auto time = timestamps[i];
    //        delete_logs_.insert(std::make_pair(key, time));
    //    }
    //    return Status::OK();
}

Status
SegmentGrowingImpl::Close() {
    if (this->record_.reserved != this->record_.ack_responder_.GetAck()) {
        PanicInfo("insert not ready");
    }
    if (this->deleted_record_.reserved != this->deleted_record_.ack_responder_.GetAck()) {
        PanicInfo("delete not ready");
    }
    state_ = SegmentState::Closed;
    return Status::OK();
}

int64_t
SegmentGrowingImpl::GetMemoryUsageInBytes() const {
    int64_t total_bytes = 0;
    int64_t ins_n = upper_align(record_.reserved, chunk_size_);
    total_bytes += ins_n * (schema_->get_total_sizeof() + 16 + 1);
    int64_t del_n = upper_align(deleted_record_.reserved, chunk_size_);
    total_bytes += del_n * (16 * 2);
    return total_bytes;
}

Status
SegmentGrowingImpl::LoadIndexing(const LoadIndexInfo& info) {
    auto field_offset = schema_->get_offset(FieldId(info.field_id));

    Assert(info.index_params.count("metric_type"));
    auto metric_type_str = info.index_params.at("metric_type");

    sealed_indexing_record_.add_entry(field_offset, GetMetricType(metric_type_str), info.index);
    return Status::OK();
}

SpanBase
SegmentGrowingImpl::chunk_data_impl(FieldOffset field_offset, int64_t chunk_id) const {
    auto vec = get_insert_record().get_base_entity(field_offset);
    return vec->get_span_base(chunk_id);
}

int64_t
SegmentGrowingImpl::num_chunk_data() const {
    auto size = get_insert_record().ack_responder_.GetAck();
    return upper_div(size, chunk_size_);
}
void
SegmentGrowingImpl::vector_search(int64_t vec_count,
                                  query::QueryInfo query_info,
                                  const void* query_data,
                                  int64_t query_count,
                                  const BitsetView& bitset,
                                  QueryResult& output) const {
    auto& sealed_indexing = this->get_sealed_indexing_record();
    if (sealed_indexing.is_ready(query_info.field_offset_)) {
        query::SearchOnSealed(this->get_schema(), sealed_indexing, query_info, query_data, query_count, bitset, output);
    } else {
        SearchOnGrowing(*this, vec_count, query_info, query_data, query_count, bitset, output);
    }
}

}  // namespace milvus::segcore
