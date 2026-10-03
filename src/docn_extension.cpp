#include "duckdb_extension.h"

#include "ai/decide.hpp"
#include "ai/models.hpp"
#include "common/config.hpp"

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_connection connection, duckdb_extension_info info, struct duckdb_extension_access *access) {
	if (!RegisterConfigOptions(connection)) {
		access->set_error(info, "docn: failed to register settings (already registered by another extension?)");
		return false;
	}
	if (!RegisterDecideFunction(connection)) {
		access->set_error(info, "docn: failed to register do_decide");
		return false;
	}
	if (!RegisterModelsFunction(connection)) {
		access->set_error(info, "docn: failed to register do_models");
		return false;
	}

	// Return true to indicate successful initialization
	return true;
}