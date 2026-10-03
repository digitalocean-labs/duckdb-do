#include "common/config.hpp"

#include "common/duckdb_handles.hpp"

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
	return RegisterOption(connection, kModelAccessKeySetting, DUCKDB_TYPE_VARCHAR, duckdb_create_varchar(""),
	                      "DigitalOcean model access key for Serverless Inference") &&
	       RegisterOption(connection, kDecideModelSetting, DUCKDB_TYPE_VARCHAR,
	                      duckdb_create_varchar("typesafe-jev-1.13.0"), "Model used by do_decide") &&
	       RegisterOption(connection, kMaxConcurrentRequestsSetting, DUCKDB_TYPE_BIGINT, duckdb_create_int64(32),
	                      "Maximum inference requests in flight at once, shared by all threads");
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
