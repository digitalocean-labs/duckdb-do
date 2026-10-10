#pragma once

#include "duckdb_extension.h"

#include <cstdint>
#include <string>

// Names of the SET options this extension registers.
constexpr const char *kModelAccessKeySetting = "do_model_access_key";
constexpr const char *kDecideModelSetting = "do_decide_model";
constexpr const char *kMaxConcurrentRequestsSetting = "do_max_concurrent_requests";
constexpr const char *kPromptModelSetting = "do_prompt_model";
constexpr const char *kEmbedModelSetting = "do_embed_model";

// Registers all SET options. Returns false if any registration fails.
bool RegisterConfigOptions(duckdb_connection connection);

// Environment variable read when do_model_access_key is not set (the recommended way).
constexpr const char *kModelAccessKeyEnv = "DO_MODEL_ACCESS_KEY";

// The model access key for this session: the do_model_access_key setting if set
// (even to ''), otherwise the DO_MODEL_ACCESS_KEY environment variable, otherwise "".
std::string GetModelAccessKey(duckdb_client_context context);

// Current value of a setting for this query's session. Returns "" / `fallback` if unset.
std::string GetStringSetting(duckdb_client_context context, const char *name);
int64_t GetIntSetting(duckdb_client_context context, const char *name, int64_t fallback);
