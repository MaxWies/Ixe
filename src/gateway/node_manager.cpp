#include "gateway/node_manager.h"

#include "gateway/flags.h"
#include "gateway/server.h"

#define log_header_ "NodeManager: "

namespace faas {
namespace gateway {

using node::NodeType;

using server::NodeWatcher;
using server::ScaleWatcher;

NodeManager::NodeManager(Server* server)
    : server_(server),
      max_running_requests_(0) {}

NodeManager::~NodeManager() {}

bool NodeManager::PickNodeForNewFuncCall(const protocol::FuncCall& func_call, uint16_t* node_id) {
    absl::MutexLock lk(&mu_);
    if (connected_node_list_.empty()) {
        return false;
    }
    if (max_running_requests_ > 0 && running_requests_.size() > max_running_requests_) {
        return false;
    }
    size_t idx;
    if (absl::GetFlag(FLAGS_lb_per_fn_round_robin)) {
        idx = (next_dispatch_node_idx_[func_call.func_id]++) % connected_node_list_.size();
    } else if (absl::GetFlag(FLAGS_lb_pick_least_load)) {
        auto iter = absl::c_min_element(
            connected_node_list_,
            [] (const Node* lhs, const Node* rhs) {
                return lhs->inflight_requests < rhs->inflight_requests;
            }
        );
        idx = static_cast<size_t>(iter - connected_node_list_.begin());
    } else {
        idx = absl::Uniform<size_t>(random_bit_gen_, 0, connected_node_list_.size());
    }
    Node* node = connected_node_list_[idx];
    node->inflight_requests++;
    node->dispatched_requests_stat.Tick();
    running_requests_.insert(func_call.full_call_id);
    *node_id = node->node_id;
    return true;
}

void NodeManager::FuncCallFinished(const protocol::FuncCall& func_call, uint16_t node_id) {
    absl::MutexLock lk(&mu_);
    if (!running_requests_.contains(func_call.full_call_id)) {
        HLOG(WARNING) << "There is no request for this function call anymore";
        return;
    }
    running_requests_.erase(func_call.full_call_id);
    if (!connected_nodes_.contains(node_id)) {
        HLOG(WARNING) << "The node does no exist anymore";
        return;
    }
    Node* node = connected_nodes_[node_id].get();
    node->inflight_requests--;
}

void NodeManager::OnNodeOnline(NodeType node_type, uint16_t node_id) {
    if (node_type != NodeType::kEngineNode) {
        return;
    }
    std::unique_ptr<Node> node = std::make_unique<Node>(node_id);
    {
        absl::MutexLock lk(&mu_);
        DCHECK(!connected_nodes_.contains(node_id))
            << fmt::format("Engine node {} already exists", node_id);
        connected_node_list_.push_back(node.get());
        connected_nodes_[node_id] = std::move(node);
        max_running_requests_ = absl::GetFlag(FLAGS_max_running_requests)
                              * connected_nodes_.size();
        HLOG_F(INFO, "{} nodes connected", connected_nodes_.size());
    }
    server_->OnEngineNodeOnline(node_id);
}

void NodeManager::OnNodeOffline(NodeType node_type, uint16_t node_id) {
    if (node_type != NodeType::kEngineNode) {
        return;
    }
    {
        absl::MutexLock lk(&mu_);
        if (!connected_nodes_.contains(node_id)){
            HLOG_F(INFO, "Engine node {} already removed", node_id);
        }
        connected_nodes_.erase(node_id);
        connected_node_list_.clear();
        for (auto& entry : connected_nodes_) {
            connected_node_list_.push_back(entry.second.get());
        }
        max_running_requests_ = absl::GetFlag(FLAGS_max_running_requests)
                              * connected_nodes_.size();
        HLOG_F(INFO, "{} nodes connected", connected_nodes_.size());
    }
    server_->OnEngineNodeOffline(node_id);
}

void NodeManager::OnNodeScaled(ScaleWatcher::ScaleOp scale_op, NodeType node_type, uint16_t node_id){
    if (node_type != NodeType::kEngineNode){
        return;
    }
    if (scale_op == ScaleWatcher::ScaleOp::kScaleOut){
        
    } else if (scale_op == ScaleWatcher::ScaleOp::kScaleIn){
        absl::MutexLock lk(&mu_);
        connected_nodes_.erase(node_id);
        HLOG_F(INFO, "Node {} will not get new function requests", node_id);
    } else {
        UNREACHABLE();
    }
}

NodeManager::Node::Node(uint16_t node_id)
    : node_id(node_id),
      inflight_requests(0),
      dispatched_requests_stat(stat::Counter::StandardReportCallback(
          fmt::format("dispatched_requests[{}]", node_id))) {}

}  // namespace gateway
}  // namespace faas
