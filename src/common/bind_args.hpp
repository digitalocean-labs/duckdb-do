#pragma once

#include "duckdb_extension.h"

#include <cstdint>
#include <string>

// Evaluates argument `index` of a scalar function once, at bind time. The argument
// must be a constant, non-NULL text value (for example a model name).
// Returns false and sets `error` otherwise.
bool FoldConstantText(duckdb_bind_info info, duckdb_client_context context, idx_t index, std::string &out,
                      std::string &error);

// What do_prompt and do_embed need at execution time, read once at bind.
struct InferenceBind {
	std::string access_key;
	std::string model;
	int64_t max_concurrent = 32;
};

// Reads the model access key and concurrency limit, and the model: argument 1 if the
// function was called with one, otherwise the `model_setting` setting.
// Returns false and sets `error` if something is missing.
bool BindInference(duckdb_bind_info info, const char *model_setting, InferenceBind &out, std::string &error);
