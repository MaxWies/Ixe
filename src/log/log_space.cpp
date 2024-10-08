#include "log/log_space.h"

#include "log/flags.h"

namespace faas {
namespace log {

MetaLogPrimary::MetaLogPrimary(const View* view, uint16_t sequencer_id)
    : LogSpaceBase(LogSpaceBase::kFullMode, view, sequencer_id),
      replicated_metalog_position_(0) {
    for (uint16_t storage_shard_id : sequencer_node_->GetStorageShardIds()) {
        const View::StorageShard* storage_shard = view_->GetStorageShard(bits::JoinTwo16(sequencer_id, storage_shard_id));
        for (uint16_t storage_id : storage_shard->GetStorageNodes()) {
            auto pair = std::make_pair(storage_shard_id, storage_id);
            shard_progrsses_[pair] = 0;
        }
        last_cut_[storage_shard_id] = 0;
    }
    for (uint16_t sequencer_id : sequencer_node_->GetReplicaSequencerNodes()) {
        metalog_progresses_[sequencer_id] = 0;
    }
    log_header_ = fmt::format("MetaLogPrimary[{}]: ", view->id());
    if (metalog_progresses_.empty()) {
        HLOG(WARNING) << "No meta log replication";
    }
    state_ = kNormal;
}

MetaLogPrimary::~MetaLogPrimary() {}

bool MetaLogPrimary::BlockShard(uint16_t shard_id, uint32_t* last_cut) {
    if(!last_cut_.contains(shard_id)){
        HLOG_F(ERROR, "Shard={} is not known", shard_id);
        *last_cut = 0;
        return false;
    }
    if(!unblocked_shards_.contains(shard_id)){
        HLOG_F(INFO, "Shard={} is already blocked", shard_id);
        *last_cut = last_cut_.at(shard_id);
        return true;
    }
    if(dirty_shards_.contains(shard_id)){
        dirty_shards_.erase(shard_id);
        HLOG_F(INFO, "Shard={} was dirty", shard_id);
    }
    HLOG_F(INFO, "Block shard={}", shard_id);
    unblocked_shards_.erase(shard_id);
    blocking_change_ = true;
    *last_cut = last_cut_.at(shard_id);
    return true;
}

bool MetaLogPrimary::UnblockShard(uint16_t shard_id, uint32_t* last_cut) {
    if(!last_cut_.contains(shard_id)){
        HLOG_F(ERROR, "Shard={} is not known", shard_id);
        *last_cut = 0;
        return false;
    }
    if(unblocked_shards_.contains(shard_id)){
        HLOG_F(WARNING, "Shard={} is already unblocked", shard_id);
        *last_cut = 0;
        return false;
    }
    HLOG_F(INFO, "Unblock shard={}", shard_id);
    unblocked_shards_.insert(shard_id);
    blocking_change_ = true;
    *last_cut = last_cut_.at(shard_id);
    return true;
}

void MetaLogPrimary::UpdateStorageProgress(uint16_t storage_id,
                                           const std::vector<uint32_t>& progress) {
    if (!view_->contains_storage_node(storage_id)) {
        HLOG_F(FATAL, "View {} does not has storage node {}", view_->id(), storage_id);
    }
    const View::Storage* storage_node = view_->GetStorageNode(storage_id);
    const View::ShardIdVec& storage_shard_ids = storage_node->GetStorageShardIds();
    if (progress.size() != storage_shard_ids.size()) {
        HLOG_F(FATAL, "Size does not match: have={}, expected={}",
               progress.size(), storage_shard_ids.size());
    }
    for (size_t i = 0; i < progress.size(); i++) {
        uint16_t storage_shard_id = bits::LowHalf32(storage_shard_ids[i]);
        if(!unblocked_shards_.contains(storage_shard_id)){
            HVLOG_F(1, "Shard {} is blocked", storage_shard_id);
            continue;
        }
        auto pair = std::make_pair(storage_shard_id, storage_id);
        DCHECK(shard_progrsses_.contains(pair));
        if (progress[i] > shard_progrsses_[pair]) {
            shard_progrsses_[pair] = progress[i];
            uint32_t current_position = GetShardReplicatedPosition(storage_shard_id);
            DCHECK_GE(current_position, last_cut_.at(storage_shard_id));
            if (current_position > last_cut_.at(storage_shard_id)) {
                HVLOG_F(1, "Store progress from storage {} for storage_shard {}: {}",
                        storage_id, storage_shard_id, bits::HexStr0x(current_position));
                dirty_shards_.insert(storage_shard_id);
            }
        }
    }
}

void MetaLogPrimary::UpdateReplicaProgress(uint16_t sequencer_id,
                                           uint32_t metalog_position) {
    if (!sequencer_node_->IsReplicaSequencerNode(sequencer_id)) {
        HLOG_F(FATAL, "Should not receive META_PROG message from sequencer {}", sequencer_id);
    }
    if (metalog_position > metalog_position_) {
        HLOG_F(FATAL, "Receive future position: received={}, current={}",
               metalog_position, metalog_position_);
    }
    DCHECK(metalog_progresses_.contains(sequencer_id));
    if (metalog_position > metalog_progresses_[sequencer_id]) {
        metalog_progresses_[sequencer_id] = metalog_position;
        UpdateMetaLogReplicatedPosition();
    }
}

std::optional<MetaLogProto> MetaLogPrimary::MarkNextCut() {
    if (dirty_shards_.empty()) {
        return std::nullopt;
    }
    MetaLogProto meta_log_proto;
    meta_log_proto.set_logspace_id(identifier());
    meta_log_proto.set_metalog_seqnum(metalog_position());
    meta_log_proto.set_type(MetaLogProto::NEW_LOGS);
    auto* new_logs_proto = meta_log_proto.mutable_new_logs_proto();
    new_logs_proto->set_start_seqnum(bits::LowHalf64(seqnum_position()));
    uint32_t total_delta = 0;
    for (uint16_t shard_id : dirty_shards_) {
        new_logs_proto->add_shard_ids(shard_id);
        new_logs_proto->add_shard_starts(last_cut_.at(shard_id));
        uint32_t current_position = GetShardReplicatedPosition(shard_id);
        DCHECK_GT(current_position, last_cut_.at(shard_id));
        uint32_t delta = current_position - last_cut_.at(shard_id);
        last_cut_[shard_id] = current_position;
        new_logs_proto->add_shard_deltas(delta);
        total_delta += delta;
    }
    blocking_change_ = false;
    dirty_shards_.clear();
    HVLOG_F(1, "Generate new NEW_LOGS meta log: start_seqnum={}, total_delta={}",
            new_logs_proto->start_seqnum(), total_delta);
    if (!ProvideMetaLog(meta_log_proto)) {
        HLOG(FATAL) << "Failed to advance metalog position";
    }
    DCHECK_EQ(new_logs_proto->start_seqnum() + total_delta,
              bits::LowHalf64(seqnum_position()));
    return meta_log_proto;
}

void MetaLogPrimary::UpdateMetaLogReplicatedPosition() {
    if (replicated_metalog_position_ == metalog_position_) {
        return;
    }
    if (metalog_progresses_.empty()) {
        return;
    }
    std::vector<uint32_t> tmp;
    tmp.reserve(metalog_progresses_.size());
    for (const auto& [sequencer_id, progress] : metalog_progresses_) {
        tmp.push_back(progress);
    }
    absl::c_sort(tmp);
    uint32_t progress = tmp.at(tmp.size() / 2);
    DCHECK_GE(progress, replicated_metalog_position_);
    DCHECK_LE(progress, metalog_position_);
    replicated_metalog_position_ = progress;
}

uint32_t MetaLogPrimary::GetShardReplicatedPosition(uint16_t storage_shard_id) const {
    uint32_t min_value = std::numeric_limits<uint32_t>::max();
    const View::StorageShard* storage_shard = view_->GetStorageShard(bits::JoinTwo16(sequencer_node_->node_id(), storage_shard_id));
    for (uint16_t storage_id : storage_shard->GetStorageNodes()) {
        auto pair = std::make_pair(storage_shard_id, storage_id);
        DCHECK(shard_progrsses_.contains(pair));
        min_value = std::min(min_value, shard_progrsses_.at(pair));
    }
    DCHECK_LT(min_value, std::numeric_limits<uint32_t>::max());
    return min_value;
}

MetaLogBackup::MetaLogBackup(const View* view, uint16_t sequencer_id)
    : LogSpaceBase(LogSpaceBase::kFullMode, view, sequencer_id) {
    log_header_ = fmt::format("MetaLogBackup[{}-{}]: ", view->id(), sequencer_id);
    state_ = kNormal;
}

MetaLogBackup::~MetaLogBackup() {}

LogProducer::LogProducer(uint16_t storage_shard_id, const View* view, uint16_t sequencer_id, uint32_t metalog_position, uint32_t next_start_id)
    : LogSpaceBase(LogSpaceBase::kLogProducer, view, sequencer_id),
      next_localid_(bits::JoinTwo32(storage_shard_id, next_start_id)) {
    AddInterestedShard(storage_shard_id);
    set_metalog_position(metalog_position);
    log_header_ = fmt::format("LogProducer[{}-{}]: ", view->id(), sequencer_id);
    state_ = kNormal;
}

LogProducer::~LogProducer() {}

void LogProducer::LocalAppend(void* caller_data, uint64_t* localid, uint64_t* next_seqnum) {
    DCHECK(!pending_appends_.contains(next_localid_));
    HVLOG_F(1, "LocalAppend with localid {}", bits::HexStr0x(next_localid_));
    pending_appends_[next_localid_] = caller_data;
    *localid = next_localid_++;
    *next_seqnum = seqnum_position();
}

void LogProducer::PollAppendResults(AppendResultVec* results) {
    *results = std::move(pending_append_results_);
    pending_append_results_.clear();
}

void LogProducer::OnNewLogs(uint32_t metalog_seqnum,
                            uint64_t start_seqnum, uint64_t start_localid,
                            uint32_t delta, uint16_t storage_shard_id) {
    for (size_t i = 0; i < delta; i++) {
        uint64_t seqnum = start_seqnum + i;
        uint64_t localid = start_localid + i;
        if (!pending_appends_.contains(localid)) {
            HLOG_F(FATAL, "Cannot find pending log entry for localid {}",
                   bits::HexStr0x(localid));
        }
        pending_append_results_.push_back(AppendResult {
            .seqnum = seqnum,
            .localid = localid,
            .metalog_progress = bits::JoinTwo32(identifier(), metalog_seqnum + 1),
            .caller_data = pending_appends_[localid]
        });
        pending_appends_.erase(localid);
    }
}

void LogProducer::OnFinalized(uint32_t metalog_position) {
    for (const auto& [localid, caller_data] : pending_appends_) {
        pending_append_results_.push_back(AppendResult {
            .seqnum = kInvalidLogSeqNum,
            .localid = localid,
            .metalog_progress = 0,
            .caller_data = caller_data
        });
    }
    pending_appends_.clear();
}

LogStorage::LogStorage(uint16_t storage_id, const View* view, uint16_t sequencer_id)
    : LogSpaceBase(LogSpaceBase::kLogStorage, view, sequencer_id),
      storage_node_(view_->GetStorageNode(storage_id)),
      shard_progress_dirty_(false),
      persisted_seqnum_position_(0) {
    for (uint32_t global_storage_shard_id : storage_node_->GetStorageShardIds()){
        // we only consider the local shard ids
        shard_progresses_[bits::LowHalf32(global_storage_shard_id)] = 0;
        AddInterestedShard(bits::LowHalf32(global_storage_shard_id));
    }
    index_data_packages_.set_logspace_id(identifier());
    log_header_ = fmt::format("LogStorage[{}-{}]: ", view->id(), sequencer_id);
    state_ = kNormal;
}

LogStorage::~LogStorage() {}

bool LogStorage::Store(const LogMetaData& log_metadata, std::span<const uint64_t> user_tags,
                       std::span<const char> log_data) {
    uint64_t localid = log_metadata.localid;
    DCHECK_EQ(size_t{log_metadata.data_size}, log_data.size());
    uint16_t storage_shard_id = gsl::narrow_cast<uint16_t>(bits::HighHalf64(localid));
    HVLOG_F(1, "Store log from storage_shard {} with localid {}",
            storage_shard_id, bits::HexStr0x(localid));
    //TODO: add ismember of storage node check
    pending_log_entries_[localid].reset(new LogEntry {
        .metadata = log_metadata,
        .user_tags = UserTagVec(user_tags.begin(), user_tags.end()),
        .data = std::string(log_data.data(), log_data.size()),
    });
    AdvanceShardProgress(storage_shard_id);
    return true;
}

void LogStorage::ReadAt(const protocol::SharedLogMessage& request) {
    DCHECK_EQ(request.logspace_id, identifier());
    uint64_t seqnum = bits::JoinTwo32(request.logspace_id, request.seqnum_lowhalf);
    if (seqnum >= seqnum_position()) {
        pending_read_requests_.insert(std::make_pair(seqnum, request));
        return;
    }
    ReadResult result = {
        .status = ReadResult::kFailed,
        .log_entry = nullptr,
        .original_request = request
    };
    if (live_log_entries_.contains(seqnum)) {
        result.status = ReadResult::kOK;
        result.log_entry = live_log_entries_[seqnum];
    } else if (seqnum < persisted_seqnum_position_) {
        result.status = ReadResult::kLookupDB;
    } else {
        HLOG_F(WARNING, "ReadRecord: Failed to locate seqnum {}", bits::HexStr0x(seqnum));
    }
    pending_read_results_.push_back(std::move(result));
}

bool LogStorage::GrabLogEntriesForPersistence(
        std::vector<std::shared_ptr<const LogEntry>>* log_entries,
        uint64_t* new_position) const {
    if (live_seqnums_.empty() || live_seqnums_.back() < persisted_seqnum_position_) {
        return false;
    }
    auto iter = absl::c_lower_bound(live_seqnums_, persisted_seqnum_position_);
    DCHECK(iter != live_seqnums_.end());
    DCHECK_GE(*iter, persisted_seqnum_position_);
    log_entries->clear();
    while (iter != live_seqnums_.end()) {
        uint64_t seqnum = *(iter++);
        DCHECK(live_log_entries_.contains(seqnum));
        log_entries->push_back(live_log_entries_.at(seqnum));
    }
    DCHECK(!log_entries->empty());
    *new_position = live_seqnums_.back() + 1;
    return true;
}

void LogStorage::LogEntriesPersisted(uint64_t new_position) {
    persisted_seqnum_position_ = new_position;
    ShrinkLiveEntriesIfNeeded();
}

void LogStorage::PollReadResults(ReadResultVec* results) {
    *results = std::move(pending_read_results_);
    pending_read_results_.clear();
}

std::optional<IndexDataPackagesProto> LogStorage::PollIndexData() {
    if (0 == index_data_packages_.index_data_proto_size()) {
        return std::nullopt;
    }
    IndexDataPackagesProto data;
    data.Swap(&index_data_packages_);
    index_data_packages_.Clear();
    index_data_packages_.set_logspace_id(identifier());
    return data;
}

std::optional<std::vector<uint32_t>> LogStorage::GrabShardProgressForSending() {
    if (!shard_progress_dirty_) {
        return std::nullopt;
    }
    std::vector<uint32_t> progress;
    progress.reserve(storage_node_->GetStorageShardIds().size());
    for (uint32_t global_storage_shard_id : storage_node_->GetStorageShardIds()) {
        uint16_t storage_shard_id = bits::LowHalf32(global_storage_shard_id);
        progress.push_back(shard_progresses_[storage_shard_id]);
    }
    shard_progress_dirty_ = false;
    return progress;
}

// delta and start_localid in the context of an active storage shard
// start_seqnum is increased for each engine
void LogStorage::OnNewLogs(uint32_t metalog_seqnum,
                           uint64_t start_seqnum, uint64_t start_localid,
                           uint32_t delta, uint16_t storage_shard_id) {
    auto iter = pending_read_requests_.begin();
    while (iter != pending_read_requests_.end() && iter->first < start_seqnum) {
        HLOG_F(WARNING, "Read request for seqnum {} has past", bits::HexStr0x(iter->first));
        pending_read_results_.push_back(ReadResult {
            .status = ReadResult::kFailed,
            .log_entry = nullptr,
            .original_request = iter->second
        });
        iter = pending_read_requests_.erase(iter);
    }
    for (size_t i = 0; i < delta; i++) {
        uint64_t seqnum = start_seqnum + i;
        uint64_t localid = start_localid + i;
        if (!pending_log_entries_.contains(localid)) {
            HLOG_F(FATAL, "MetalogUpdate: Cannot find pending log entry for localid {}",
                   bits::HexStr0x(localid));
        }
        // Build the log entry for live_log_entries_
        LogEntry* log_entry = pending_log_entries_[localid].release();
        pending_log_entries_.erase(localid);
        HVLOG_F(1, "MetalogUpdate: Finalize the log entry (seqnum={}, localid={})",
                bits::HexStr0x(seqnum), bits::HexStr0x(localid));
        log_entry->metadata.seqnum = seqnum;
        std::shared_ptr<const LogEntry> log_entry_ptr(log_entry);
        // Add the new entry to index data
        index_data_.add_seqnum_halves(bits::LowHalf64(seqnum));
        index_data_.add_engine_ids(bits::HighHalf64(localid));
        index_data_.add_user_logspaces(log_entry->metadata.user_logspace);
        index_data_.add_user_tag_sizes(
            gsl::narrow_cast<uint32_t>(log_entry->user_tags.size()));
        index_data_.mutable_user_tags()->Add(
            log_entry->user_tags.begin(), log_entry->user_tags.end());
        // Update live_seqnums_ and live_log_entries_
        DCHECK(live_seqnums_.empty() || seqnum > live_seqnums_.back());
        live_seqnums_.push_back(seqnum);
        live_log_entries_[seqnum] = log_entry_ptr;
        DCHECK_EQ(live_seqnums_.size(), live_log_entries_.size());
        ShrinkLiveEntriesIfNeeded();
        // Check if we have read request on it
        while (iter != pending_read_requests_.end() && iter->first == seqnum) {
            pending_read_results_.push_back(ReadResult {
                .status = ReadResult::kOK,
                .log_entry = log_entry_ptr,
                .original_request = iter->second
            });
            iter = pending_read_requests_.erase(iter);
        }
    }
    index_data_.add_my_productive_storage_shards(storage_shard_id);
}

void LogStorage::OnMetaLogApplied(const MetaLogProto& meta_log_proto){
    switch (meta_log_proto.type()) {
    case MetaLogProto::NEW_LOGS:
        {
            if (0 < index_data_.seqnum_halves_size()) {
                index_data_.set_metalog_position(metalog_position());
                index_data_.set_end_seqnum_position(local_seqnum_position());
                index_data_.set_num_productive_storage_shards(gsl::narrow_cast<uint32_t>(meta_log_proto.new_logs_proto().shard_ids_size()));
                IndexDataProto* data = index_data_packages_.add_index_data_proto();
                data->Swap(&index_data_);
                index_data_.Clear();
            }
            break;
        }
    default:
        break;
    }
}

void LogStorage::OnFinalized(uint32_t metalog_position) {
    if (!pending_log_entries_.empty()) {
        HLOG_F(WARNING, "{} pending log entries discarded", pending_log_entries_.size());
        pending_log_entries_.clear();
    }
    if (!pending_read_requests_.empty()) {
        HLOG_F(FATAL, "There are {} pending reads", pending_read_requests_.size());
    }
}

void LogStorage::AdvanceShardProgress(uint16_t storage_shard_id) {
    uint32_t current = shard_progresses_[storage_shard_id];
    while (pending_log_entries_.contains(bits::JoinTwo32(storage_shard_id, current))) {
        current++;
    }
    if (current > shard_progresses_[storage_shard_id]) {
        HVLOG_F(1, "Update shard progres for storage_shard {}: from={}, to={}",
                storage_shard_id,
                bits::HexStr0x(shard_progresses_[storage_shard_id]),
                bits::HexStr0x(current));
        shard_progress_dirty_ = true;
        shard_progresses_[storage_shard_id] = current;
    }
}

void LogStorage::ShrinkLiveEntriesIfNeeded() {
    size_t max_size = absl::GetFlag(FLAGS_slog_storage_max_live_entries);
    while (live_seqnums_.size() > max_size
             && live_seqnums_.front() < persisted_seqnum_position_) {
        live_log_entries_.erase(live_seqnums_.front());
        live_seqnums_.pop_front();
        DCHECK_EQ(live_seqnums_.size(), live_log_entries_.size());
    }
}

void LogStorage::RemovePendingEntries(uint16_t storage_shard_id) {
    auto iter = pending_log_entries_.begin();
    while (iter != pending_log_entries_.end()) {
        if (gsl::narrow_cast<uint16_t>(bits::HighHalf64(iter->first) == storage_shard_id)){
            HLOG_F(INFO, "Remove entry {}", bits::HexStr0x(iter->first));
            pending_log_entries_.erase(iter);
        }
    }
}

}  // namespace log
}  // namespace faas
