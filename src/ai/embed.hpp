#pragma once

#include "duckdb_extension.h"

// Registers the do_embed SQL function. Returns false if registration fails.
bool RegisterEmbedFunction(duckdb_connection connection);
