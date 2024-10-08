#pragma once

#include "common/flags.h"

ABSL_DECLARE_FLAG(int, slog_local_cut_interval_us);
ABSL_DECLARE_FLAG(int, slog_global_cut_interval_us);
ABSL_DECLARE_FLAG(size_t, slog_log_space_hash_tokens);
ABSL_DECLARE_FLAG(size_t, slog_num_tail_metalog_entries);

ABSL_DECLARE_FLAG(bool, slog_enable_statecheck);
ABSL_DECLARE_FLAG(int, slog_statecheck_interval_sec);

ABSL_DECLARE_FLAG(bool, slog_engine_force_remote_index);
ABSL_DECLARE_FLAG(float, slog_engine_prob_remote_index);
ABSL_DECLARE_FLAG(bool, slog_engine_enable_cache);
ABSL_DECLARE_FLAG(int, slog_engine_cache_cap_mb);
ABSL_DECLARE_FLAG(bool, slog_engine_propagate_auxdata);

ABSL_DECLARE_FLAG(bool, slog_engine_index_tier_only);
ABSL_DECLARE_FLAG(bool, slog_engine_distributed_indexing);
ABSL_DECLARE_FLAG(int, slog_engine_seqnum_cache_cap);
ABSL_DECLARE_FLAG(int, slog_engine_seqnum_suffix_cap);
ABSL_DECLARE_FLAG(int, slog_engine_tag_cache_cap);
ABSL_DECLARE_FLAG(int, slog_engine_per_tag_seqnums_limit);
ABSL_DECLARE_FLAG(std::string, slog_engine_postpone_registration);
ABSL_DECLARE_FLAG(std::string, slog_engine_postpone_caching);

ABSL_DECLARE_FLAG(int, slog_storage_cache_cap_mb);
ABSL_DECLARE_FLAG(std::string, slog_storage_backend);
ABSL_DECLARE_FLAG(int, slog_storage_bgthread_interval_ms);
ABSL_DECLARE_FLAG(size_t, slog_storage_max_live_entries);

ABSL_DECLARE_FLAG(bool, slog_storage_index_tier_only);

ABSL_DECLARE_FLAG(bool, slog_activate_min_seqnum_completion);
