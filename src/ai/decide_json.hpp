#pragma once

#include <string>
#include <utility>
#include <vector>

// The JSON side of do_decide: building request bodies and shaping responses.
// Uses no DuckDB types; decide.cpp converts SQL arguments into these structs.

struct DecideQuestion {
	// "noul", "choice" or "score"
	std::string type;
	std::string instructions;
	// choice only: (label, description) pairs, in the order the user wrote them
	std::vector<std::pair<std::string, std::string>> choices;
	// score only: levels ordered low to high
	std::vector<std::string> levels;
};

// A question together with its id (the key used in the request and the answer).
using NamedQuestion = std::pair<std::string, DecideQuestion>;

// Returns an error message, or an empty string if the question is valid.
std::string ValidateQuestion(const NamedQuestion &question);

// Request body without the row text, ending in `"state":`. Built once per query;
// BuildRequestBody() appends each row's text. Returns an empty string on failure.
std::string BuildRequestPrefix(const std::string &model, const std::vector<NamedQuestion> &questions);

// prefix + row text as a JSON string + "}". Returns an empty string on failure.
std::string BuildRequestBody(const std::string &prefix, const std::string &text);

// Turns a successful API response into do_decide's output JSON: the API answers
// unchanged, plus a "label" on score answers. If `single_question_id` is not empty,
// returns only that question's answer instead of the map of all answers.
std::string ShapeAnswers(const std::string &body, const std::string &single_question_id);

// Output JSON for a row whose request failed: {"error":{"status":..,"message":..}}
std::string ErrorJson(long status, const std::string &message);