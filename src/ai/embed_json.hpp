#pragma once

#include <string>
#include <vector>

// The JSON side of do_embed. Uses no DuckDB types.

// One request that embeds all `texts`. Returns an empty string on failure.
std::string BuildEmbedBody(const std::string &model, const std::vector<std::string> &texts);

// One vector per input, in input order. The API sends each vector with its input
// index, so order is restored from that. On failure returns an empty list and sets `error`.
std::vector<std::vector<float>> ParseEmbeddings(const std::string &body, size_t expected, std::string &error);
