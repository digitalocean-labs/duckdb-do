#pragma once

#include "duckdb_extension.h"

#include <cstdint>
#include <string>

// Names of the SET options this extension registers.
constexpr const char *kModelAccessKeySetting = "do_model_access_key";
constexpr const char *kDecideModelSetting = "do_decide_model";
constexpr const char *kMaxConcurrentRequestsSetting = "do_max_concurrent_requests";

// Registers all SET options. Returns false if any registration fails.
bool RegisterConfigOptions(duckdb_connection connection);

// Current value of a setting for this query's session. Returns "" / `fallback` if unset.
std::string GetStringSetting(duckdb_client_context context, const char *name);
int64_t GetIntSetting(duckdb_client_context context, const char *name, int64_t fallback);
