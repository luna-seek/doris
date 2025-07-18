// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <memory>
#include <roaring/roaring.hh>

#include "common/exception.h"
#include "olap/rowset/segment_v2/bitmap_index_reader.h"
#include "olap/rowset/segment_v2/bloom_filter.h"
#include "olap/rowset/segment_v2/inverted_index_iterator.h"
#include "runtime/define_primitive_type.h"
#include "util/runtime_profile.h"
#include "vec/columns/column.h"
#include "vec/exprs/vruntimefilter_wrapper.h"

using namespace doris::segment_v2;

namespace doris {

enum class PredicateType {
    UNKNOWN = 0,
    EQ = 1,
    NE = 2,
    LT = 3,
    LE = 4,
    GT = 5,
    GE = 6,
    IN_LIST = 7,
    NOT_IN_LIST = 8,
    IS_NULL = 9,
    IS_NOT_NULL = 10,
    BF = 11,            // BloomFilter
    BITMAP_FILTER = 12, // BitmapFilter
    MATCH = 13,         // fulltext match
};

template <PrimitiveType primitive_type, typename ResultType>
ResultType get_zone_map_value(void* data_ptr) {
    ResultType res;
    // DecimalV2's storage value is different from predicate or compute value type
    // need convert it to DecimalV2Value
    if constexpr (primitive_type == PrimitiveType::TYPE_DECIMALV2) {
        decimal12_t decimal_12_t_value;
        memcpy((char*)(&decimal_12_t_value), data_ptr, sizeof(decimal12_t));
        res.from_olap_decimal(decimal_12_t_value.integer, decimal_12_t_value.fraction);
    } else if constexpr (primitive_type == PrimitiveType::TYPE_DATE) {
        static_assert(std::is_same_v<ResultType, VecDateTimeValue>);
        uint24_t date;
        memcpy(&date, data_ptr, sizeof(uint24_t));
        res.from_olap_date(date);
    } else if constexpr (primitive_type == PrimitiveType::TYPE_DATETIME) {
        static_assert(std::is_same_v<ResultType, VecDateTimeValue>);
        uint64_t datetime;
        memcpy(&datetime, data_ptr, sizeof(uint64_t));
        res.from_olap_datetime(datetime);
    } else {
        memcpy(reinterpret_cast<void*>(&res), data_ptr, sizeof(ResultType));
    }
    return res;
}

inline std::string type_to_string(PredicateType type) {
    switch (type) {
    case PredicateType::UNKNOWN:
        return "UNKNOWN";

    case PredicateType::EQ:
        return "EQ";

    case PredicateType::NE:
        return "NE";

    case PredicateType::LT:
        return "LT";

    case PredicateType::LE:
        return "LE";

    case PredicateType::GT:
        return "GT";

    case PredicateType::GE:
        return "GE";

    case PredicateType::IN_LIST:
        return "IN_LIST";

    case PredicateType::NOT_IN_LIST:
        return "NOT_IN_LIST";

    case PredicateType::IS_NULL:
        return "IS_NULL";

    case PredicateType::IS_NOT_NULL:
        return "IS_NOT_NULL";

    case PredicateType::BF:
        return "BF";
    default:
        return "";
    };

    return "";
}

struct PredicateTypeTraits {
    static constexpr bool is_range(PredicateType type) {
        return (type == PredicateType::LT || type == PredicateType::LE ||
                type == PredicateType::GT || type == PredicateType::GE);
    }

    static constexpr bool is_bloom_filter(PredicateType type) { return type == PredicateType::BF; }

    static constexpr bool is_list(PredicateType type) {
        return (type == PredicateType::IN_LIST || type == PredicateType::NOT_IN_LIST);
    }

    static constexpr bool is_equal_or_list(PredicateType type) {
        return (type == PredicateType::EQ || type == PredicateType::IN_LIST);
    }

    static constexpr bool is_comparison(PredicateType type) {
        return (type == PredicateType::EQ || type == PredicateType::NE ||
                type == PredicateType::LT || type == PredicateType::LE ||
                type == PredicateType::GT || type == PredicateType::GE);
    }
};

#define EVALUATE_BY_SELECTOR(EVALUATE_IMPL_WITH_NULL_MAP, EVALUATE_IMPL_WITHOUT_NULL_MAP) \
    const bool is_dense_column = pred_col.size() == size;                                 \
    for (uint16_t i = 0; i < size; i++) {                                                 \
        uint16_t idx = is_dense_column ? i : sel[i];                                      \
        if constexpr (is_nullable) {                                                      \
            if (EVALUATE_IMPL_WITH_NULL_MAP(idx)) {                                       \
                sel[new_size++] = idx;                                                    \
            }                                                                             \
        } else {                                                                          \
            if (EVALUATE_IMPL_WITHOUT_NULL_MAP(idx)) {                                    \
                sel[new_size++] = idx;                                                    \
            }                                                                             \
        }                                                                                 \
    }

class ColumnPredicate {
public:
    explicit ColumnPredicate(uint32_t column_id, bool opposite = false)
            : _column_id(column_id), _opposite(opposite) {
        reset_judge_selectivity();
    }

    virtual ~ColumnPredicate() = default;

    virtual PredicateType type() const = 0;

    //evaluate predicate on Bitmap
    virtual Status evaluate(BitmapIndexIterator* iterator, uint32_t num_rows,
                            roaring::Roaring* roaring) const = 0;

    //evaluate predicate on inverted
    virtual Status evaluate(const vectorized::IndexFieldNameAndTypePair& name_with_type,
                            IndexIterator* iterator, uint32_t num_rows,
                            roaring::Roaring* bitmap) const {
        return Status::NotSupported(
                "Not Implemented evaluate with inverted index, please check the predicate");
    }

    virtual double get_ignore_threshold() const { return 0; }

    // evaluate predicate on IColumn
    // a short circuit eval way
    uint16_t evaluate(const vectorized::IColumn& column, uint16_t* sel, uint16_t size) const {
        if (always_true()) {
            return size;
        }

        uint16_t new_size = _evaluate_inner(column, sel, size);
        if (_can_ignore()) {
            do_judge_selectivity(size - new_size, size);
        }
        update_filter_info(size - new_size, size);
        return new_size;
    }
    virtual void evaluate_and(const vectorized::IColumn& column, const uint16_t* sel, uint16_t size,
                              bool* flags) const {}
    virtual void evaluate_or(const vectorized::IColumn& column, const uint16_t* sel, uint16_t size,
                             bool* flags) const {}

    virtual bool support_zonemap() const { return true; }

    virtual bool evaluate_and(const std::pair<WrapperField*, WrapperField*>& statistic) const {
        return true;
    }

    virtual bool is_always_true(const std::pair<WrapperField*, WrapperField*>& statistic) const {
        return false;
    }

    virtual bool evaluate_del(const std::pair<WrapperField*, WrapperField*>& statistic) const {
        return false;
    }

    virtual bool evaluate_and(const BloomFilter* bf) const { return true; }

    virtual bool evaluate_and(const StringRef* dict_words, const size_t dict_count) const {
        return true;
    }

    virtual bool can_do_bloom_filter(bool ngram) const { return false; }

    // Check input type could apply safely.
    // Note: Currenly ColumnPredicate is not include complex type, so use PrimitiveType
    // is simple and intuitive
    virtual bool can_do_apply_safely(PrimitiveType input_type, bool is_null) const = 0;

    // used to evaluate pre read column in lazy materialization
    // now only support integer/float
    // a vectorized eval way
    virtual void evaluate_vec(const vectorized::IColumn& column, uint16_t size, bool* flags) const {
        DCHECK(false) << "should not reach here";
    }
    virtual void evaluate_and_vec(const vectorized::IColumn& column, uint16_t size,
                                  bool* flags) const {
        DCHECK(false) << "should not reach here";
    }

    virtual std::string get_search_str() const {
        DCHECK(false) << "should not reach here";
        return "";
    }

    virtual void set_page_ng_bf(std::unique_ptr<segment_v2::BloomFilter>) {
        DCHECK(false) << "should not reach here";
    }
    uint32_t column_id() const { return _column_id; }

    bool opposite() const { return _opposite; }

    std::string debug_string() const {
        return _debug_string() +
               fmt::format(", column_id={}, opposite={}, can_ignore={}, runtime_filter_id={}",
                           _column_id, _opposite, _can_ignore(), _runtime_filter_id);
    }

    int get_runtime_filter_id() const { return _runtime_filter_id; }

    void attach_profile_counter(
            int filter_id, std::shared_ptr<RuntimeProfile::Counter> predicate_filtered_rows_counter,
            std::shared_ptr<RuntimeProfile::Counter> predicate_input_rows_counter) {
        _runtime_filter_id = filter_id;
        DCHECK(predicate_filtered_rows_counter != nullptr);
        DCHECK(predicate_input_rows_counter != nullptr);

        if (predicate_filtered_rows_counter != nullptr) {
            _predicate_filtered_rows_counter = predicate_filtered_rows_counter;
        }
        if (predicate_input_rows_counter != nullptr) {
            _predicate_input_rows_counter = predicate_input_rows_counter;
        }
    }

    /// TODO: Currently we only record statistics for runtime filters, in the future we should record for all predicates
    void update_filter_info(int64_t filter_rows, int64_t input_rows) const {
        COUNTER_UPDATE(_predicate_input_rows_counter, input_rows);
        COUNTER_UPDATE(_predicate_filtered_rows_counter, filter_rows);
    }

    static std::string pred_type_string(PredicateType type) {
        switch (type) {
        case PredicateType::EQ:
            return "eq";
        case PredicateType::NE:
            return "ne";
        case PredicateType::LT:
            return "lt";
        case PredicateType::LE:
            return "le";
        case PredicateType::GT:
            return "gt";
        case PredicateType::GE:
            return "ge";
        case PredicateType::IN_LIST:
            return "in";
        case PredicateType::NOT_IN_LIST:
            return "not_in";
        case PredicateType::IS_NULL:
            return "is_null";
        case PredicateType::IS_NOT_NULL:
            return "is_not_null";
        case PredicateType::BF:
            return "bf";
        case PredicateType::MATCH:
            return "match";
        default:
            return "unknown";
        }
    }

    bool always_true() const { return _always_true; }
    // Return whether the ColumnPredicate was created by a runtime filter.
    // If true, it was definitely created by a runtime filter.
    // If false, it may still have been created by a runtime filter,
    // as certain filters like "in filter" generate key ranges instead of ColumnPredicate.
    // is_runtime_filter uses _can_ignore, except for BitmapFilter,
    // as BitmapFilter cannot ignore data.
    virtual bool is_runtime_filter() const { return _can_ignore(); }

protected:
    virtual std::string _debug_string() const = 0;
    virtual bool _can_ignore() const { return _runtime_filter_id != -1; }
    virtual uint16_t _evaluate_inner(const vectorized::IColumn& column, uint16_t* sel,
                                     uint16_t size) const {
        throw Exception(INTERNAL_ERROR, "Not Implemented _evaluate_inner");
    }

    void reset_judge_selectivity() const {
        _always_true = false;
        _judge_counter = config::runtime_filter_sampling_frequency;
        _judge_input_rows = 0;
        _judge_filter_rows = 0;
    }

    void do_judge_selectivity(uint64_t filter_rows, uint64_t input_rows) const {
        if ((_judge_counter--) == 0) {
            reset_judge_selectivity();
        }
        if (!_always_true) {
            _judge_filter_rows += filter_rows;
            _judge_input_rows += input_rows;
            vectorized::VRuntimeFilterWrapper::judge_selectivity(
                    get_ignore_threshold(), _judge_filter_rows, _judge_input_rows, _always_true);
        }
    }

    uint32_t _column_id;
    // TODO: the value is only in delete condition, better be template value
    bool _opposite;
    int _runtime_filter_id = -1;
    // VRuntimeFilterWrapper and ColumnPredicate share the same logic,
    // but it's challenging to unify them, so the code is duplicated.
    // _judge_counter, _judge_input_rows, _judge_filter_rows, and _always_true
    // are variables used to implement the _always_true logic, calculated periodically
    // based on runtime_filter_sampling_frequency. During each period, if _always_true
    // is evaluated as true, the logic for always_true is applied for the rest of that period
    // without recalculating. At the beginning of the next period,
    // reset_judge_selectivity is used to reset these variables.
    mutable int _judge_counter = 0;
    mutable uint64_t _judge_input_rows = 0;
    mutable uint64_t _judge_filter_rows = 0;
    mutable bool _always_true = false;

    std::shared_ptr<RuntimeProfile::Counter> _predicate_filtered_rows_counter =
            std::make_shared<RuntimeProfile::Counter>(TUnit::UNIT, 0);

    std::shared_ptr<RuntimeProfile::Counter> _predicate_input_rows_counter =
            std::make_shared<RuntimeProfile::Counter>(TUnit::UNIT, 0);
};

} //namespace doris
