#pragma once

#include "duckdb_extension.h"

// Registers the do_models table function. Returns false if registration fails.
bool RegisterModelsFunction(duckdb_connection connection);