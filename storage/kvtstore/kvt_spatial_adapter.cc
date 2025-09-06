#include "kvt_spatial_adapter.h"
#include <cstring>
#include <algorithm>
#include <limits>
#include <cmath>

namespace kvt_spatial {

// Static member initialization
KVTSpatialAdapter* KVTSpatialAdapter::instance_ = nullptr;
std::mutex KVTSpatialAdapter::instance_mutex_;

// RTreeNode implementation
void RTreeNode::recalculate_mbr() {
    if (entries.empty()) {
        node_mbr = MBR();
        return;
    }
    
    node_mbr = entries[0].mbr;
    for (size_t i = 1; i < entries.size(); ++i) {
        node_mbr = mbr_union(node_mbr, entries[i].mbr);
    }
}

double RTreeNode::calculate_enlargement(const MBR& new_mbr) const {
    MBR enlarged = mbr_union(node_mbr, new_mbr);
    return mbr_area(enlarged) - mbr_area(node_mbr);
}

// SpatialSearchIterator implementation
SpatialSearchIterator::SpatialSearchIterator(kvt_transaction_t* txn,
                                           uint64_t table_id,
                                           uint32_t index_id,
                                           const MBR& search_mbr,
                                           SpatialPredicate predicate)
    : txn_(txn), table_id_(table_id), index_id_(index_id),
      search_mbr_(search_mbr), predicate_(predicate),
      current_index_(0), result_count_(0) {
    
    // Get root node and start search
    KVTSpatialAdapter* adapter = KVTSpatialAdapter::get_instance();
    uint64_t root_id = adapter->get_root_node_id(txn, table_id, index_id);
    
    if (root_id != 0) {
        search_queue_.push(SearchState(root_id, 255)); // 255 = unknown level
        search_node(root_id, 255);
    }
}

SpatialSearchIterator::~SpatialSearchIterator() = default;

bool SpatialSearchIterator::get_next(uint64_t& row_id, MBR* mbr) {
    // Process search queue if current results are exhausted
    while (current_index_ >= current_results_.size() && !search_queue_.empty()) {
        SearchState state = search_queue_.front();
        search_queue_.pop();
        search_node(state.node_id, state.level);
    }
    
    if (current_index_ < current_results_.size()) {
        row_id = current_results_[current_index_++];
        result_count_++;
        return true;
    }
    
    return false;
}

void SpatialSearchIterator::reset() {
    while (!search_queue_.empty()) {
        search_queue_.pop();
    }
    current_results_.clear();
    current_index_ = 0;
    result_count_ = 0;
    
    // Restart search from root
    KVTSpatialAdapter* adapter = KVTSpatialAdapter::get_instance();
    uint64_t root_id = adapter->get_root_node_id(txn_, table_id_, index_id_);
    
    if (root_id != 0) {
        search_queue_.push(SearchState(root_id, 255));
        search_node(root_id, 255);
    }
}

void SpatialSearchIterator::search_node(uint64_t node_id, uint8_t level) {
    auto node = load_node(node_id);
    if (!node) {
        return;
    }
    
    for (const auto& entry : node->entries) {
        if (!matches_predicate(entry.mbr)) {
            continue;
        }
        
        if (node->is_leaf()) {
            current_results_.push_back(entry.ptr.row_id);
        } else {
            search_queue_.push(SearchState(entry.ptr.child_id, node->level - 1));
        }
    }
}

bool SpatialSearchIterator::matches_predicate(const MBR& mbr) const {
    switch (predicate_) {
        case SP_INTERSECTS:
            return MBR_INTERSECT_CMP(&search_mbr_, &mbr);
        case SP_CONTAINS:
            return MBR_CONTAIN_CMP(&search_mbr_, &mbr);
        case SP_WITHIN:
            return MBR_WITHIN_CMP(&mbr, &search_mbr_);
        case SP_EQUALS:
            return MBR_EQUAL_CMP(&search_mbr_, &mbr);
        case SP_DISJOINT:
            return MBR_DISJOINT_CMP(&search_mbr_, &mbr);
        default:
            return MBR_INTERSECT_CMP(&search_mbr_, &mbr);
    }
}

std::unique_ptr<RTreeNode> SpatialSearchIterator::load_node(uint64_t node_id) {
    KVTSpatialAdapter* adapter = KVTSpatialAdapter::get_instance();
    return adapter->load_node(txn_, table_id_, index_id_, node_id);
}

// KVTSpatialAdapter implementation
KVTSpatialAdapter::KVTSpatialAdapter() {}

KVTSpatialAdapter::~KVTSpatialAdapter() {
    node_cache_.clear();
}

KVTSpatialAdapter* KVTSpatialAdapter::get_instance() {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    if (!instance_) {
        instance_ = new KVTSpatialAdapter();
    }
    return instance_;
}

void KVTSpatialAdapter::shutdown() {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    delete instance_;
    instance_ = nullptr;
}

int KVTSpatialAdapter::create_spatial_index(kvt_transaction_t* txn,
                                           uint64_t table_id,
                                           uint32_t index_id) {
    // Create root node
    auto root = std::make_unique<RTreeNode>();
    root->node_id = allocate_node_id(txn, table_id, index_id);
    root->level = 0;  // Start as leaf
    root->entry_count = 0;
    
    // Save root node
    int ret = save_node(txn, table_id, index_id, *root);
    if (ret != 0) {
        return ret;
    }
    
    // Set root node ID in metadata
    ret = set_root_node_id(txn, table_id, index_id, root->node_id);
    if (ret != 0) {
        return ret;
    }
    
    // Initialize statistics
    SpatialIndexStats stats = {1, 1, 0, 1, 0.0};
    uint8_t key_buf[256];
    size_t key_len;
    make_meta_key(key_buf, &key_len, table_id, index_id, META_INDEX_STATS);
    
    return kvt_set(txn, key_buf, key_len,
                   reinterpret_cast<uint8_t*>(&stats), sizeof(stats));
}

int KVTSpatialAdapter::drop_spatial_index(kvt_transaction_t* txn,
                                         uint64_t table_id,
                                         uint32_t index_id) {
    // Clear cache for this index
    node_cache_.clear();
    
    // Delete all nodes by scanning the keyspace
    uint8_t start_key[256], end_key[256];
    size_t start_len, end_len;
    
    // Create range for all nodes of this index
    make_node_key(start_key, &start_len, table_id, index_id, 0);
    make_node_key(end_key, &end_len, table_id, index_id, UINT64_MAX);
    
    kvt_scan_t* scan = kvt_scan_init(txn, start_key, start_len,
                                     end_key, end_len, 1000);
    if (!scan) {
        return -1;
    }
    
    std::vector<std::vector<uint8_t>> keys_to_delete;
    uint8_t* key;
    size_t key_size;
    uint8_t* value;
    size_t value_size;
    
    while (kvt_scan_next(scan, &key, &key_size, &value, &value_size) == 0) {
        keys_to_delete.emplace_back(key, key + key_size);
    }
    kvt_scan_close(scan);
    
    // Delete all found keys
    for (const auto& key_vec : keys_to_delete) {
        kvt_delete(txn, key_vec.data(), key_vec.size());
    }
    
    // Delete metadata
    make_meta_key(start_key, &start_len, table_id, index_id, META_ROOT_NODE);
    kvt_delete(txn, start_key, start_len);
    
    make_meta_key(start_key, &start_len, table_id, index_id, META_NEXT_NODE_ID);
    kvt_delete(txn, start_key, start_len);
    
    make_meta_key(start_key, &start_len, table_id, index_id, META_INDEX_STATS);
    kvt_delete(txn, start_key, start_len);
    
    return 0;
}

int KVTSpatialAdapter::insert_spatial(kvt_transaction_t* txn,
                                     uint64_t table_id,
                                     uint32_t index_id,
                                     uint64_t row_id,
                                     const MBR& mbr) {
    InsertContext ctx;
    ctx.txn = txn;
    ctx.table_id = table_id;
    ctx.index_id = index_id;
    
    uint64_t root_id = get_root_node_id(txn, table_id, index_id);
    if (root_id == 0) {
        // Index doesn't exist, create it
        int ret = create_spatial_index(txn, table_id, index_id);
        if (ret != 0) {
            return ret;
        }
        root_id = get_root_node_id(txn, table_id, index_id);
    }
    
    RTreeEntry new_entry(mbr, row_id);
    return insert_internal(ctx, root_id, 255, new_entry);
}

int KVTSpatialAdapter::insert_internal(InsertContext& ctx,
                                      uint64_t node_id,
                                      uint8_t level,
                                      const RTreeEntry& entry) {
    auto node = load_node(ctx.txn, ctx.table_id, ctx.index_id, node_id);
    if (!node) {
        return -1;
    }
    
    ctx.path.push_back(node_id);
    
    if (node->is_leaf()) {
        // Insert into leaf node
        if (!node->is_full()) {
            node->entries.push_back(entry);
            node->entry_count++;
            node->recalculate_mbr();
            return save_node(ctx.txn, ctx.table_id, ctx.index_id, *node);
        } else {
            // Node is full, need to split
            auto split_result = split_node(*node, entry);
            if (!split_result) {
                return -1;
            }
            
            // Save both new nodes
            int ret = save_node(ctx.txn, ctx.table_id, ctx.index_id,
                              *split_result->left_node);
            if (ret != 0) return ret;
            
            ret = save_node(ctx.txn, ctx.table_id, ctx.index_id,
                          *split_result->right_node);
            if (ret != 0) return ret;
            
            // Adjust tree upwards
            adjust_tree(ctx, split_result->left_node->node_id,
                       split_result->right_node->node_id,
                       split_result->left_mbr, split_result->right_mbr);
        }
    } else {
        // Choose subtree for insertion
        int best_index = choose_subtree(*node, entry.mbr);
        ctx.path_indices.push_back(best_index);
        
        uint64_t child_id = node->entries[best_index].ptr.child_id;
        int ret = insert_internal(ctx, child_id, node->level - 1, entry);
        
        if (ret == 0) {
            // Update parent MBR if needed
            auto child = load_node(ctx.txn, ctx.table_id, ctx.index_id, child_id);
            if (child) {
                node->entries[best_index].mbr = child->node_mbr;
                node->recalculate_mbr();
                save_node(ctx.txn, ctx.table_id, ctx.index_id, *node);
            }
        }
        
        return ret;
    }
    
    return 0;
}

std::unique_ptr<SplitResult> KVTSpatialAdapter::split_node(const RTreeNode& node,
                                                          const RTreeEntry& new_entry) {
    auto result = std::make_unique<SplitResult>();
    
    // Collect all entries including the new one
    std::vector<RTreeEntry> all_entries = node.entries;
    all_entries.push_back(new_entry);
    
    // Pick seeds using quadratic split
    int seed1, seed2;
    pick_seeds(all_entries, seed1, seed2);
    
    // Initialize two groups
    result->left_node = std::make_unique<RTreeNode>();
    result->right_node = std::make_unique<RTreeNode>();
    
    result->left_node->level = node.level;
    result->right_node->level = node.level;
    
    result->left_node->entries.push_back(all_entries[seed1]);
    result->right_node->entries.push_back(all_entries[seed2]);
    
    MBR left_mbr = all_entries[seed1].mbr;
    MBR right_mbr = all_entries[seed2].mbr;
    
    // Distribute remaining entries
    std::vector<bool> assigned(all_entries.size(), false);
    assigned[seed1] = true;
    assigned[seed2] = true;
    
    while (result->left_node->entries.size() + result->right_node->entries.size() 
           < all_entries.size()) {
        
        // Check if one group needs all remaining entries
        size_t remaining = all_entries.size() - 
                          result->left_node->entries.size() - 
                          result->right_node->entries.size();
        
        if (result->left_node->entries.size() + remaining <= MIN_NODE_ENTRIES) {
            // Assign all remaining to left
            for (size_t i = 0; i < all_entries.size(); ++i) {
                if (!assigned[i]) {
                    result->left_node->entries.push_back(all_entries[i]);
                    left_mbr = mbr_union(left_mbr, all_entries[i].mbr);
                    assigned[i] = true;
                }
            }
            break;
        }
        
        if (result->right_node->entries.size() + remaining <= MIN_NODE_ENTRIES) {
            // Assign all remaining to right
            for (size_t i = 0; i < all_entries.size(); ++i) {
                if (!assigned[i]) {
                    result->right_node->entries.push_back(all_entries[i]);
                    right_mbr = mbr_union(right_mbr, all_entries[i].mbr);
                    assigned[i] = true;
                }
            }
            break;
        }
        
        // Pick next entry to assign
        int next_entry = -1;
        double max_diff = -std::numeric_limits<double>::infinity();
        bool assign_to_left = true;
        
        for (size_t i = 0; i < all_entries.size(); ++i) {
            if (assigned[i]) continue;
            
            MBR new_left = mbr_union(left_mbr, all_entries[i].mbr);
            MBR new_right = mbr_union(right_mbr, all_entries[i].mbr);
            
            double left_enlargement = mbr_area(new_left) - mbr_area(left_mbr);
            double right_enlargement = mbr_area(new_right) - mbr_area(right_mbr);
            
            double diff = std::abs(left_enlargement - right_enlargement);
            
            if (diff > max_diff) {
                max_diff = diff;
                next_entry = i;
                assign_to_left = (left_enlargement < right_enlargement);
            }
        }
        
        if (next_entry >= 0) {
            if (assign_to_left) {
                result->left_node->entries.push_back(all_entries[next_entry]);
                left_mbr = mbr_union(left_mbr, all_entries[next_entry].mbr);
            } else {
                result->right_node->entries.push_back(all_entries[next_entry]);
                right_mbr = mbr_union(right_mbr, all_entries[next_entry].mbr);
            }
            assigned[next_entry] = true;
        }
    }
    
    // Set node properties
    result->left_node->node_id = node.node_id;
    result->left_node->entry_count = result->left_node->entries.size();
    result->left_node->recalculate_mbr();
    
    result->right_node->node_id = allocate_node_id(ctx.txn, ctx.table_id, ctx.index_id);
    result->right_node->entry_count = result->right_node->entries.size();
    result->right_node->recalculate_mbr();
    
    result->left_mbr = result->left_node->node_mbr;
    result->right_mbr = result->right_node->node_mbr;
    
    return result;
}

int KVTSpatialAdapter::choose_subtree(const RTreeNode& node, const MBR& mbr) {
    int best_index = 0;
    double min_enlargement = std::numeric_limits<double>::infinity();
    double min_area = std::numeric_limits<double>::infinity();
    
    for (size_t i = 0; i < node.entries.size(); ++i) {
        MBR enlarged = mbr_union(node.entries[i].mbr, mbr);
        double enlargement = mbr_area(enlarged) - mbr_area(node.entries[i].mbr);
        
        if (enlargement < min_enlargement ||
            (enlargement == min_enlargement && 
             mbr_area(node.entries[i].mbr) < min_area)) {
            min_enlargement = enlargement;
            min_area = mbr_area(node.entries[i].mbr);
            best_index = i;
        }
    }
    
    return best_index;
}

void KVTSpatialAdapter::adjust_tree(InsertContext& ctx,
                                   uint64_t left_node_id,
                                   uint64_t right_node_id,
                                   const MBR& left_mbr,
                                   const MBR& right_mbr) {
    // If we split the root, create a new root
    if (ctx.path.size() == 1) {
        auto new_root = std::make_unique<RTreeNode>();
        new_root->node_id = allocate_node_id(ctx.txn, ctx.table_id, ctx.index_id);
        new_root->level = 1;  // One level above leaves
        
        new_root->entries.emplace_back(left_mbr, left_node_id);
        new_root->entries.emplace_back(right_mbr, right_node_id);
        new_root->entry_count = 2;
        new_root->recalculate_mbr();
        
        save_node(ctx.txn, ctx.table_id, ctx.index_id, *new_root);
        set_root_node_id(ctx.txn, ctx.table_id, ctx.index_id, new_root->node_id);
    }
    // TODO: Handle splits at higher levels
}

int KVTSpatialAdapter::delete_spatial(kvt_transaction_t* txn,
                                     uint64_t table_id,
                                     uint32_t index_id,
                                     uint64_t row_id,
                                     const MBR& mbr) {
    DeleteContext ctx;
    ctx.txn = txn;
    ctx.table_id = table_id;
    ctx.index_id = index_id;
    ctx.target_row_id = row_id;
    ctx.target_mbr = mbr;
    ctx.found = false;
    
    uint64_t root_id = get_root_node_id(txn, table_id, index_id);
    if (root_id == 0) {
        return -1;  // Index doesn't exist
    }
    
    int ret = delete_internal(ctx, root_id, 255);
    
    if (ret == 0 && ctx.found) {
        // TODO: Condense tree if needed
        return 0;
    }
    
    return ret;
}

int KVTSpatialAdapter::delete_internal(DeleteContext& ctx,
                                      uint64_t node_id,
                                      uint8_t level) {
    auto node = load_node(ctx.txn, ctx.table_id, ctx.index_id, node_id);
    if (!node) {
        return -1;
    }
    
    if (node->is_leaf()) {
        // Search for the entry to delete
        for (auto it = node->entries.begin(); it != node->entries.end(); ++it) {
            if (it->ptr.row_id == ctx.target_row_id &&
                MBR_EQUAL_CMP(&it->mbr, &ctx.target_mbr)) {
                node->entries.erase(it);
                node->entry_count--;
                node->recalculate_mbr();
                ctx.found = true;
                return save_node(ctx.txn, ctx.table_id, ctx.index_id, *node);
            }
        }
    } else {
        // Search in children
        for (const auto& entry : node->entries) {
            if (MBR_INTERSECT_CMP(&entry.mbr, &ctx.target_mbr)) {
                int ret = delete_internal(ctx, entry.ptr.child_id, node->level - 1);
                if (ret == 0 && ctx.found) {
                    // Update parent MBR if needed
                    auto child = load_node(ctx.txn, ctx.table_id, ctx.index_id,
                                         entry.ptr.child_id);
                    if (child) {
                        // Find and update the entry
                        for (auto& e : node->entries) {
                            if (e.ptr.child_id == entry.ptr.child_id) {
                                e.mbr = child->node_mbr;
                                break;
                            }
                        }
                        node->recalculate_mbr();
                        save_node(ctx.txn, ctx.table_id, ctx.index_id, *node);
                    }
                    return 0;
                }
            }
        }
    }
    
    return ctx.found ? 0 : -1;
}

int KVTSpatialAdapter::update_spatial(kvt_transaction_t* txn,
                                     uint64_t table_id,
                                     uint32_t index_id,
                                     uint64_t row_id,
                                     const MBR& old_mbr,
                                     const MBR& new_mbr) {
    // Simple implementation: delete then insert
    int ret = delete_spatial(txn, table_id, index_id, row_id, old_mbr);
    if (ret != 0) {
        return ret;
    }
    
    return insert_spatial(txn, table_id, index_id, row_id, new_mbr);
}

std::unique_ptr<SpatialSearchIterator> KVTSpatialAdapter::search(
    kvt_transaction_t* txn,
    uint64_t table_id,
    uint32_t index_id,
    const MBR& search_mbr,
    SpatialPredicate predicate) {
    
    return std::make_unique<SpatialSearchIterator>(txn, table_id, index_id,
                                                   search_mbr, predicate);
}

int KVTSpatialAdapter::bulk_load(kvt_transaction_t* txn,
                                uint64_t table_id,
                                uint32_t index_id,
                                const std::vector<std::pair<uint64_t, MBR>>& entries) {
    // Simple implementation: insert one by one
    // TODO: Implement STR (Sort-Tile-Recursive) bulk loading
    for (const auto& entry : entries) {
        int ret = insert_spatial(txn, table_id, index_id,
                               entry.first, entry.second);
        if (ret != 0) {
            return ret;
        }
    }
    return 0;
}

int KVTSpatialAdapter::get_index_stats(kvt_transaction_t* txn,
                                      uint64_t table_id,
                                      uint32_t index_id,
                                      SpatialIndexStats* stats) {
    uint8_t key_buf[256];
    size_t key_len;
    make_meta_key(key_buf, &key_len, table_id, index_id, META_INDEX_STATS);
    
    uint8_t* value;
    size_t value_len;
    int ret = kvt_get(txn, key_buf, key_len, &value, &value_len);
    
    if (ret == 0 && value_len == sizeof(SpatialIndexStats)) {
        memcpy(stats, value, sizeof(SpatialIndexStats));
        return 0;
    }
    
    return -1;
}

int KVTSpatialAdapter::validate_index(kvt_transaction_t* txn,
                                     uint64_t table_id,
                                     uint32_t index_id) {
    // TODO: Implement index validation
    // Check tree consistency, MBR correctness, etc.
    return 0;
}

std::unique_ptr<RTreeNode> KVTSpatialAdapter::load_node(kvt_transaction_t* txn,
                                                       uint64_t table_id,
                                                       uint32_t index_id,
                                                       uint64_t node_id) {
    uint8_t key_buf[256];
    size_t key_len;
    make_node_key(key_buf, &key_len, table_id, index_id, node_id);
    
    uint8_t* value;
    size_t value_len;
    int ret = kvt_get(txn, key_buf, key_len, &value, &value_len);
    
    if (ret == 0) {
        return deserialize_node(value, value_len);
    }
    
    return nullptr;
}

int KVTSpatialAdapter::save_node(kvt_transaction_t* txn,
                                uint64_t table_id,
                                uint32_t index_id,
                                const RTreeNode& node) {
    uint8_t key_buf[256];
    size_t key_len;
    make_node_key(key_buf, &key_len, table_id, index_id, node.node_id);
    
    uint8_t value_buf[65536];  // Max node size
    size_t value_len = serialize_node(node, value_buf);
    
    return kvt_set(txn, key_buf, key_len, value_buf, value_len);
}

int KVTSpatialAdapter::delete_node(kvt_transaction_t* txn,
                                  uint64_t table_id,
                                  uint32_t index_id,
                                  uint64_t node_id) {
    uint8_t key_buf[256];
    size_t key_len;
    make_node_key(key_buf, &key_len, table_id, index_id, node_id);
    
    return kvt_delete(txn, key_buf, key_len);
}

uint64_t KVTSpatialAdapter::get_root_node_id(kvt_transaction_t* txn,
                                            uint64_t table_id,
                                            uint32_t index_id) {
    uint8_t key_buf[256];
    size_t key_len;
    make_meta_key(key_buf, &key_len, table_id, index_id, META_ROOT_NODE);
    
    uint8_t* value;
    size_t value_len;
    int ret = kvt_get(txn, key_buf, key_len, &value, &value_len);
    
    if (ret == 0 && value_len == sizeof(uint64_t)) {
        uint64_t root_id;
        memcpy(&root_id, value, sizeof(uint64_t));
        return root_id;
    }
    
    return 0;
}

int KVTSpatialAdapter::set_root_node_id(kvt_transaction_t* txn,
                                       uint64_t table_id,
                                       uint32_t index_id,
                                       uint64_t root_id) {
    uint8_t key_buf[256];
    size_t key_len;
    make_meta_key(key_buf, &key_len, table_id, index_id, META_ROOT_NODE);
    
    return kvt_set(txn, key_buf, key_len,
                   reinterpret_cast<uint8_t*>(&root_id), sizeof(root_id));
}

uint64_t KVTSpatialAdapter::allocate_node_id(kvt_transaction_t* txn,
                                            uint64_t table_id,
                                            uint32_t index_id) {
    uint8_t key_buf[256];
    size_t key_len;
    make_meta_key(key_buf, &key_len, table_id, index_id, META_NEXT_NODE_ID);
    
    uint64_t next_id = 1;
    uint8_t* value;
    size_t value_len;
    
    int ret = kvt_get(txn, key_buf, key_len, &value, &value_len);
    if (ret == 0 && value_len == sizeof(uint64_t)) {
        memcpy(&next_id, value, sizeof(uint64_t));
    }
    
    uint64_t allocated_id = next_id++;
    kvt_set(txn, key_buf, key_len,
            reinterpret_cast<uint8_t*>(&next_id), sizeof(next_id));
    
    return allocated_id;
}

void KVTSpatialAdapter::make_node_key(uint8_t* key_buf, size_t* key_len,
                                     uint64_t table_id, uint32_t index_id,
                                     uint64_t node_id) {
    key_buf[0] = SPATIAL_INDEX_KEYSPACE;
    memcpy(key_buf + 1, &table_id, sizeof(table_id));
    memcpy(key_buf + 9, &index_id, sizeof(index_id));
    memcpy(key_buf + 13, &node_id, sizeof(node_id));
    *key_len = 21;
}

void KVTSpatialAdapter::make_meta_key(uint8_t* key_buf, size_t* key_len,
                                     uint64_t table_id, uint32_t index_id,
                                     MetaType meta_type) {
    key_buf[0] = SPATIAL_META_KEYSPACE;
    memcpy(key_buf + 1, &table_id, sizeof(table_id));
    memcpy(key_buf + 9, &index_id, sizeof(index_id));
    key_buf[13] = static_cast<uint8_t>(meta_type);
    *key_len = 14;
}

size_t KVTSpatialAdapter::serialize_node(const RTreeNode& node, uint8_t* buffer) {
    size_t offset = 0;
    
    // Serialize node metadata
    memcpy(buffer + offset, &node.node_id, sizeof(node.node_id));
    offset += sizeof(node.node_id);
    
    buffer[offset++] = node.level;
    
    uint16_t entry_count = node.entries.size();
    memcpy(buffer + offset, &entry_count, sizeof(entry_count));
    offset += sizeof(entry_count);
    
    // Serialize node MBR
    memcpy(buffer + offset, &node.node_mbr.xmin, sizeof(double));
    offset += sizeof(double);
    memcpy(buffer + offset, &node.node_mbr.ymin, sizeof(double));
    offset += sizeof(double);
    memcpy(buffer + offset, &node.node_mbr.xmax, sizeof(double));
    offset += sizeof(double);
    memcpy(buffer + offset, &node.node_mbr.ymax, sizeof(double));
    offset += sizeof(double);
    
    // Serialize entries
    for (const auto& entry : node.entries) {
        // MBR
        memcpy(buffer + offset, &entry.mbr.xmin, sizeof(double));
        offset += sizeof(double);
        memcpy(buffer + offset, &entry.mbr.ymin, sizeof(double));
        offset += sizeof(double);
        memcpy(buffer + offset, &entry.mbr.xmax, sizeof(double));
        offset += sizeof(double);
        memcpy(buffer + offset, &entry.mbr.ymax, sizeof(double));
        offset += sizeof(double);
        
        // Pointer (row_id or child_id)
        memcpy(buffer + offset, &entry.ptr.row_id, sizeof(uint64_t));
        offset += sizeof(uint64_t);
    }
    
    return offset;
}

std::unique_ptr<RTreeNode> KVTSpatialAdapter::deserialize_node(const uint8_t* buffer,
                                                              size_t len) {
    if (len < sizeof(uint64_t) + 1 + sizeof(uint16_t) + 4 * sizeof(double)) {
        return nullptr;
    }
    
    auto node = std::make_unique<RTreeNode>();
    size_t offset = 0;
    
    // Deserialize node metadata
    memcpy(&node->node_id, buffer + offset, sizeof(node->node_id));
    offset += sizeof(node->node_id);
    
    node->level = buffer[offset++];
    
    memcpy(&node->entry_count, buffer + offset, sizeof(node->entry_count));
    offset += sizeof(node->entry_count);
    
    // Deserialize node MBR
    memcpy(&node->node_mbr.xmin, buffer + offset, sizeof(double));
    offset += sizeof(double);
    memcpy(&node->node_mbr.ymin, buffer + offset, sizeof(double));
    offset += sizeof(double);
    memcpy(&node->node_mbr.xmax, buffer + offset, sizeof(double));
    offset += sizeof(double);
    memcpy(&node->node_mbr.ymax, buffer + offset, sizeof(double));
    offset += sizeof(double);
    
    // Deserialize entries
    for (uint16_t i = 0; i < node->entry_count; ++i) {
        if (offset + 4 * sizeof(double) + sizeof(uint64_t) > len) {
            break;
        }
        
        RTreeEntry entry;
        
        // MBR
        memcpy(&entry.mbr.xmin, buffer + offset, sizeof(double));
        offset += sizeof(double);
        memcpy(&entry.mbr.ymin, buffer + offset, sizeof(double));
        offset += sizeof(double);
        memcpy(&entry.mbr.xmax, buffer + offset, sizeof(double));
        offset += sizeof(double);
        memcpy(&entry.mbr.ymax, buffer + offset, sizeof(double));
        offset += sizeof(double);
        
        // Pointer
        memcpy(&entry.ptr.row_id, buffer + offset, sizeof(uint64_t));
        offset += sizeof(uint64_t);
        
        node->entries.push_back(entry);
    }
    
    return node;
}

void KVTSpatialAdapter::pick_seeds(const std::vector<RTreeEntry>& entries,
                                  int& seed1, int& seed2) {
    double max_waste = -std::numeric_limits<double>::infinity();
    seed1 = 0;
    seed2 = 1;
    
    for (size_t i = 0; i < entries.size(); ++i) {
        for (size_t j = i + 1; j < entries.size(); ++j) {
            double waste = calculate_waste(entries[i].mbr, entries[j].mbr);
            if (waste > max_waste) {
                max_waste = waste;
                seed1 = i;
                seed2 = j;
            }
        }
    }
}

double KVTSpatialAdapter::calculate_waste(const MBR& mbr1, const MBR& mbr2) {
    MBR combined = mbr_union(mbr1, mbr2);
    return mbr_area(combined) - mbr_area(mbr1) - mbr_area(mbr2);
}

// NodeCache implementation
void KVTSpatialAdapter::NodeCache::put(uint64_t node_id,
                                      std::shared_ptr<RTreeNode> node) {
    std::lock_guard<std::mutex> lock(mutex);
    
    if (cache.size() >= max_size) {
        // Simple eviction: remove first element
        cache.erase(cache.begin());
    }
    
    cache[node_id] = node;
}

std::shared_ptr<RTreeNode> KVTSpatialAdapter::NodeCache::get(uint64_t node_id) {
    std::lock_guard<std::mutex> lock(mutex);
    
    auto it = cache.find(node_id);
    if (it != cache.end()) {
        return it->second;
    }
    
    return nullptr;
}

void KVTSpatialAdapter::NodeCache::clear() {
    std::lock_guard<std::mutex> lock(mutex);
    cache.clear();
}

} // namespace kvt_spatial