#pragma once

#ifndef __FAAS_SRC
#error common/flags.h cannot be included outside
#endif

#include "base/common.h"

__BEGIN_THIRD_PARTY_HEADERS
#include <absl/flags/flag.h>
#include <absl/flags/declare.h>
__END_THIRD_PARTY_HEADERS

ABSL_DECLARE_FLAG(std::string, listen_addr);
ABSL_DECLARE_FLAG(std::string, listen_iface);
ABSL_DECLARE_FLAG(int, num_io_workers);
ABSL_DECLARE_FLAG(int, message_conn_per_worker);
ABSL_DECLARE_FLAG(int, socket_listen_backlog);
ABSL_DECLARE_FLAG(bool, tcp_enable_reuseport);
ABSL_DECLARE_FLAG(bool, tcp_enable_nodelay);
ABSL_DECLARE_FLAG(bool, tcp_enable_keepalive);

ABSL_DECLARE_FLAG(std::string, zookeeper_host);
ABSL_DECLARE_FLAG(std::string, zookeeper_root_path);

ABSL_DECLARE_FLAG(int, scale_in_grace_period_s);
