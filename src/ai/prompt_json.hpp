#pragma once

#include <optional>
#include <string>

// The JSON side of do_prompt. Uses no DuckDB types.

// Chat request with one user message. Returns an empty string on failure.
std::string BuildChatBody(const std::string &model, const std::string &prompt);

// The reply text from a chat response, or std::nullopt if there is none
// (invalid JSON, no choices, or an empty reply).
std::optional<std::string> ParseChatText(const std::string &body);
