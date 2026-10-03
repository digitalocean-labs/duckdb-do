#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The JSON side of do_models: parsing the model catalog. Uses no DuckDB types.

struct ModelInfo {
	// The model identifier used in requests and settings, e.g. "typesafe-jev-1.13.0"
	std::string name;
	// e.g. "chat", "reasoning", "embedding", "decision"
	std::string type;
	std::string description;
	std::string creator;
	// Not reported for every model
	std::optional<int64_t> context_length;
	std::optional<int64_t> max_output_tokens;
};

// Parses a GET /v2/gen-ai/models/catalog response. On failure returns an empty
// list and sets `error`.
std::vector<ModelInfo> ParseModels(const std::string &body, std::string &error);