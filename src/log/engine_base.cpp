#include "log/engine_base.h"

#include "common/time.h"
#include "log/flags.h"
#include "log/utils.h"
#include "server/constants.h"
#include "engine/engine.h"
#include "utils/bits.h"

#define log_header_ "LogEngineBase: "

namespace faas {
namespace log {

using protocol::FuncCall;
using protocol::FuncCallHelper;
using protocol::Message;
using protocol::MessageHelper;
using protocol::SharedLogMessage;
using protocol::SharedLogMessageHelper;
using protocol::SharedLogOpType;
using protocol::SharedLogResultType;

using server::IOWorker;
using server::ConnectionBase;
using server::IngressConnection;
using server::EgressHub;
using server::NodeWatcher;

EngineBase::EngineBase(engine::Engine* engine)
    : node_id_(engine->node_id_),
      engine_(engine),
      next_local_op_id_(0),
      registered_(false) 
      {
          if (absl::GetFlag(FLAGS_slog_engine_postpone_registration) != ""){
              std::vector<int> v;
              std::stringstream ss(absl::GetFlag(FLAGS_slog_engine_postpone_registration));
              for (int i; ss >> i;) {
                  v.push_back(i);
                  if (ss.peek() == ',') {
                      ss.ignore();
                  }
              }
              for (int i : v) {
                  if (node_id_ % i == 0) {
                      postpone_registration_ = true;
                      HLOG_F(INFO, "I will postpone registration. my_node_id={}, arg={}", node_id_, i);
                  }
              }
          }
          if (absl::GetFlag(FLAGS_slog_engine_postpone_caching) != ""){
              std::vector<int> v;
              std::stringstream ss(absl::GetFlag(FLAGS_slog_engine_postpone_caching));
              for (int i; ss >> i;) {
                  v.push_back(i);
                  if (ss.peek() == ',') {
                      ss.ignore();
                  }
              }
              for (int i : v) {
                  if (node_id_ % i == 0) {
                      postpone_caching_ = true;
                      HLOG_F(INFO, "I will postpone caching. my_node_id={}, arg={}", node_id_, i);
                  }
              }
          }
      }

EngineBase::~EngineBase() {}

zk::ZKSession* EngineBase::zk_session() {
    return engine_->zk_session();
}

void EngineBase::Start() {
    SetupZKWatchers();
    SetupTimers();
    // Setup cache
    if (absl::GetFlag(FLAGS_slog_engine_enable_cache)) {
        log_cache_.emplace(absl::GetFlag(FLAGS_slog_engine_cache_cap_mb));
    }
}

void EngineBase::Stop() {}

void EngineBase::SetupZKWatchers() {
    view_watcher_.SetViewCreatedCallback(
        [this, base_engine = engine_] (const View* view) {
            auto my_engine = this;
            base_engine->CreateOnceTimer(
                kRegistrationTimerId,
                absl::Seconds(1),
                base_engine->SomeIOWorker(),
                [my_engine = my_engine, view = view] () {
                    my_engine->OnViewCreated(view);
                }
            );
        }
    );
    view_watcher_.SetViewFrozenCallback(
        [this] (const View* view) {
            this->OnViewFrozen(view);
        }
    );
    view_watcher_.SetViewFinalizedCallback(
        [this] (const FinalizedView* finalized_view) {
            this->OnViewFinalized(finalized_view);
        }
    );
    view_watcher_.StartWatching(zk_session());
    activation_watcher_.emplace(zk_session(), "activate");
    activation_watcher_->SetNodeCreatedCallback(
        absl::bind_front(&EngineBase::OnActivationZNodeCreated, this));
    activation_watcher_->Start();
#ifdef __FAAS_STAT_THREAD
    statistics_watcher_.emplace(zk_session(), "stat");
    statistics_watcher_->SetNodeCreatedCallback(
        absl::bind_front(&EngineBase::OnStatZNodeCreated, this));
    statistics_watcher_->Start();
#endif
}

void EngineBase::SetupTimers() {
}

void EngineBase::OnNewExternalFuncCall(const FuncCall& func_call, uint32_t log_space) {
    absl::MutexLock fn_ctx_lk(&fn_ctx_mu_);
    if (fn_call_ctx_.contains(func_call.full_call_id)) {
        HLOG(FATAL) << "FuncCall already exists: "
                    << FuncCallHelper::DebugString(func_call);
    }
    fn_call_ctx_[func_call.full_call_id] = FnCallContext {
        .user_logspace = log_space,
        .metalog_progress = 0,
        .parent_call_id = protocol::kInvalidFuncCallId
    };
}

void EngineBase::OnNewInternalFuncCall(const FuncCall& func_call,
                                       const FuncCall& parent_func_call) {
    absl::MutexLock fn_ctx_lk(&fn_ctx_mu_);
    if (fn_call_ctx_.contains(func_call.full_call_id)) {
        HLOG(FATAL) << "FuncCall already exists: "
                    << FuncCallHelper::DebugString(func_call);
    }
    if (!fn_call_ctx_.contains(parent_func_call.full_call_id)) {
        HLOG(FATAL) << "Cannot find parent FuncCall: "
                    << FuncCallHelper::DebugString(parent_func_call);
    }
    FnCallContext ctx = fn_call_ctx_.at(parent_func_call.full_call_id);
    ctx.parent_call_id = parent_func_call.full_call_id;
    fn_call_ctx_[func_call.full_call_id] = std::move(ctx);
}

void EngineBase::OnFuncCallCompleted(const FuncCall& func_call) {
    absl::MutexLock fn_ctx_lk(&fn_ctx_mu_);
    if (!fn_call_ctx_.contains(func_call.full_call_id)) {
        HLOG(FATAL) << "Cannot find FuncCall: "
                    << FuncCallHelper::DebugString(func_call);
    }
    fn_call_ctx_.erase(func_call.full_call_id);
}

void EngineBase::LocalOpHandler(LocalOp* op) {
    switch (op->type) {
    case SharedLogOpType::APPEND:
        HandleLocalAppend(op);
        break;
    case SharedLogOpType::READ_NEXT:
    case SharedLogOpType::READ_PREV:
    case SharedLogOpType::READ_NEXT_B:
        HandleLocalRead(op);
        break;
    case SharedLogOpType::TRIM:
        HandleLocalTrim(op);
        break;
    case SharedLogOpType::SET_AUXDATA:
        HandleLocalSetAuxData(op);
        break;
    default:
        UNREACHABLE();
    }
}

void EngineBase::MessageHandler(const SharedLogMessage& message,
                                std::span<const char> payload) {
    switch (SharedLogMessageHelper::GetOpType(message)) {
    case SharedLogOpType::INDEX_DATA:
        OnRecvNewIndexData(message, payload);
        break;
    case SharedLogOpType::METALOGS:
        OnRecvNewMetaLogs(message, payload);
        break;
    case SharedLogOpType::RESPONSE:
        OnRecvResponse(message, payload);
        break;
    case SharedLogOpType::REGISTER:
        OnRecvRegistrationResponse(message);
        break;
    default:
        UNREACHABLE();
    }
}

void EngineBase::PopulateLogTagsAndData(const Message& message, LocalOp* op) {
    DCHECK(op->type == SharedLogOpType::APPEND);
    DCHECK_EQ(message.log_aux_data_size, 0U);
    std::span<const char> data = MessageHelper::GetInlineData(message);
    size_t num_tags = message.log_num_tags;
    if (num_tags > 0) {
        op->user_tags.resize(num_tags);
        memcpy(op->user_tags.data(), data.data(), num_tags * sizeof(uint64_t));
    }
    op->data.AppendData(data.subspan(num_tags * sizeof(uint64_t)));
}

void EngineBase::OnMessageFromFuncWorker(const Message& message) {
#ifdef __FAAS_OP_TRACING
    int64_t func_ctx_ts = GetMonotonicMicroTimestamp();
#endif
    protocol::FuncCall func_call = MessageHelper::GetFuncCall(message);
    FnCallContext ctx;
    {
        absl::ReaderMutexLock fn_ctx_lk(&fn_ctx_mu_);
        if (!fn_call_ctx_.contains(func_call.full_call_id)) {
            HLOG(ERROR) << "Cannot find FuncCall: "
                        << FuncCallHelper::DebugString(func_call);
            return;
        }
        ctx = fn_call_ctx_.at(func_call.full_call_id);
        if (postpone_caching_ || !registered_) {
            // return ok if engine is postponed
            SharedLogResultType result; 
            switch(MessageHelper::GetSharedLogOpType(message)){
                case SharedLogOpType::APPEND:
                    result = SharedLogResultType::APPEND_OK;
                    break;
                case SharedLogOpType::READ_NEXT:
                case SharedLogOpType::READ_PREV:
                case SharedLogOpType::READ_NEXT_B:
                    result = SharedLogResultType::READ_OK;
                    break;
                case SharedLogOpType::TRIM:
                    result = SharedLogResultType::TRIM_OK;
                    break;
                case SharedLogOpType::SET_AUXDATA:
                    result = SharedLogResultType::AUXDATA_OK;
                    break;
                default:
                    UNREACHABLE(); 
            }
            Message response = MessageHelper::NewSharedLogOpSucceeded(
                result, kInvalidLogSeqNum
            );
            response.log_client_data = message.log_client_data;
            engine_->SendFuncWorkerMessage(message.log_client_id, &response);
            return;
        }
    }

    LocalOp* op = log_op_pool_.Get();
    op->id = next_local_op_id_.fetch_add(1, std::memory_order_acq_rel);
    op->start_timestamp = GetMonotonicMicroTimestamp();
    op->client_id = message.log_client_id;
    op->client_data = message.log_client_data;
    op->func_call_id = func_call.full_call_id;
    op->user_logspace = ctx.user_logspace;
    op->metalog_progress = ctx.metalog_progress;
    op->type = MessageHelper::GetSharedLogOpType(message);
    op->seqnum = kInvalidLogSeqNum;
    op->query_tag = kInvalidLogTag;
    op->index_lookup_miss = false;
    op->user_tags.clear();
    op->data.Reset();

    switch (op->type) {
    case SharedLogOpType::APPEND:
        PopulateLogTagsAndData(message, op);
        break;
    case SharedLogOpType::READ_NEXT:
    case SharedLogOpType::READ_PREV:
    case SharedLogOpType::READ_NEXT_B:
        op->query_tag = message.log_tag;
        op->seqnum = message.log_seqnum;
        break;
    case SharedLogOpType::TRIM:
        op->seqnum = message.log_seqnum;
        break;
    case SharedLogOpType::SET_AUXDATA:
        op->seqnum = message.log_seqnum;
        op->data.AppendData(MessageHelper::GetInlineData(message));
        break;
    default:
        HLOG(FATAL) << "Unknown shared log op type: " << message.log_op;
    }

#ifdef __FAAS_OP_TRACING
    InitTrace(op->id, op->type, func_ctx_ts, "InitByUsingMessageFromFuncWorker");
#endif
    LocalOpHandler(op);
}

void EngineBase::OnRecvSharedLogMessage(int conn_type, uint16_t src_node_id,
                                        const SharedLogMessage& message,
                                        std::span<const char> payload) {
    SharedLogOpType op_type = SharedLogMessageHelper::GetOpType(message);
    DCHECK(
        (conn_type == kSequencerIngressTypeId && op_type == SharedLogOpType::METALOGS)
     || (conn_type == kEngineIngressTypeId && op_type == SharedLogOpType::READ_NEXT)
     || (conn_type == kEngineIngressTypeId && op_type == SharedLogOpType::READ_PREV)
     || (conn_type == kEngineIngressTypeId && op_type == SharedLogOpType::READ_NEXT_B)
     || (conn_type == kStorageIngressTypeId && op_type == SharedLogOpType::INDEX_DATA)
     || op_type == SharedLogOpType::RESPONSE
    ) << fmt::format("Invalid combination: conn_type={:#x}, op_type={:#x}",
                     conn_type, message.op_type);
    MessageHandler(message, payload);
}

void EngineBase::ReplicateLogEntry(const View* view, const View::StorageShard* storage_shard, const LogMetaData& log_metadata,
                                   std::span<const uint64_t> user_tags,
                                   std::span<const char> log_data) {
    SharedLogMessage message = SharedLogMessageHelper::NewReplicateMessage();
    log_utils::PopulateMetaDataToMessage(log_metadata, &message);
    message.origin_node_id = node_id_;
    message.payload_size = gsl::narrow_cast<uint32_t>(
        user_tags.size() * sizeof(uint64_t) + log_data.size());
    for (uint16_t storage_id : storage_shard->GetStorageNodes()) {
        engine_->SendSharedLogMessage(protocol::ConnType::ENGINE_TO_STORAGE,
                                      storage_id, message,
                                      VECTOR_AS_CHAR_SPAN(user_tags), log_data);
    }
}

void EngineBase::PropagateAuxData(const View* view, const View::StorageShard* storage_shard, const LogMetaData& log_metadata, 
                                  std::span<const char> aux_data) {
    SharedLogMessage message = SharedLogMessageHelper::NewSetAuxDataMessage(
        log_metadata.seqnum);
    message.origin_node_id = node_id_;
    message.payload_size = gsl::narrow_cast<uint32_t>(aux_data.size());
    for (uint16_t storage_id : storage_shard->GetStorageNodes()) {
        engine_->SendSharedLogMessage(protocol::ConnType::ENGINE_TO_STORAGE,
                                      storage_id, message, aux_data);
    }
}

void EngineBase::FinishLocalOpWithResponse(LocalOp* op, Message* response,
                                           uint64_t metalog_progress, bool success) {
    if (metalog_progress > 0) {
        absl::MutexLock fn_ctx_lk(&fn_ctx_mu_);
        if (fn_call_ctx_.contains(op->func_call_id)) {
            FnCallContext& ctx = fn_call_ctx_[op->func_call_id];
            if (metalog_progress > ctx.metalog_progress) {
                ctx.metalog_progress = metalog_progress;
            }
        }
#ifdef __FAAS_OP_LATENCY
        finished_operations_.push_back(OpLatency{
            .type = op->type,
            .duration = GetMonotonicMicroTimestamp() - op->start_timestamp,
            .success = success
        });
#endif
    }
    response->log_client_data = op->client_data;
    engine_->SendFuncWorkerMessage(op->client_id, response);
#ifdef __FAAS_OP_TRACING
    CompleteTrace(op->id, "FinishedOpAndSentResponse");
#endif
    log_op_pool_.Return(op);
}

void EngineBase::FinishLocalOpWithFailure(LocalOp* op, SharedLogResultType result,
                                          uint64_t metalog_progress) {
    Message response = MessageHelper::NewSharedLogOpFailed(result);
    FinishLocalOpWithResponse(op, &response, metalog_progress, false);
}

void EngineBase::LogCachePut(const LogMetaData& log_metadata,
                             std::span<const uint64_t> user_tags,
                             std::span<const char> log_data) {
    if (!log_cache_.has_value()) {
        return;
    }
    HVLOG_F(1, "Store cache for log entry (seqnum {})", bits::HexStr0x(log_metadata.seqnum));
    log_cache_->Put(log_metadata, user_tags, log_data);
}

std::optional<LogEntry> EngineBase::LogCacheGet(uint64_t seqnum) {
    return log_cache_.has_value() ? log_cache_->Get(seqnum) : std::nullopt;
}

void EngineBase::LogCachePutAuxData(uint64_t seqnum, std::span<const char> data) {
    if (log_cache_.has_value()) {
        log_cache_->PutAuxData(seqnum, data);
    }
}

std::optional<std::string> EngineBase::LogCacheGetAuxData(uint64_t seqnum) {
    return log_cache_.has_value() ? log_cache_->GetAuxData(seqnum) : std::nullopt;
}

bool EngineBase::SendIndexTierReadRequest(uint16_t index_node_id, SharedLogMessage* request){
    static constexpr int kMaxRetries = 3;
    for (int i = 0; i < kMaxRetries; i++) {
        bool success = engine_->SendSharedLogMessage(
            protocol::ConnType::ENGINE_TO_INDEX, index_node_id, *request);
        if (success) {
            return true;
        }
    }
    return false;
}

bool EngineBase::SendStorageReadRequest(const IndexQueryResult& result,
                                        const View::StorageShard* storage_shard) {
    static constexpr int kMaxRetries = 3;
    DCHECK(result.state == IndexQueryResult::kFound);

    uint64_t seqnum = result.found_result.seqnum;
    SharedLogMessage request = SharedLogMessageHelper::NewReadAtMessage(
        bits::HighHalf64(seqnum), bits::LowHalf64(seqnum));
    request.user_metalog_progress = result.metalog_progress;
    request.storage_shard_id = storage_shard->local_shard_id();
    request.origin_node_id = result.original_query.origin_node_id;
    request.hop_times = result.original_query.hop_times + 1;
    request.client_data = result.original_query.client_data;
    for (int i = 0; i < kMaxRetries; i++) {
        uint16_t storage_id = storage_shard->PickStorageNode();
        bool success = engine_->SendSharedLogMessage(
            protocol::ConnType::ENGINE_TO_STORAGE, storage_id, request);
        if (success) {
            return true;
        }
    }
    return false;
}

void EngineBase::SendReadResponse(const IndexQuery& query,
                                  protocol::SharedLogMessage* response,
                                  std::span<const char> user_tags_payload,
                                  std::span<const char> data_payload,
                                  std::span<const char> aux_data_payload) {
    response->origin_node_id = node_id_;
    response->hop_times = query.hop_times + 1;
    response->client_data = query.client_data;
    response->payload_size = gsl::narrow_cast<uint32_t>(
        user_tags_payload.size() + data_payload.size() + aux_data_payload.size());
    uint16_t engine_id = query.origin_node_id;
    bool success = engine_->SendSharedLogMessage(
        protocol::ConnType::SLOG_ENGINE_TO_ENGINE,
        engine_id, *response, user_tags_payload, data_payload, aux_data_payload);
    if (!success) {
        HLOG_F(WARNING, "Failed to send read response to engine {}", engine_id);
    }
}

void EngineBase::SendReadFailureResponse(const IndexQuery& query,
                                         protocol::SharedLogResultType result_type,
                                         uint64_t metalog_progress) {
    SharedLogMessage response = SharedLogMessageHelper::NewResponse(result_type);
    response.user_metalog_progress = metalog_progress;
    SendReadResponse(query, &response);
}

bool EngineBase::SendSequencerMessage(uint16_t sequencer_id,
                                      SharedLogMessage* message,
                                      std::span<const char> payload) {
    message->origin_node_id = node_id_;
    message->payload_size = gsl::narrow_cast<uint32_t>(payload.size());
    return engine_->SendSharedLogMessage(
        protocol::ConnType::ENGINE_TO_SEQUENCER,
        sequencer_id, *message, payload);
}

bool EngineBase::SendRegistrationRequest(uint16_t destination_id, protocol::ConnType connection_type, SharedLogMessage* message){
    static constexpr int kMaxRetries = 3;
    DCHECK(message->op_type == gsl::narrow_cast<uint16_t>(protocol::SharedLogOpType::REGISTER));
    message->origin_node_id = node_id_;
    for (int i = 0; i < kMaxRetries; i++) {
        bool success = engine_->SendSharedLogMessage(connection_type, destination_id, *message);
        if (success) {
            return true;
        }
    }
    HLOG_F(ERROR, "Failed to send registration request to destination_id={}", destination_id);
    return false;
}

void EngineBase::OnActivationZNodeCreated(std::string_view path,
                                   std::span<const char> contents) {
    HLOG(INFO) << "Received activation command";
    if (path == "register") {
        {
            absl::MutexLock fn_ctx_lk(&fn_ctx_mu_);
            if (!postpone_registration_) {
                // already registered
                return;
            }
            postpone_registration_ = false;
        }
        if (this->missed_view_ == nullptr) {
            HLOG(WARNING) << "No view yet";
            return;
        }
        SomeIOWorker()->ScheduleFunction(
            nullptr, [this] {
                OnViewCreated(this->missed_view_);
            }
        );
    } else if (path == "cache") {
        {
            absl::ReaderMutexLock fn_ctx_lk(&fn_ctx_mu_);
            if (!postpone_caching_) {
                // already does caching
                return;
            }
        }
        OnActivateCaching();
        absl::MutexLock fn_ctx_lk(&fn_ctx_mu_);
        postpone_caching_ = false;
    } else {
        HLOG(ERROR) << "Unknown command: " << path;
    }
}

server::IOWorker* EngineBase::SomeIOWorker() {
    return engine_->SomeIOWorker();
}

#ifdef __FAAS_STAT_THREAD
void EngineBase::OnStatZNodeCreated(std::string_view path,
                                   std::span<const char> contents) {
    if (path == "start") {
        HLOG(INFO) << "Received statistics thread activation command";
        int parsed;
        if (!absl::SimpleAtoi(std::string_view(contents.data(), contents.size()), &parsed)) {
            LOG(ERROR) << "Failed to parse node_id: " << path;
            return;
        }
        OnActivateStatisticsThread(parsed);
    } else {
        HLOG(ERROR) << "Unknown command: " << path;
    }
}
#endif

#ifdef __FAAS_OP_LATENCY
void EngineBase::PrintOpLatencies(std::ostringstream* append_results, std::ostringstream* read_results){
    absl::MutexLock fn_ctx_lk(&fn_ctx_mu_);
    for (OpLatency op : finished_operations_){
        switch(op.type){
        case SharedLogOpType::APPEND:
            *append_results << std::to_string(op.duration) << (op.success? ",1\n" : ",0\n");
            break;
        case SharedLogOpType::READ_NEXT:
        case SharedLogOpType::READ_PREV:
        case SharedLogOpType::READ_NEXT_B:
            *read_results << std::to_string(op.duration) << (op.success? ",1\n" : ",0\n");
            break;
        default:
            break;
        }
    }
    finished_operations_.clear();
}
#endif

#ifdef __FAAS_OP_TRACING

bool EngineBase::IsOpTraced(uint64_t id){
    return 0 == id % trace_granularity_;
}

void EngineBase::InitTrace(uint64_t id, SharedLogOpType type, int64_t first_ts, const std::string func_desc){
    if(!IsOpTraced(id)){
        return;
    }
    HVLOG_F(1, "Init trace for {}", id);
    absl::MutexLock trace_mu_lk(&trace_mu_);
    OpTrace* trace = new OpTrace {
        .type = type
    };
    int64_t now_ts = GetMonotonicMicroTimestamp();
    trace->func_desc.push_back("Start");
    trace->func_desc.push_back(func_desc);
    trace->relative_ts.push_back(0);
    trace->relative_ts.push_back(now_ts-first_ts);
    trace->absolute_ts.push_back(first_ts);
    trace->absolute_ts.push_back(now_ts);
    traces_[id].reset(trace);
}

void EngineBase::SaveTracePoint(uint64_t function_call_id, const std::string func_desc){
    uint64_t id = function_call_id >> 4;
    if(!IsOpTraced(id)){
        return;
    }
    absl::MutexLock trace_mu_lk(&trace_mu_);
    if (traces_.contains(id)){
        OpTrace* trace = traces_.at(id).get();
        int64_t now_ts = GetMonotonicMicroTimestamp();
        trace->func_desc.push_back(func_desc);
        trace->relative_ts.push_back(now_ts-trace->absolute_ts.back());
        trace->absolute_ts.push_back(now_ts);
    } else {
        HLOG_F(WARNING, "Trace Point for {} not in traces", id);
    }
}

void EngineBase::SaveOrIncreaseTracePoint(uint64_t function_call_id, const std::string func_desc){
    uint64_t id = function_call_id >> 4;
    if(!IsOpTraced(id)){
        return;
    }
    absl::MutexLock trace_mu_lk(&trace_mu_);
    if (traces_.contains(id)){
        OpTrace* trace = traces_.at(id).get();
        int64_t now_ts = GetMonotonicMicroTimestamp();
        int64_t last_absolute_ts = trace->absolute_ts.back();
        if (!trace->func_desc.back().compare(func_desc)){
            int64_t relative_ts = trace->relative_ts.back();
            trace->relative_ts.pop_back();
            trace->relative_ts.push_back(relative_ts + now_ts - last_absolute_ts);
            trace->absolute_ts.pop_back();
            trace->absolute_ts.push_back(now_ts);
        } else {
            trace->func_desc.push_back(func_desc);
            trace->relative_ts.push_back(now_ts-last_absolute_ts);
            trace->absolute_ts.push_back(now_ts);
        }
    } else {
        HLOG_F(WARNING, "Trace Point for {} not in traces", id);
    }
}

void EngineBase::CompleteTrace(uint64_t function_call_id, const std::string func_desc){
    uint64_t id = function_call_id >> 4;
    if(!IsOpTraced(id)){
        return;
    }
    HVLOG_F(1, "Complete trace for {}", id);
    absl::MutexLock trace_mu_lk(&trace_mu_);
    if (traces_.contains(id)){
        OpTrace* trace = traces_.at(id).get();
        int64_t now_ts = GetMonotonicMicroTimestamp();
        trace->func_desc.push_back(func_desc);
        trace->relative_ts.push_back(now_ts-trace->absolute_ts.back());
        trace->absolute_ts.push_back(now_ts);
        finished_traces_.insert(id);
    } else {
        HLOG_F(WARNING, "Trace Point for {} not in traces", id);
    }
}

void EngineBase::PrintTrace(std::ostringstream* append_results, std::ostringstream* read_results, const OpTrace* op_trace){
    switch(op_trace->type){
    case SharedLogOpType::APPEND:
        for (std::string func_desc : op_trace->func_desc){
            *append_results << func_desc << ", ";
        }
        *append_results << "\n";
        for (int64_t ts : op_trace->relative_ts){
            *append_results << std::to_string(ts) << ", ";
        }
        *append_results << "\n";
        break;
    case SharedLogOpType::READ_NEXT:
    case SharedLogOpType::READ_PREV:
    case SharedLogOpType::READ_NEXT_B:
        for (std::string func_desc : op_trace->func_desc){
            *read_results << func_desc << ", ";
        }
        *read_results << "\n";
        for (int64_t ts : op_trace->relative_ts){
            *read_results << std::to_string(ts) << ", ";
        }
        *read_results << "\n";
        break;
    default:
        break;
    }
}

#endif

}  // namespace log
}  // namespace faas
