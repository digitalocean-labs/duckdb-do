#include "ai/decide_json.hpp"

#include <yyjson.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <set>

namespace {

// Returns the first label that appears twice, or std::nullopt if all are unique.
std::optional<std::string> FindDuplicate(const std::vector<std::string> &labels) {
	std::set<std::string> seen;
	for (const std::string &label : labels) {
		if (!seen.insert(label).second) {
			return label;
		}
	}
	return std::nullopt;
}

// RAII owners for yyjson documents and strings written by yyjson.
using DocPtr = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;
using MutDocPtr = std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)>;
using CharPtr = std::unique_ptr<char, decltype(&free)>;

MutDocPtr NewMutDoc() {
	return MutDocPtr(yyjson_mut_doc_new(nullptr), &yyjson_mut_doc_free);
}

// Writes a JSON value to a string. Returns an empty string on failure.
std::string Write(yyjson_mut_doc *doc, yyjson_mut_val *root) {
	yyjson_mut_doc_set_root(doc, root);
	size_t length = 0;
	CharPtr json(yyjson_mut_write(doc, 0, &length), &free);
	return json ? std::string(json.get(), length) : std::string();
}

// For a score answer, adds "label": the level with the highest probability.
// The live API sends legend {"0":"low",...} and probabilities {"0":0.1,...}.
void AddScoreLabel(yyjson_mut_doc *doc, yyjson_mut_val *answer) {
	const char *type = yyjson_mut_get_str(yyjson_mut_obj_get(answer, "type"));
	if (!type || std::strcmp(type, "score") != 0) {
		return;
	}
	yyjson_mut_val *probabilities = yyjson_mut_obj_get(answer, "probabilities");
	yyjson_mut_val *legend = yyjson_mut_obj_get(answer, "legend");

	const char *best_position = nullptr;
	double best_probability = -1;
	size_t index, count;
	yyjson_mut_val *key, *value;
	yyjson_mut_obj_foreach(probabilities, index, count, key, value) {
		if (yyjson_mut_is_num(value) && yyjson_mut_get_num(value) > best_probability) {
			best_probability = yyjson_mut_get_num(value);
			best_position = yyjson_mut_get_str(key);
		}
	}
	if (!best_position) {
		return;
	}
	const char *label = yyjson_mut_get_str(yyjson_mut_obj_get(legend, best_position));
	if (label) {
		yyjson_mut_obj_add_strcpy(doc, answer, "label", label);
	}
}

} // namespace

std::string ValidateQuestion(const NamedQuestion &question) {
	const std::string &id = question.first;
	const DecideQuestion &q = question.second;
	if (q.instructions.empty()) {
		return "question '" + id + "' needs instructions";
	}
	if (q.type == "noul") {
		return "";
	}
	if (q.type == "choice") {
		if (q.choices.size() < 2 || q.choices.size() > 255) {
			return "question '" + id + "' (choice) needs 2 to 255 labels, got " + std::to_string(q.choices.size());
		}
		std::vector<std::string> labels;
		for (const auto &choice : q.choices) {
			labels.push_back(choice.first);
		}
		std::optional<std::string> duplicate = FindDuplicate(labels);
		if (duplicate) {
			return "question '" + id + "' (choice) has the label '" + *duplicate + "' twice";
		}
		return "";
	}
	if (q.type == "score") {
		if (q.levels.size() < 2 || q.levels.size() > 10) {
			return "question '" + id + "' (score) needs 2 to 10 levels, got " + std::to_string(q.levels.size());
		}
		std::optional<std::string> duplicate = FindDuplicate(q.levels);
		if (duplicate) {
			return "question '" + id + "' (score) has the level '" + *duplicate + "' twice";
		}
		return "";
	}
	return "question '" + id + "' has type '" + q.type + "', expected 'noul', 'choice' or 'score'";
}

std::string BuildRequestPrefix(const std::string &model, const std::vector<NamedQuestion> &questions) {
	MutDocPtr doc = NewMutDoc();
	yyjson_mut_doc *d = doc.get();
	yyjson_mut_val *root = yyjson_mut_obj(d);
	yyjson_mut_obj_add_strncpy(d, root, "model", model.data(), model.size());

	yyjson_mut_val *questions_json = yyjson_mut_obj(d);
	for (const NamedQuestion &named : questions) {
		const DecideQuestion &q = named.second;
		yyjson_mut_val *question = yyjson_mut_obj(d);
		yyjson_mut_obj_add_strncpy(d, question, "type", q.type.data(), q.type.size());
		yyjson_mut_obj_add_strncpy(d, question, "instructions", q.instructions.data(), q.instructions.size());
		if (q.type == "choice") {
			// API shape: {"label": "description", ...}
			yyjson_mut_val *criteria = yyjson_mut_obj(d);
			for (const auto &choice : q.choices) {
				yyjson_mut_obj_add(criteria, yyjson_mut_strncpy(d, choice.first.data(), choice.first.size()),
				                   yyjson_mut_strncpy(d, choice.second.data(), choice.second.size()));
			}
			yyjson_mut_obj_add_val(d, question, "criteria", criteria);
		} else if (q.type == "score") {
			// API shape: ["low", "medium", "high"]
			yyjson_mut_val *criteria = yyjson_mut_arr(d);
			for (const std::string &level : q.levels) {
				yyjson_mut_arr_add_strncpy(d, criteria, level.data(), level.size());
			}
			yyjson_mut_obj_add_val(d, question, "criteria", criteria);
		}
		yyjson_mut_obj_add(questions_json, yyjson_mut_strncpy(d, named.first.data(), named.first.size()), question);
	}
	yyjson_mut_obj_add_val(d, root, "questions", questions_json);

	// {"model":..,"questions":{..}}  ->  {"model":..,"questions":{..},"state":
	std::string json = Write(d, root);
	if (json.empty()) {
		return "";
	}
	json.pop_back();
	return json + ",\"state\":";
}

std::string BuildRequestBody(const std::string &prefix, const std::string &text) {
	// Let yyjson quote and escape the text (quotes, newlines, unicode).
	MutDocPtr doc = NewMutDoc();
	std::string state = Write(doc.get(), yyjson_mut_strncpy(doc.get(), text.data(), text.size()));
	if (state.empty()) {
		return "";
	}
	return prefix + state + "}";
}

std::string ShapeAnswers(const std::string &body, const std::string &single_question_id) {
	DocPtr response(yyjson_read(body.data(), body.size(), 0), &yyjson_doc_free);
	if (!response) {
		return ErrorJson(200, "response is not valid JSON");
	}
	yyjson_val *answers = yyjson_obj_get(yyjson_doc_get_root(response.get()), "answers");
	if (!yyjson_is_obj(answers)) {
		return ErrorJson(200, "response has no answers");
	}

	MutDocPtr out = NewMutDoc();
	yyjson_mut_val *answers_copy = yyjson_val_mut_copy(out.get(), answers);
	size_t index, count;
	yyjson_mut_val *key, *answer;
	yyjson_mut_obj_foreach(answers_copy, index, count, key, answer) {
		AddScoreLabel(out.get(), answer);
	}

	if (single_question_id.empty()) {
		return Write(out.get(), answers_copy);
	}
	yyjson_mut_val *single = yyjson_mut_obj_get(answers_copy, single_question_id.c_str());
	if (!single) {
		return ErrorJson(200, "response has no answer for '" + single_question_id + "'");
	}
	return Write(out.get(), single);
}

std::string ErrorJson(long status, const std::string &message) {
	MutDocPtr doc = NewMutDoc();
	yyjson_mut_doc *d = doc.get();
	yyjson_mut_val *error = yyjson_mut_obj(d);
	yyjson_mut_obj_add_int(d, error, "status", status);
	yyjson_mut_obj_add_strncpy(d, error, "message", message.data(), message.size());
	yyjson_mut_val *root = yyjson_mut_obj(d);
	yyjson_mut_obj_add_val(d, root, "error", error);
	return Write(d, root);
}