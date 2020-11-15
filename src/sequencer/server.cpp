#include "sequencer/server.h"

#define HLOG(l) LOG(l) << "Server: "
#define HVLOG(l) VLOG(l) << "Server: "

namespace faas {
namespace sequencer {

using protocol::SequencerMessage;
using protocol::SequencerMessageHelper;

Server::Server()
    : state_(kCreated),
      engine_conn_port_(-1),
      event_loop_thread_("Server/EL", absl::bind_front(&Server::EventLoopThreadMain, this)),
      node_manager_(this) {
    UV_DCHECK_OK(uv_loop_init(&uv_loop_));
    uv_loop_.data = &event_loop_thread_;
    UV_DCHECK_OK(uv_async_init(&uv_loop_, &stop_event_, &Server::StopCallback));
    stop_event_.data = this;
    core_.SetSendFsmRecordsMessageCallback(
        absl::bind_front(&Server::SendFsmRecordsMessage, this));
}

Server::~Server() {
    State state = state_.load();
    DCHECK(state == kCreated || state == kStopped);
    UV_DCHECK_OK(uv_loop_close(&uv_loop_));
}

void Server::Start() {
    DCHECK(state_.load() == kCreated);
    DCHECK(engine_conn_port_ != -1);
    node_manager_.Start(&uv_loop_, address_, gsl::narrow_cast<uint16_t>(engine_conn_port_));
    // Start thread for running event loop
    event_loop_thread_.Start();
    state_.store(kRunning);
}

void Server::ScheduleStop() {
    HLOG(INFO) << "Scheduled to stop";
    UV_DCHECK_OK(uv_async_send(&stop_event_));
}

void Server::WaitForFinish() {
    DCHECK(state_.load() != kCreated);
    event_loop_thread_.Join();
    DCHECK(state_.load() == kStopped);
    HLOG(INFO) << "Stopped";
}

void Server::EventLoopThreadMain() {
    HLOG(INFO) << "Event loop starts";
    int ret = uv_run(&uv_loop_, UV_RUN_DEFAULT);
    if (ret != 0) {
        HLOG(WARNING) << "uv_run returns non-zero value: " << ret;
    }
    HLOG(INFO) << "Event loop finishes";
    state_.store(kStopped);
}

void Server::OnNewNodeConnected(uint16_t node_id, std::string_view shared_log_addr) {
    core_.OnNewNodeConnected(node_id, shared_log_addr);
}

void Server::OnNodeDisconnected(uint16_t node_id) {
    core_.OnNodeDisconnected(node_id);
}

void Server::OnRecvNodeMessage(uint16_t node_id, const SequencerMessage& message,
                               std::span<const char> payload) {
    if (SequencerMessageHelper::IsLocalCut(message)) {
        log::LocalCutMsgProto message_proto;
        if (!message_proto.ParseFromArray(payload.data(), payload.size())) {
            HLOG(ERROR) << "Failed to parse sequencer message!";
            return;
        }
        core_.NewLocalCutMessage(message_proto);
    } else {
        HLOG(ERROR) << fmt::format("Unknown message type: {}!", message.message_type);
    }
}

void Server::SendFsmRecordsMessage(uint16_t node_id, std::span<const char> data) {
    SequencerMessage message = SequencerMessageHelper::NewFsmRecords(data);
    if (!node_manager_.SendMessage(node_id, message, data)) {
        HLOG(ERROR) << fmt::format("Failed to send FsmRecordsMessage to node {}", node_id);
    }
}

UV_ASYNC_CB_FOR_CLASS(Server, Stop) {
    if (state_.load(std::memory_order_consume) == kStopping) {
        HLOG(WARNING) << "Already in stopping state";
        return;
    }
    HLOG(INFO) << "Start stopping process";
    node_manager_.ScheduleStop();
    uv_close(UV_AS_HANDLE(&stop_event_), nullptr);
    state_.store(kStopping);
}

}  // namespace sequencer
}  // namespace faas
