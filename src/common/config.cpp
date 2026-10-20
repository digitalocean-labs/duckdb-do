#include "common/config.hpp"

#include "common/duckdb_handles.hpp"

#include <cstdlib>

DUCKDB_EXTENSION_EXTERN

namespace {

// Registers one database-wide option. DuckDB copies the type and default value,
// so we destroy our handles right after registering.
bool RegisterOption(duckdb_connection connection, const char *name, duckdb_type type, duckdb_value default_value,
                    const char *description) {
	duckdb_config_option option = duckdb_create_config_option();
	duckdb_logical_type logical_type = duckdb_create_logical_type(type);

	duckdb_config_option_set_name(option, name);
	duckdb_config_option_set_type(option, logical_type);
	duckdb_config_option_set_default_value(option, default_value);
	duckdb_config_option_set_description(option, description);
	duckdb_state state = duckdb_register_config_option(connection, option);

	duckdb_destroy_logical_type(&logical_type);
	duckdb_destroy_value(&default_value);
	duckdb_destroy_config_option(&option);
	return state == DuckDBSuccess;
}

} // namespace

bool RegisterConfigOptions(duckdb_connection connection) {
	// NULL by default: unset means "read DO_MODEL_ACCESS_KEY from the environment".
	return RegisterOption(connection, kModelAccessKeySetting, DUCKDB_TYPE_VARCHAR, duckdb_create_null_value(),
	                      "DigitalOcean model access key. Leave unset to use the DO_MODEL_ACCESS_KEY environment variable") &&
	       RegisterOption(connection, kDecideModelSetting, DUCKDB_TYPE_VARCHAR,
	                      duckdb_create_varchar("typesafe-jev-1.13.0"), "Model used by do_decide") &&
	       RegisterOption(connection, kMaxConcurrentRequestsSetting, DUCKDB_TYPE_BIGINT, duckdb_create_int64(32),
	                      "Maximum inference requests in flight at once, shared by all threads") &&
	       RegisterOption(connection, kPromptModelSetting, DUCKDB_TYPE_VARCHAR, duckdb_create_varchar("deepseek-4-flash"),
	                      "Model used by do_prompt") &&
	       RegisterOption(connection, kEmbedModelSetting, DUCKDB_TYPE_VARCHAR,
	                      duckdb_create_varchar("qwen3-embedding-0.6b"), "Model used by do_embed");
}

std::string GetModelAccessKey(duckdb_client_context context) {
	ValueHandle value(duckdb_client_context_get_config_option(context, kModelAccessKeySetting, nullptr));
	if (value && !duckdb_is_null_value(value.get())) {
		// Set explicitly, even to '' (which disables the environment fallback).
		return TakeDuckDBString(duckdb_get_varchar(value.get()));
	}
	const char *from_env = std::getenv(kModelAccessKeyEnv);
	return from_env ? from_env : "";
}

std::string GetStringSetting(duckdb_client_context context, const char *name) {
	ValueHandle value(duckdb_client_context_get_config_option(context, name, nullptr));
	if (!value || duckdb_is_null_value(value.get())) {
		return "";
	}
	return TakeDuckDBString(duckdb_get_varchar(value.get()));
}

int64_t GetIntSetting(duckdb_client_context context, const char *name, int64_t fallback) {
	ValueHandle value(duckdb_client_context_get_config_option(context, name, nullptr));
	if (!value || duckdb_is_null_value(value.get())) {
		return fallback;
	}
	return duckdb_get_int64(value.get());
}
