#pragma once

#include "duckdb_extension.h"

// Registers the do_decide SQL function. Returns false if registration fails.
bool RegisterDecideFunction(duckdb_connection connection);