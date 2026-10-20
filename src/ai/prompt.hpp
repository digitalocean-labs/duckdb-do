#pragma once

#include "duckdb_extension.h"

// Registers the do_prompt SQL function. Returns false if registration fails.
bool RegisterPromptFunction(duckdb_connection connection);
