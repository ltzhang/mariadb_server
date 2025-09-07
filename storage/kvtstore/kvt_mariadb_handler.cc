/*
   Copyright (c) 2025 KVT Storage Engine

   Implementation of handler integration with MariaDB-specific KVT APIs
*/

#include "kvt_mariadb_handler.h"
#include "sql_class.h"
#include "field.h"
#include <sstream>

namespace kvt_handler {

// =============================================================================
// ConditionAnalyzer Implementation
// =============================================================================

bool ConditionAnalyzer::analyze_condition(
    const COND* cond,
    TABLE* table,
    KVTCompositeFilter& filter,
    std::string& reason)
{
    if (!cond) {
        reason = "No condition provided";
        return false;
    }
    
    const Item* item = static_cast<const Item*>(cond);
    
    // Check for simple condition
    if (item->type() == Item::FUNC_ITEM) {
        const Item_func* func = static_cast<const Item_func*>(item);
        
        // Handle AND/OR
        if (func->functype() == Item_func::COND_AND_FUNC) {
            filter.op = KVTLogicalOp::AND;
            for (uint i = 0; i < func->argument_count(); i++) {
                KVTFilterCondition child_filter;
                if (convert_item_to_filter(func->arguments()[i], table, child_filter)) {
                    filter.conditions.push_back(child_filter);
                } else {
                    // Try as composite filter
                    KVTCompositeFilter child_composite;
                    std::string child_reason;
                    if (analyze_condition(
                        reinterpret_cast<const COND*>(func->arguments()[i]),
                        table, child_composite, child_reason)) {
                        filter.conditions.push_back(child_composite);
                    } else {
                        reason = "Cannot push down sub-condition: " + child_reason;
                        return false;
                    }
                }
            }
            return true;
        } else if (func->functype() == Item_func::COND_OR_FUNC) {
            filter.op = KVTLogicalOp::OR;
            for (uint i = 0; i < func->argument_count(); i++) {
                KVTFilterCondition child_filter;
                if (convert_item_to_filter(func->arguments()[i], table, child_filter)) {
                    filter.conditions.push_back(child_filter);
                } else {
                    reason = "Cannot push down OR sub-condition";
                    return false;
                }
            }
            return true;
        } else {
            // Try as simple filter
            KVTFilterCondition simple_filter;
            if (convert_item_to_filter(item, table, simple_filter)) {
                filter.op = KVTLogicalOp::AND;
                filter.conditions.push_back(simple_filter);
                return true;
            }
        }
    }
    
    reason = "Condition too complex for pushdown";
    return false;
}

bool ConditionAnalyzer::is_pushable(const COND* cond) {
    if (!cond) return false;
    
    const Item* item = static_cast<const Item*>(cond);
    
    if (item->type() == Item::FUNC_ITEM) {
        const Item_func* func = static_cast<const Item_func*>(item);
        
        switch (func->functype()) {
            case Item_func::EQ_FUNC:
            case Item_func::NE_FUNC:
            case Item_func::LT_FUNC:
            case Item_func::LE_FUNC:
            case Item_func::GT_FUNC:
            case Item_func::GE_FUNC:
            case Item_func::IN_FUNC:
            case Item_func::BETWEEN:
            case Item_func::LIKE_FUNC:
            case Item_func::ISNULL_FUNC:
            case Item_func::ISNOTNULL_FUNC:
            case Item_func::COND_AND_FUNC:
            case Item_func::COND_OR_FUNC:
                return true;
            default:
                return false;
        }
    }
    
    return false;
}

bool ConditionAnalyzer::convert_item_to_filter(
    const Item* item,
    TABLE* table,
    KVTFilterCondition& filter)
{
    if (!item || item->type() != Item::FUNC_ITEM) {
        return false;
    }
    
    const Item_func* func = static_cast<const Item_func*>(item);
    
    switch (func->functype()) {
        case Item_func::EQ_FUNC:
        case Item_func::NE_FUNC:
        case Item_func::LT_FUNC:
        case Item_func::LE_FUNC:
        case Item_func::GT_FUNC:
        case Item_func::GE_FUNC:
            return convert_comparison(func, table, filter);
            
        case Item_func::IN_FUNC:
            return convert_in_condition(
                static_cast<const Item_func_in*>(func), table, filter);
            
        case Item_func::BETWEEN:
            return convert_between(
                static_cast<const Item_func_between*>(func), table, filter);
            
        case Item_func::LIKE_FUNC:
            return convert_like(
                static_cast<const Item_func_like*>(func), table, filter);
            
        case Item_func::ISNULL_FUNC:
            if (func->argument_count() == 1 && 
                func->arguments()[0]->type() == Item::FIELD_ITEM) {
                Item_field* field = static_cast<Item_field*>(func->arguments()[0]);
                filter.column_index = field->field->field_index;
                filter.op = KVTCompareOp::IS_NULL;
                return true;
            }
            break;
            
        case Item_func::ISNOTNULL_FUNC:
            if (func->argument_count() == 1 && 
                func->arguments()[0]->type() == Item::FIELD_ITEM) {
                Item_field* field = static_cast<Item_field*>(func->arguments()[0]);
                filter.column_index = field->field->field_index;
                filter.op = KVTCompareOp::IS_NOT_NULL;
                return true;
            }
            break;
            
        default:
            break;
    }
    
    return false;
}

bool ConditionAnalyzer::convert_comparison(
    const Item_func* func,
    TABLE* table,
    KVTFilterCondition& filter)
{
    if (func->argument_count() != 2) {
        return false;
    }
    
    Item* left = func->arguments()[0];
    Item* right = func->arguments()[1];
    
    // Check for field = constant pattern
    if (left->type() == Item::FIELD_ITEM && right->const_item()) {
        Item_field* field = static_cast<Item_field*>(left);
        filter.column_index = field->field->field_index;
        filter.op = func_type_to_compare_op(func->functype());
        return item_to_kvt_value(right, filter.value);
    }
    
    // Check for constant = field pattern
    if (left->const_item() && right->type() == Item::FIELD_ITEM) {
        Item_field* field = static_cast<Item_field*>(right);
        filter.column_index = field->field->field_index;
        // Reverse the operator
        switch (func->functype()) {
            case Item_func::LT_FUNC:
                filter.op = KVTCompareOp::GT;
                break;
            case Item_func::LE_FUNC:
                filter.op = KVTCompareOp::GE;
                break;
            case Item_func::GT_FUNC:
                filter.op = KVTCompareOp::LT;
                break;
            case Item_func::GE_FUNC:
                filter.op = KVTCompareOp::LE;
                break;
            default:
                filter.op = func_type_to_compare_op(func->functype());
        }
        return item_to_kvt_value(left, filter.value);
    }
    
    return false;
}

bool ConditionAnalyzer::convert_in_condition(
    const Item_func_in* func,
    TABLE* table,
    KVTFilterCondition& filter)
{
    if (func->argument_count() < 2) {
        return false;
    }
    
    // First argument should be a field
    if (func->arguments()[0]->type() != Item::FIELD_ITEM) {
        return false;
    }
    
    Item_field* field = static_cast<Item_field*>(func->arguments()[0]);
    filter.column_index = field->field->field_index;
    filter.op = KVTCompareOp::EQ;  // IN is handled via in_values
    
    // Convert all IN values
    for (uint i = 1; i < func->argument_count(); i++) {
        KVTValue val;
        if (!item_to_kvt_value(func->arguments()[i], val)) {
            return false;
        }
        filter.in_values.push_back(val);
    }
    
    return true;
}

bool ConditionAnalyzer::convert_between(
    const Item_func_between* func,
    TABLE* table,
    KVTFilterCondition& filter)
{
    if (func->argument_count() != 3) {
        return false;
    }
    
    // First argument should be a field
    if (func->arguments()[0]->type() != Item::FIELD_ITEM) {
        return false;
    }
    
    Item_field* field = static_cast<Item_field*>(func->arguments()[0]);
    filter.column_index = field->field->field_index;
    filter.op = KVTCompareOp::GE;  // BETWEEN uses both bounds
    
    // Get low and high values
    if (!item_to_kvt_value(func->arguments()[1], filter.value)) {
        return false;
    }
    
    KVTValue high;
    if (!item_to_kvt_value(func->arguments()[2], high)) {
        return false;
    }
    filter.high_value = high;
    
    return true;
}

bool ConditionAnalyzer::convert_like(
    const Item_func_like* func,
    TABLE* table,
    KVTFilterCondition& filter)
{
    if (func->argument_count() != 2) {
        return false;
    }
    
    // First argument should be a field
    if (func->arguments()[0]->type() != Item::FIELD_ITEM) {
        return false;
    }
    
    // Second argument should be a constant string
    if (!func->arguments()[1]->const_item()) {
        return false;
    }
    
    Item_field* field = static_cast<Item_field*>(func->arguments()[0]);
    filter.column_index = field->field->field_index;
    filter.op = KVTCompareOp::EQ;  // LIKE is special-cased
    
    // Get pattern
    char buff[MAX_FIELD_WIDTH];
    String str(buff, sizeof(buff), &my_charset_bin);
    String* res = func->arguments()[1]->val_str(&str);
    
    if (res) {
        filter.value = KVTValue(std::string(res->ptr(), res->length()));
        return true;
    }
    
    return false;
}

bool ConditionAnalyzer::item_to_kvt_value(Item* item, KVTValue& value) {
    if (!item || !item->const_item()) {
        return false;
    }
    
    if (item->null_value || item->is_null()) {
        value.is_null = true;
        return true;
    }
    
    value.is_null = false;
    
    switch (item->result_type()) {
        case INT_RESULT: {
            longlong val = item->val_int();
            value = KVTValue((int64_t)val);
            return true;
        }
        case REAL_RESULT: {
            double val = item->val_real();
            value = KVTValue(val);
            return true;
        }
        case STRING_RESULT: {
            char buff[MAX_FIELD_WIDTH];
            String str(buff, sizeof(buff), &my_charset_bin);
            String* res = item->val_str(&str);
            if (res) {
                value = KVTValue(std::string(res->ptr(), res->length()));
                return true;
            }
            break;
        }
        case DECIMAL_RESULT: {
            // Convert decimal to string
            char buff[MAX_FIELD_WIDTH];
            String str(buff, sizeof(buff), &my_charset_bin);
            String* res = item->val_str(&str);
            if (res) {
                value = KVTValue(std::string(res->ptr(), res->length()));
                value.type = KVTColumnType::DECIMAL;
                return true;
            }
            break;
        }
        default:
            break;
    }
    
    return false;
}

KVTCompareOp ConditionAnalyzer::func_type_to_compare_op(Item_func::Functype type) {
    switch (type) {
        case Item_func::EQ_FUNC:
            return KVTCompareOp::EQ;
        case Item_func::NE_FUNC:
            return KVTCompareOp::NE;
        case Item_func::LT_FUNC:
            return KVTCompareOp::LT;
        case Item_func::LE_FUNC:
            return KVTCompareOp::LE;
        case Item_func::GT_FUNC:
            return KVTCompareOp::GT;
        case Item_func::GE_FUNC:
            return KVTCompareOp::GE;
        default:
            return KVTCompareOp::EQ;
    }
}

// =============================================================================
// SchemaConverter Implementation
// =============================================================================

void SchemaConverter::convert_table_schema(
    TABLE* table,
    std::vector<KVTColumnInfo>& columns)
{
    columns.clear();
    columns.reserve(table->s->fields);
    
    for (uint i = 0; i < table->s->fields; i++) {
        Field* field = table->field[i];
        KVTColumnInfo col_info;
        
        col_info.index = i;
        col_info.name = field->field_name.str;
        col_info.type = field_type_to_kvt_type(field);
        col_info.max_length = field->field_length;
        col_info.nullable = field->maybe_null();
        col_info.is_primary_key = (field->flags & PRI_KEY_FLAG) != 0;
        col_info.is_indexed = (field->flags & MULTIPLE_KEY_FLAG) != 0;
        
        columns.push_back(col_info);
    }
}

KVTColumnType SchemaConverter::field_type_to_kvt_type(Field* field) {
    switch (field->real_type()) {
        case MYSQL_TYPE_TINY:
            return (field->flags & UNSIGNED_FLAG) ? 
                   KVTColumnType::UINT8 : KVTColumnType::INT8;
        case MYSQL_TYPE_SHORT:
            return (field->flags & UNSIGNED_FLAG) ? 
                   KVTColumnType::UINT16 : KVTColumnType::INT16;
        case MYSQL_TYPE_LONG:
            return (field->flags & UNSIGNED_FLAG) ? 
                   KVTColumnType::UINT32 : KVTColumnType::INT32;
        case MYSQL_TYPE_LONGLONG:
            return (field->flags & UNSIGNED_FLAG) ? 
                   KVTColumnType::UINT64 : KVTColumnType::INT64;
        case MYSQL_TYPE_FLOAT:
            return KVTColumnType::FLOAT;
        case MYSQL_TYPE_DOUBLE:
            return KVTColumnType::DOUBLE;
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
            return KVTColumnType::DECIMAL;
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
            return KVTColumnType::VARCHAR;
        case MYSQL_TYPE_STRING:
            return KVTColumnType::CHAR;
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB:
            return KVTColumnType::BLOB;
        case MYSQL_TYPE_DATE:
            return KVTColumnType::DATE;
        case MYSQL_TYPE_TIME:
        case MYSQL_TYPE_TIME2:
            return KVTColumnType::TIME;
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_DATETIME2:
            return KVTColumnType::DATETIME;
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_TIMESTAMP2:
            return KVTColumnType::TIMESTAMP;
        case MYSQL_TYPE_JSON:
            return KVTColumnType::JSON;
        default:
            return KVTColumnType::VARCHAR;
    }
}

// =============================================================================
// PushdownExecutor Implementation
// =============================================================================

int PushdownExecutor::execute_filter(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTCompositeFilter& filter,
    const KVTKey& start_key,
    const KVTKey& end_key,
    size_t limit,
    std::vector<KVTRow>& results)
{
    std::string error_msg;
    KVTError err = kvt_filter_composite(
        tx_id, table_id, filter, start_key, end_key, limit, results, error_msg);
    
    if (err == KVTError::SUCCESS) {
        PushdownStats::record_filter_pushdown(true, results.size());
        return 0;
    } else {
        PushdownStats::record_filter_pushdown(false);
        return ErrorHandler::kvt_to_mysql_error(err);
    }
}

int PushdownExecutor::execute_aggregation(
    uint64_t tx_id,
    uint64_t table_id,
    const std::vector<KVTAggregateSpec>& aggregates,
    const KVTCompositeFilter* filter,
    const KVTKey& start_key,
    const KVTKey& end_key,
    std::vector<KVTValue>& results)
{
    std::string error_msg;
    KVTError err = kvt_aggregate_multi(
        tx_id, table_id, aggregates, filter, start_key, end_key, results, error_msg);
    
    if (err == KVTError::SUCCESS) {
        PushdownStats::record_aggregation_pushdown(true);
        return 0;
    } else {
        PushdownStats::record_aggregation_pushdown(false);
        return ErrorHandler::kvt_to_mysql_error(err);
    }
}

int PushdownExecutor::execute_increment(
    uint64_t tx_id,
    uint64_t table_id,
    const KVTKey& row_key,
    uint32_t column_index,
    int64_t delta,
    int64_t& old_value)
{
    std::string error_msg;
    KVTError err = kvt_atomic_increment(
        tx_id, table_id, row_key, column_index, delta, old_value, error_msg);
    
    if (err == KVTError::SUCCESS) {
        PushdownStats::record_atomic_operation();
        return 0;
    } else {
        return ErrorHandler::kvt_to_mysql_error(err);
    }
}

// =============================================================================
// ErrorHandler Implementation
// =============================================================================

int ErrorHandler::kvt_to_mysql_error(KVTError err) {
    switch (err) {
        case KVTError::SUCCESS:
            return 0;
        case KVTError::KEY_NOT_FOUND:
            return HA_ERR_KEY_NOT_FOUND;
        case KVTError::TABLE_NOT_FOUND:
            return HA_ERR_NO_SUCH_TABLE;
        case KVTError::WRITE_CONFLICT:
        case KVTError::UPDATE_CONFLICT:
        case KVTError::DELETE_CONFLICT:
            return HA_ERR_LOCK_WAIT_TIMEOUT;
        case KVTError::KEY_IS_LOCKED:
            return HA_ERR_LOCK_TABLE_FULL;
        default:
            return HA_ERR_GENERIC;
    }
}

// =============================================================================
// PushdownStats Implementation
// =============================================================================

void PushdownStats::record_filter_pushdown(bool success, uint64_t rows_filtered) {
    auto& stats = get_stats();
    if (success) {
        stats.filter_pushdowns++;
        stats.rows_filtered_at_storage += rows_filtered;
    } else {
        stats.pushdown_failures++;
    }
}

void PushdownStats::record_aggregation_pushdown(bool success) {
    auto& stats = get_stats();
    if (success) {
        stats.aggregation_pushdowns++;
    } else {
        stats.pushdown_failures++;
    }
}

void PushdownStats::record_atomic_operation() {
    get_stats().atomic_operations++;
}

void PushdownStats::record_batch_operation(size_t batch_size) {
    auto& stats = get_stats();
    stats.batch_operations++;
    stats.bytes_saved += batch_size * 100;  // Estimate savings
}

} // namespace kvt_handler