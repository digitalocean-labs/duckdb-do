#include "duckdb_extension.h"

#include "ai/decide.hpp"
#include "ai/embed.hpp"
#include "ai/models.hpp"
#include "ai/prompt.hpp"
#include "common/config.hpp"
#include "common/http.hpp"

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_connection connection, duckdb_extension_info info, struct duckdb_extension_access *access) {
	InitUserAgent(duckdb_library_version());

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
	if (!RegisterPromptFunction(connection)) {
		access->set_error(info, "docn: failed to register do_prompt");
		return false;
	}
	if (!RegisterEmbedFunction(connection)) {
		access->set_error(info, "docn: failed to register do_embed");
		return false;
	}

	// Return true to indicate successful initialization
	return true;
}