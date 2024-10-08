#pragma once

#include "base/common.h"
#include "utils/hash.h"
#include "utils/bits.h"
#include "utils/io.h"

__BEGIN_THIRD_PARTY_HEADERS
#include "proto/shared_log.pb.h"
__END_THIRD_PARTY_HEADERS

namespace faas {
namespace log {

// View and its inner class will never change after construction
class View {
public:
    explicit View(const ViewProto& view_proto);
    ~View() {}

    uint16_t id() const { return id_; }

    size_t metalog_replicas() const { return metalog_replicas_; }
    size_t userlog_replicas() const { return userlog_replicas_; }
    size_t index_replicas() const { return index_replicas_; }
    size_t num_index_shards() const { return num_index_shards_; }
    size_t aggregator_replicas() const { return aggregator_replicas_; }
    size_t num_phylogs() const { return num_phylogs_; }

    size_t num_sequencer_nodes() const { return sequencer_node_ids_.size(); }
    size_t num_storage_nodes() const { return storage_node_ids_.size(); }
    size_t num_index_nodes() const { return index_node_ids_.size(); }
    size_t num_aggregator_nodes() const { return aggregator_node_ids_.size(); }
    size_t num_local_storage_shards() const { return local_storage_shard_ids_.size(); }
    size_t num_global_storage_shards() const { return global_storage_shard_ids_.size(); }

    using NodeIdVec = absl::FixedArray<uint16_t>;
    using ShardIdVec = absl::FixedArray<uint32_t>;

    const NodeIdVec& GetSequencerNodes() const { return sequencer_node_ids_; }
    const NodeIdVec& GetStorageNodes() const { return storage_node_ids_; }
    const NodeIdVec& GetIndexNodes() const { return index_node_ids_; }
    const NodeIdVec& GetAggregatorNodes() const { return aggregator_node_ids_; }
    const NodeIdVec& GetLocalStorageShardIds() const {return local_storage_shard_ids_;}
    const ShardIdVec& GetGlobalStorageShardIds() const {return global_storage_shard_ids_;}

    const NodeIdVec& GetStorageShardIds(uint16_t sequencer_node_id) const {
        DCHECK(sequencer_storage_shard_ids_.contains(sequencer_node_id));
        return sequencer_storage_shard_ids_.at(sequencer_node_id);
    }

    bool contains_sequencer_node(uint16_t node_id) const {
        return sequencer_nodes_.contains(node_id);
    }
    bool contains_storage_node(uint16_t node_id) const {
        return storage_nodes_.contains(node_id);
    }
    bool contains_index_node(uint16_t node_id) const {
        return index_nodes_.contains(node_id);
    }
    bool contains_storage_shard_id(uint16_t sequencer_id, uint16_t local_storage_shard_id) const {
        return storage_shard_units_.contains(bits::JoinTwo16(sequencer_id, local_storage_shard_id));
    }
    bool is_active_phylog(uint16_t sequencer_node_id) const {
        return active_phylogs_.contains(sequencer_node_id);
    }

    void GetActiveSequencerNodes(std::vector<uint16_t>* active_sequencers) const {
        for(uint16_t sequencer_node_id : GetSequencerNodes()){
            if(is_active_phylog(sequencer_node_id)){
                active_sequencers->push_back(sequencer_node_id);
            }
        }
    }

    uint32_t LogSpaceIdentifier(uint32_t user_logspace) const {
        uint64_t h = hash::xxHash64(user_logspace, /* seed= */ log_space_hash_seed_);
        uint16_t node_id = log_space_hash_tokens_[h % log_space_hash_tokens_.size()];
        DCHECK(sequencer_nodes_.contains(node_id));
        return bits::JoinTwo16(id_, node_id);
    }

    uint64_t log_space_hash_seed() const { return log_space_hash_seed_; }
    const NodeIdVec& log_space_hash_tokens() const { return log_space_hash_tokens_; }

    class StorageShard {
    public:
        StorageShard(StorageShard&& other) = default;
        ~StorageShard() {}

        uint32_t shard_id() const { return shard_id_; }

        uint16_t local_shard_id() const { return bits::LowHalf32(shard_id_); }

        uint16_t GetSequencerNode() const {
            return sequencer_node_;
        }

        const View::NodeIdVec& GetStorageNodes() const {
            return storage_nodes_;
        }

        bool HasStorageNode(uint16_t storage_node) const {
            for(uint16_t s : storage_nodes_){
                if(s == storage_node){
                    return true;
                }
            }
            return false;
        }

        uint16_t PickStorageNode() const {
            size_t idx = __atomic_fetch_add(&next_storage_node_, 1, __ATOMIC_RELAXED);
            return storage_nodes_.at(idx % storage_nodes_.size());
        }

        size_t PickIndexShard() const {
            size_t sx = __atomic_fetch_add(&next_index_shard, 1, __ATOMIC_RELAXED);
            return sx % view_->num_index_shards_;
        }

        uint16_t PickIndexNode(size_t shard) const {
            View::NodeIdVec index_nodes = index_shard_nodes_.at(shard);
            size_t idx = __atomic_fetch_add(&(next_index_replica_node_.at(shard)), 1, __ATOMIC_RELAXED);
            return index_nodes.at(idx % index_nodes.size());
        }

        uint16_t PickIndexNodeByTag(uint64_t tag) const {
            size_t shard = tag % view_->num_index_shards_;
            View::NodeIdVec index_nodes = index_shard_nodes_.at(shard);
            return index_nodes.at(static_cast<size_t>(std::rand()) % index_nodes.size());
        }

        uint16_t PickAggregatorNode(const std::vector<uint16_t>& sharded_index_nodes) const {
            if (aggregator_nodes_.empty()) {
                return sharded_index_nodes.at(static_cast<size_t>(std::rand()) % sharded_index_nodes.size());
            }
            size_t idx = __atomic_fetch_add(&next_aggregator_node_, 1, __ATOMIC_RELAXED);
            return aggregator_nodes_.at(idx % aggregator_nodes_.size());
        }

        bool UseMasterSlaveMerging() const {
            return aggregator_nodes_.empty();
        }

        void PickIndexNodePerShard(std::vector<uint16_t>& sharded_index_nodes) const {
            size_t first_shard = PickIndexShard();
            for (size_t i = first_shard; i < view_->num_index_shards_ + first_shard ; i++) {
                size_t shard = i % view_->num_index_shards_;
                DCHECK_LE(shard, static_cast<size_t>(view_->num_index_shards_ - 1));
                uint16_t index_node = PickIndexNode(shard);
                sharded_index_nodes.push_back(index_node);
            } 
        }

    private:
        friend class View;
        const View* view_;
        uint32_t shard_id_; /*sequencer_id||storage_shard_id*/

        View::NodeIdVec storage_nodes_;
        View::NodeIdVec aggregator_nodes_;
        uint16_t sequencer_node_;

        mutable std::vector<size_t> next_index_replica_node_;
        std::vector<View::NodeIdVec> index_shard_nodes_;

        mutable size_t next_index_shard;
        mutable size_t next_storage_node_;
        mutable size_t next_aggregator_node_;

        StorageShard(const View* view, uint32_t shard_id,
               const View::NodeIdVec& storage_nodes,
               const uint16_t sequencer_node,
               const std::vector<View::NodeIdVec>& index_shard_nodes,
               const View::NodeIdVec& aggregator_nodes);
        DISALLOW_IMPLICIT_CONSTRUCTORS(StorageShard);
    };

    const StorageShard* GetStorageShard(uint32_t shard_id) const {
        DCHECK(storage_shard_units_.contains(shard_id));
        return storage_shard_units_.at(shard_id);
    }

    class Sequencer {
    public:
        Sequencer(Sequencer&& other) = default;
        ~Sequencer() {}

        const View* view() const { return view_; }
        uint16_t node_id() const { return node_id_; }

        const View::NodeIdVec& GetStorageShardIds() const {
            return view_->GetStorageShardIds(node_id_);
        }

        const View::NodeIdVec& GetReplicaSequencerNodes() const {
            return replica_sequencer_nodes_;
        }

        bool IsReplicaSequencerNode(uint16_t sequencer_node_id) const {
            return replica_sequencer_node_set_.contains(sequencer_node_id);
        }

    private:
        friend class View;
        const View* view_;
        uint16_t node_id_;

        View::NodeIdVec replica_sequencer_nodes_;
        absl::flat_hash_set<uint16_t> replica_sequencer_node_set_;

        Sequencer(const View* view, uint16_t node_id,
                  const View::NodeIdVec& replica_sequencer_nodes);
        DISALLOW_IMPLICIT_CONSTRUCTORS(Sequencer);
    };

    const Sequencer* GetSequencerNode(uint16_t node_id) const {
        DCHECK(sequencer_nodes_.contains(node_id));
        return sequencer_nodes_.at(node_id);
    }

    class Storage {
    public:
        Storage(Storage&& other) = default;
        ~Storage() {}

        const View* view() const { return view_; }
        uint16_t node_id() const { return node_id_; }

        // aka my shards
        const View::ShardIdVec& GetStorageShardIds() const {
            return storage_shard_ids_;
        }

        const View::NodeIdVec& GetLocalStorageShardIds(uint16_t sequencer_id) const {
            return local_storage_shard_ids_.at(sequencer_id);
        }

        bool IsStorageShardMember(uint32_t storage_shard_id) const {
            //TODO: improve
            for(uint32_t s : GetStorageShardIds()){
                if(s == storage_shard_id){
                    return true;
                }
            }
            return false;
        }


        uint16_t PickIndexShard() const {
            size_t idx = __atomic_fetch_add(&next_index_shard_, 1, __ATOMIC_RELAXED);
            return gsl::narrow_cast<uint16_t>(idx % view_->num_index_shards());
        }

    private:
        friend class View;
        const View* view_;
        uint16_t node_id_;

        View::ShardIdVec storage_shard_ids_;
        absl::flat_hash_map<uint16_t, View::NodeIdVec> local_storage_shard_ids_;
        std::vector<View::NodeIdVec> index_shard_nodes_;
        mutable size_t next_index_shard_;
        mutable size_t next_index_data_sender;

        Storage(const View* view, uint16_t node_id,
                const View::ShardIdVec storage_shard_ids,
                const std::vector<View::NodeIdVec>& index_shard_nodes);
        DISALLOW_IMPLICIT_CONSTRUCTORS(Storage);
    };

    const Storage* GetStorageNode(uint16_t node_id) const {
        DCHECK(storage_nodes_.contains(node_id));
        return storage_nodes_.at(node_id);
    }

    class Index {
    public:
        Index(Index&& other) = default;
        ~Index() {}

        const View* view() const { return view_; }
        uint16_t node_id() const { return node_id_; }

        uint16_t PickStorageNode(uint32_t storage_shard_id) const {
            size_t next_storage_node_ = next_shard_storage_node_.at(storage_shard_id);
            size_t idx = __atomic_fetch_add(&next_storage_node_, 1, __ATOMIC_RELAXED);
            size_t storage_node_pos = idx % view_->userlog_replicas_;
            LOG_F(INFO, "Use storage node at position {}", storage_node_pos);
            std::vector<uint16_t> storage_nodes = per_shard_storage_nodes_.at(storage_shard_id);
            DCHECK_EQ(view_->userlog_replicas_, storage_nodes.size());
            return storage_nodes.at(storage_node_pos);
        }

        bool IsIndexShardMember(uint16_t index_shard) const {
            return index_shards_.contains(index_shard);
        }

    private:
        friend class View;
        const View* view_;
        uint16_t node_id_;

        //TODO: should be NodeIdVec
        absl::flat_hash_map<uint32_t, std::vector<uint16_t>> per_shard_storage_nodes_;
        absl::flat_hash_map<uint32_t, size_t> next_shard_storage_node_;
        absl::flat_hash_set<uint16_t> index_shards_;

        Index(const View* view, uint16_t node_id,
              const absl::flat_hash_map<uint32_t, std::vector<uint16_t>>& storage_shard_nodes,
              const absl::flat_hash_set<uint16_t>& index_shards);
        DISALLOW_IMPLICIT_CONSTRUCTORS(Index);
    };

    const Index* GetIndexNode(uint16_t node_id) const {
        DCHECK(index_nodes_.contains(node_id));
        return index_nodes_.at(node_id);
    }

private:
    uint16_t id_;

    size_t metalog_replicas_;
    size_t userlog_replicas_;
    size_t index_replicas_;
    size_t aggregator_replicas_;
    size_t num_index_shards_;
    size_t num_phylogs_;
    size_t storage_shards_per_sequencer_;

    NodeIdVec sequencer_node_ids_;
    NodeIdVec storage_node_ids_;
    NodeIdVec index_node_ids_;
    NodeIdVec aggregator_node_ids_;
    NodeIdVec local_storage_shard_ids_;
    ShardIdVec global_storage_shard_ids_;

    absl::flat_hash_map</*sequencer_id*/uint16_t, NodeIdVec> sequencer_storage_shard_ids_;

    absl::flat_hash_set<uint16_t> active_phylogs_;

    absl::InlinedVector<Sequencer, 16> sequencers_;
    absl::InlinedVector<Storage, 16>   storages_;
    absl::InlinedVector<Index, 16>     indexes_;
    absl::InlinedVector<StorageShard, 16> storage_shards_;

    absl::flat_hash_map<uint32_t, StorageShard*>    storage_shard_units_;

    absl::flat_hash_map<uint16_t, Sequencer*> sequencer_nodes_;
    absl::flat_hash_map<uint16_t, Storage*>   storage_nodes_;
    absl::flat_hash_map<uint16_t, Index*>     index_nodes_;

    uint64_t  log_space_hash_seed_;
    NodeIdVec log_space_hash_tokens_;

    DISALLOW_COPY_AND_ASSIGN(View);
};

}  // namespace log
}  // namespace faas
