#ifndef KVT_SPATIAL_ADAPTER_H
#define KVT_SPATIAL_ADAPTER_H

#include <memory>
#include <vector>
#include <queue>
#include <unordered_map>
#include <mutex>
#include "kvt/kvt_inc.h"
#include "../../sql/spatial.h"

namespace kvt_spatial {

// MBR comparison macros (from InnoDB's gis0rtree.h)
#define MBR_CONTAIN_CMP(a, b) \
    ((((b)->xmin >= (a)->xmin) && ((b)->xmax <= (a)->xmax) \
     && ((b)->ymin >= (a)->ymin) && ((b)->ymax <= (a)->ymax)))

#define MBR_EQUAL_CMP(a, b) \
    ((((b)->xmin == (a)->xmin) && ((b)->xmax == (a)->xmax)) \
     && (((b)->ymin == (a)->ymin) && ((b)->ymax == (a)->ymax)))

#define MBR_INTERSECT_CMP(a, b) \
    ((((b)->xmin <= (a)->xmax) && ((b)->xmax >= (a)->xmin)) \
     && (((b)->ymin <= (a)->ymax) && ((b)->ymax >= (a)->ymin)))

#define MBR_DISJOINT_CMP(a, b) (!MBR_INTERSECT_CMP(a, b))

#define MBR_WITHIN_CMP(a, b) \
    ((((b)->xmin <= (a)->xmin) && ((b)->xmax >= (a)->xmax)) \
     && (((b)->ymin <= (a)->ymin) && ((b)->ymax >= (a)->ymax)))

// Maximum entries per R-tree node
constexpr uint16_t MAX_NODE_ENTRIES = 64;
constexpr uint16_t MIN_NODE_ENTRIES = MAX_NODE_ENTRIES / 3;

// Key space identifiers
constexpr uint8_t SPATIAL_INDEX_KEYSPACE = 0x05;
constexpr uint8_t SPATIAL_META_KEYSPACE = 0x06;

// Metadata types
enum MetaType : uint8_t {
    META_ROOT_NODE = 0x01,
    META_NEXT_NODE_ID = 0x02,
    META_INDEX_STATS = 0x03
};

// Spatial predicates matching MariaDB's definitions
enum SpatialPredicate {
    SP_INTERSECTS = 0,
    SP_CONTAINS = 1,
    SP_WITHIN = 2,
    SP_EQUALS = 3,
    SP_DISJOINT = 4,
    SP_OVERLAPS = 5,
    SP_TOUCHES = 6,
    SP_CROSSES = 7
};

// R-tree node entry
struct RTreeEntry {
    MBR mbr;
    union {
        uint64_t row_id;    // For leaf nodes
        uint64_t child_id;  // For internal nodes
    } ptr;
    
    RTreeEntry() : mbr() { ptr.row_id = 0; }
    RTreeEntry(const MBR& m, uint64_t id) : mbr(m) { ptr.row_id = id; }
};

// R-tree node
struct RTreeNode {
    uint64_t node_id;
    uint8_t level;           // 0 = leaf, >0 = internal
    uint16_t entry_count;
    MBR node_mbr;
    std::vector<RTreeEntry> entries;
    
    RTreeNode() : node_id(0), level(0), entry_count(0) {
        entries.reserve(MAX_NODE_ENTRIES);
    }
    
    bool is_leaf() const { return level == 0; }
    bool is_full() const { return entry_count >= MAX_NODE_ENTRIES; }
    bool is_underfull() const { return entry_count < MIN_NODE_ENTRIES; }
    
    void recalculate_mbr();
    double calculate_enlargement(const MBR& new_mbr) const;
};

// Split result for node splitting
struct SplitResult {
    std::unique_ptr<RTreeNode> left_node;
    std::unique_ptr<RTreeNode> right_node;
    MBR left_mbr;
    MBR right_mbr;
};

// Index statistics
struct SpatialIndexStats {
    uint64_t total_nodes;
    uint64_t leaf_nodes;
    uint64_t total_entries;
    uint32_t tree_height;
    double avg_node_fill_ratio;
};

// Search iterator for spatial queries
class SpatialSearchIterator {
public:
    SpatialSearchIterator(uint64_t txn_id,
                         uint64_t table_id, 
                         uint32_t index_id,
                         const MBR& search_mbr,
                         SpatialPredicate predicate);
    ~SpatialSearchIterator();
    
    bool get_next(uint64_t& row_id, MBR* mbr = nullptr);
    void reset();
    uint64_t get_count() const { return result_count_; }
    
private:
    struct SearchState {
        uint64_t node_id;
        uint8_t level;
        
        SearchState(uint64_t id, uint8_t lvl) : node_id(id), level(lvl) {}
    };
    
    uint64_t txn_id_;
    uint64_t table_id_;
    uint32_t index_id_;
    MBR search_mbr_;
    SpatialPredicate predicate_;
    
    std::queue<SearchState> search_queue_;
    std::vector<uint64_t> current_results_;
    size_t current_index_;
    uint64_t result_count_;
    
    void search_node(uint64_t node_id, uint8_t level);
    bool matches_predicate(const MBR& mbr) const;
    std::unique_ptr<RTreeNode> load_node(uint64_t node_id);
};

// Main spatial adapter class
class KVTSpatialAdapter {
public:
    static KVTSpatialAdapter* get_instance();
    static void shutdown();
    
    // Index management
    int create_spatial_index(uint64_t txn_id,
                           uint64_t table_id, 
                           uint32_t index_id);
    int drop_spatial_index(uint64_t txn_id,
                          uint64_t table_id, 
                          uint32_t index_id);
    
    // Data operations
    int insert_spatial(uint64_t txn_id,
                      uint64_t table_id, 
                      uint32_t index_id,
                      uint64_t row_id, 
                      const MBR& mbr);
    
    int delete_spatial(uint64_t txn_id,
                      uint64_t table_id, 
                      uint32_t index_id,
                      uint64_t row_id, 
                      const MBR& mbr);
    
    int update_spatial(uint64_t txn_id,
                      uint64_t table_id, 
                      uint32_t index_id,
                      uint64_t row_id,
                      const MBR& old_mbr, 
                      const MBR& new_mbr);
    
    // Search operations
    std::unique_ptr<SpatialSearchIterator> search(
        uint64_t txn_id,
        uint64_t table_id, 
        uint32_t index_id,
        const MBR& search_mbr,
        SpatialPredicate predicate);
    
    // Bulk operations
    int bulk_load(uint64_t txn_id,
                 uint64_t table_id, 
                 uint32_t index_id,
                 const std::vector<std::pair<uint64_t, MBR>>& entries);
    
    // Statistics
    int get_index_stats(uint64_t txn_id,
                       uint64_t table_id, 
                       uint32_t index_id,
                       SpatialIndexStats* stats);
    
    // Maintenance
    int validate_index(uint64_t txn_id,
                      uint64_t table_id, 
                      uint32_t index_id);
    
    // Node I/O operations (made public for SpatialSearchIterator)
    std::unique_ptr<RTreeNode> load_node(uint64_t txn_id,
                                        uint64_t table_id,
                                        uint32_t index_id,
                                        uint64_t node_id);
    
    uint64_t get_root_node_id(uint64_t txn_id,
                            uint64_t table_id,
                            uint32_t index_id);
    
private:
    KVTSpatialAdapter();
    ~KVTSpatialAdapter();
    
    // Singleton instance
    static KVTSpatialAdapter* instance_;
    static std::mutex instance_mutex_;
    
    // Node cache for performance
    struct NodeCache {
        std::unordered_map<uint64_t, std::shared_ptr<RTreeNode>> cache;
        std::mutex mutex;
        size_t max_size = 1000;
        
        void put(uint64_t node_id, std::shared_ptr<RTreeNode> node);
        std::shared_ptr<RTreeNode> get(uint64_t node_id);
        void clear();
    };
    NodeCache node_cache_;
    
    // R-tree operations
    struct InsertContext {
        uint64_t txn_id;
        uint64_t table_id;
        uint32_t index_id;
        std::vector<uint64_t> path;  // Node IDs from root to leaf
        std::vector<size_t> path_indices;  // Entry indices in each node
    };
    
    int insert_internal(InsertContext& ctx,
                       uint64_t node_id,
                       uint8_t level,
                       const RTreeEntry& entry);
    
    std::unique_ptr<SplitResult> split_node(const RTreeNode& node,
                                           const RTreeEntry& new_entry);
    
    int choose_subtree(const RTreeNode& node, const MBR& mbr);
    
    void adjust_tree(InsertContext& ctx,
                    uint64_t left_node_id,
                    uint64_t right_node_id,
                    const MBR& left_mbr,
                    const MBR& right_mbr);
    
    // Node I/O operations (private versions)
    int save_node(uint64_t txn_id,
                 uint64_t table_id,
                 uint32_t index_id,
                 const RTreeNode& node);
    
    int delete_node(uint64_t txn_id,
                   uint64_t table_id,
                   uint32_t index_id,
                   uint64_t node_id);
    
    int set_root_node_id(uint64_t txn_id,
                        uint64_t table_id,
                        uint32_t index_id,
                        uint64_t root_id);
    
    uint64_t allocate_node_id(uint64_t txn_id,
                            uint64_t table_id,
                            uint32_t index_id);
    
    // Utility functions
    void make_node_key(uint8_t* key_buf, size_t* key_len,
                      uint64_t table_id, uint32_t index_id,
                      uint64_t node_id);
    
    void make_meta_key(uint8_t* key_buf, size_t* key_len,
                      uint64_t table_id, uint32_t index_id,
                      MetaType meta_type);
    
    size_t serialize_node(const RTreeNode& node, uint8_t* buffer);
    std::unique_ptr<RTreeNode> deserialize_node(const uint8_t* buffer, size_t len);
    
    // Quadratic split algorithm
    void pick_seeds(const std::vector<RTreeEntry>& entries,
                   int& seed1, int& seed2);
    
    double calculate_waste(const MBR& mbr1, const MBR& mbr2);
    
    // Delete operation helpers
    struct DeleteContext {
        uint64_t txn_id;
        uint64_t table_id;
        uint32_t index_id;
        uint64_t target_row_id;
        MBR target_mbr;
        bool found;
    };
    
    int delete_internal(DeleteContext& ctx,
                       uint64_t node_id,
                       uint8_t level);
    
    int condense_tree(uint64_t txn_id,
                     uint64_t table_id,
                     uint32_t index_id,
                     const std::vector<uint64_t>& path);
};

// Utility functions for MBR operations
inline double mbr_area(const MBR& mbr) {
    return (mbr.xmax - mbr.xmin) * (mbr.ymax - mbr.ymin);
}

inline double mbr_perimeter(const MBR& mbr) {
    return 2.0 * ((mbr.xmax - mbr.xmin) + (mbr.ymax - mbr.ymin));
}

inline MBR mbr_union(const MBR& mbr1, const MBR& mbr2) {
    MBR result;
    result.xmin = std::min(mbr1.xmin, mbr2.xmin);
    result.ymin = std::min(mbr1.ymin, mbr2.ymin);
    result.xmax = std::max(mbr1.xmax, mbr2.xmax);
    result.ymax = std::max(mbr1.ymax, mbr2.ymax);
    return result;
}

inline bool mbr_contains_point(const MBR& mbr, double x, double y) {
    return x >= mbr.xmin && x <= mbr.xmax && 
           y >= mbr.ymin && y <= mbr.ymax;
}

} // namespace kvt_spatial

#endif // KVT_SPATIAL_ADAPTER_H