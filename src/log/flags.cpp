#include "log/flags.h"

ABSL_FLAG(int, slog_local_cut_interval_us, 1000, "");
ABSL_FLAG(int, slog_global_cut_interval_us, 1000, "");
ABSL_FLAG(size_t, slog_log_space_hash_tokens, 128, "");
ABSL_FLAG(size_t, slog_num_tail_metalog_entries, 32, "");

ABSL_FLAG(bool, slog_enable_statecheck, false, "");
ABSL_FLAG(int, slog_statecheck_interval_sec, 10, "");

ABSL_FLAG(bool, slog_engine_force_remote_index, false, "");
ABSL_FLAG(float, slog_engine_prob_remote_index, 0.0f, "");
ABSL_FLAG(bool, slog_engine_enable_cache, false, "");
ABSL_FLAG(int, slog_engine_cache_cap_mb, 1024, "");
ABSL_FLAG(bool, slog_engine_propagate_auxdata, false, "");

ABSL_FLAG(bool, slog_engine_index_tier_only, false, "");
ABSL_FLAG(bool, slog_engine_distributed_indexing, false, "");
ABSL_FLAG(int, slog_engine_seqnum_cache_cap, 1000, "");
ABSL_FLAG(int, slog_engine_seqnum_suffix_cap, 100000, "");
ABSL_FLAG(int, slog_engine_tag_cache_cap, 1000000, "");
ABSL_FLAG(int, slog_engine_per_tag_seqnums_limit, 10000, "");

ABSL_FLAG(std::string, slog_engine_postpone_registration, "", "");
ABSL_FLAG(std::string, slog_engine_postpone_caching, "", "");

ABSL_FLAG(int, slog_storage_cache_cap_mb, 1024, "");
ABSL_FLAG(std::string, slog_storage_backend, "rocksdb",
          "rocskdb, tkrzw_hash, tkrzw_tree, or tkrzw_skip");
ABSL_FLAG(int, slog_storage_bgthread_interval_ms, 1, "");
ABSL_FLAG(size_t, slog_storage_max_live_entries, 65536, "");

ABSL_FLAG(bool, slog_storage_index_tier_only, false, "");

ABSL_FLAG(bool, slog_activate_min_seqnum_completion, false, "");
