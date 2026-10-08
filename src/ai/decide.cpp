#include "ai/decide.hpp"

#include "ai/decide_json.hpp"
#include "common/config.hpp"
#include "common/duckdb_handles.hpp"
#include "common/http.hpp"

#include <exception>
#include <memory>
#include <string>
#include <vector>

// do_decide, the DuckDB side: registration, bind (once per query) and
// execute (once per chunk of up to 2048 rows). JSON work is in decide_json.cpp.
//
//   do_decide(text, question)                          yes/no
//   do_decide(text, question, 'choice', labels)        pick one label
//   do_decide(text, question, 'score', levels)         place on an ordered scale
//   do_decide(text, {id: {type, instructions, criteria}, ...})   several questions

namespace {

constexpr const char *kDecideUrl = "https://inference.do-ai.run/v1/systemone";
// Question id used for the single-question forms.
constexpr const char *kSingleQuestionId = "decision";

// Everything execute needs, computed once at bind and shared read-only by all threads.
struct DecideBindData {
	std::string access_key;
	std::string request_prefix;
	// Empty for the multi-question form (return all answers).
	std::string single_question_id;
	int64_t max_concurrent = 32;
};

duckdb_type TypeId(duckdb_value value) {
	// The type is borrowed from the value: do not destroy it.
	return duckdb_get_type_id(duckdb_get_value_type(value));
}

std::string ValueToString(duckdb_value value) {
	return TakeDuckDBString(duckdb_get_varchar(value));
}

// Returns the field `name` of a STRUCT value, or an empty handle if it has none.
ValueHandle GetStructField(duckdb_value value, const std::string &name) {
	duckdb_logical_type type = duckdb_get_value_type(value);
	if (duckdb_get_type_id(type) != DUCKDB_TYPE_STRUCT) {
		return ValueHandle();
	}
	idx_t count = duckdb_struct_type_child_count(type);
	for (idx_t i = 0; i < count; i++) {
		if (TakeDuckDBString(duckdb_struct_type_child_name(type, i)) == name) {
			return ValueHandle(duckdb_get_struct_child(value, i));
		}
	}
	return ValueHandle();
}

// Reads choice labels or score levels from a LIST value:
//   ['a', 'b']   or   [{label: 'a', description: '...'}, ...]
// Returns an error message, or an empty string on success.
std::string ReadCriteria(duckdb_value criteria, DecideQuestion &question) {
	if (question.type == "noul") {
		return "yes/no (noul) questions take no criteria";
	}
	if (TypeId(criteria) != DUCKDB_TYPE_LIST) {
		return "criteria must be a list, e.g. ['low', 'medium', 'high']";
	}
	idx_t count = duckdb_get_list_size(criteria);
	for (idx_t i = 0; i < count; i++) {
		ValueHandle item(duckdb_get_list_child(criteria, i));
		if (!item || duckdb_is_null_value(item.get())) {
			return "criteria must not contain NULL";
		}
		std::string label;
		std::string description;
		if (TypeId(item.get()) == DUCKDB_TYPE_VARCHAR) {
			label = ValueToString(item.get());
			description = label;
		} else if (TypeId(item.get()) == DUCKDB_TYPE_STRUCT) {
			ValueHandle label_value = GetStructField(item.get(), "label");
			ValueHandle description_value = GetStructField(item.get(), "description");
			if (!label_value || duckdb_is_null_value(label_value.get())) {
				return "each criteria struct needs a 'label', e.g. {label: 'billing', description: '...'}";
			}
			label = ValueToString(label_value.get());
			bool has_description = description_value && !duckdb_is_null_value(description_value.get());
			description = has_description ? ValueToString(description_value.get()) : label;
		} else {
			return "criteria items must be strings or {label, description} structs";
		}

		if (question.type == "choice") {
			question.choices.emplace_back(label, description);
		} else {
			question.levels.push_back(label);
		}
	}
	return "";
}

// Reads one question of the multi-question form: {type, instructions, criteria}.
std::string ReadQuestionStruct(const std::string &id, duckdb_value value, DecideQuestion &question) {
	ValueHandle type = GetStructField(value, "type");
	ValueHandle instructions = GetStructField(value, "instructions");
	if (!type || !instructions) {
		return "question '" + id + "' must be a struct like {type: 'noul', instructions: '...'}";
	}
	question.type = ValueToString(type.get());
	question.instructions = ValueToString(instructions.get());

	ValueHandle criteria = GetStructField(value, "criteria");
	if (criteria && !duckdb_is_null_value(criteria.get())) {
		std::string error = ReadCriteria(criteria.get(), question);
		if (!error.empty()) {
			return "question '" + id + "': " + error;
		}
	}
	return "";
}

// Reads settings and the constant arguments, validates them and builds the
// request prefix. Returns an error message, or an empty string on success.
std::string Bind(duckdb_bind_info info, DecideBindData &data) {
	duckdb_client_context raw_context = nullptr;
	duckdb_scalar_function_get_client_context(info, &raw_context);
	ClientContextHandle context(raw_context);

	data.access_key = GetModelAccessKey(context.get());
	if (data.access_key.empty()) {
		return "no model access key. Set the DO_MODEL_ACCESS_KEY environment variable";
	}
	std::string model = GetStringSetting(context.get(), kDecideModelSetting);
	data.max_concurrent = GetIntSetting(context.get(), kMaxConcurrentRequestsSetting, 32);

	// Every argument after the row text must be a constant. Evaluate ("fold")
	// each one once here, instead of reading it again for every row.
	std::vector<ValueHandle> args;
	idx_t argument_count = duckdb_scalar_function_bind_get_argument_count(info);
	for (idx_t i = 1; i < argument_count; i++) {
		std::string position = "argument " + std::to_string(i + 1);
		ExpressionHandle expression(duckdb_scalar_function_bind_get_argument(info, i));
		if (!duckdb_expression_is_foldable(expression.get())) {
			return position + " must be a constant, not a column";
		}
		duckdb_value folded = nullptr;
		ErrorDataHandle error(duckdb_expression_fold(context.get(), expression.get(), &folded));
		ValueHandle value(folded);
		if (error) {
			return position + ": " + duckdb_error_data_message(error.get());
		}
		if (!value || duckdb_is_null_value(value.get())) {
			return position + " must not be NULL";
		}
		args.push_back(std::move(value));
	}

	std::vector<NamedQuestion> questions;
	if (args.size() == 1 && TypeId(args[0].get()) == DUCKDB_TYPE_VARCHAR) {
		// do_decide(text, question)
		DecideQuestion question;
		question.type = "noul";
		question.instructions = ValueToString(args[0].get());
		questions.emplace_back(kSingleQuestionId, std::move(question));
		data.single_question_id = kSingleQuestionId;
	} else if (args.size() == 1 && TypeId(args[0].get()) == DUCKDB_TYPE_STRUCT) {
		// do_decide(text, {id: {type, instructions, criteria}, ...})
		duckdb_logical_type type = duckdb_get_value_type(args[0].get());
		idx_t count = duckdb_struct_type_child_count(type);
		for (idx_t i = 0; i < count; i++) {
			std::string id = TakeDuckDBString(duckdb_struct_type_child_name(type, i));
			ValueHandle child(duckdb_get_struct_child(args[0].get(), i));
			DecideQuestion question;
			std::string error = ReadQuestionStruct(id, child.get(), question);
			if (!error.empty()) {
				return error;
			}
			questions.emplace_back(id, std::move(question));
		}
	} else if (args.size() == 1) {
		return "argument 2 must be a question string or a struct of questions";
	} else {
		// do_decide(text, question, 'choice' | 'score', criteria)
		DecideQuestion question;
		question.instructions = ValueToString(args[0].get());
		question.type = ValueToString(args[1].get());
		if (question.type != "choice" && question.type != "score") {
			return "argument 3 must be 'choice' or 'score', got '" + question.type + "'";
		}
		std::string error = ReadCriteria(args[2].get(), question);
		if (!error.empty()) {
			return error;
		}
		questions.emplace_back(kSingleQuestionId, std::move(question));
		data.single_question_id = kSingleQuestionId;
	}

	for (const NamedQuestion &question : questions) {
		std::string error = ValidateQuestion(question);
		if (!error.empty()) {
			return error;
		}
	}
	data.request_prefix = BuildRequestPrefix(model, questions);
	if (data.request_prefix.empty()) {
		return "could not build the request JSON";
	}
	return "";
}

void DecideBind(duckdb_bind_info info) {
	// C++ exceptions must never escape into DuckDB's C API, so catch everything here.
	try {
		auto data = std::make_unique<DecideBindData>();
		std::string error = Bind(info, *data);
		if (!error.empty()) {
			duckdb_scalar_function_bind_set_error(info, ("do_decide: " + error).c_str());
			return;
		}
		// DuckDB now owns the bind data and calls the lambda to delete it.
		duckdb_scalar_function_set_bind_data(info, data.release(),
		                                     [](void *ptr) { delete static_cast<DecideBindData *>(ptr); });
	} catch (std::exception &e) {
		duckdb_scalar_function_bind_set_error(info, (std::string("do_decide: ") + e.what()).c_str());
	}
}

void Execute(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	const auto &data = *static_cast<DecideBindData *>(duckdb_scalar_function_get_bind_data(info));
	idx_t row_count = duckdb_data_chunk_get_size(input);
	duckdb_vector text_vector = duckdb_data_chunk_get_vector(input, 0);
	auto *texts = static_cast<duckdb_string_t *>(duckdb_vector_get_data(text_vector));
	uint64_t *validity = duckdb_vector_get_validity(text_vector);

	duckdb_vector_ensure_validity_writable(output);
	uint64_t *output_validity = duckdb_vector_get_validity(output);

	// One request per non-NULL row. `body_rows[i]` is the row that bodies[i] belongs to.
	std::vector<std::string> bodies;
	std::vector<idx_t> body_rows;
	for (idx_t row = 0; row < row_count; row++) {
		if (!duckdb_validity_row_is_valid(validity, row)) {
			duckdb_validity_set_row_invalid(output_validity, row);
			continue;
		}
		std::string text(duckdb_string_t_data(&texts[row]), duckdb_string_t_length(texts[row]));
		std::string body = BuildRequestBody(data.request_prefix, text);
		if (body.empty()) {
			std::string result = ErrorJson(0, "row text could not be encoded as JSON");
			duckdb_vector_assign_string_element_len(output, row, result.data(), result.size());
			continue;
		}
		bodies.push_back(std::move(body));
		body_rows.push_back(row);
	}

	std::vector<HttpResponse> responses = PostJsonConcurrent(kDecideUrl, data.access_key, bodies, data.max_concurrent);

	for (size_t i = 0; i < responses.size(); i++) {
		const HttpResponse &response = responses[i];
		// A rejected key or unknown model fails every row the same way: stop the query.
		if (response.status == 401 || response.status == 403 || response.status == 404) {
			std::string message = "do_decide: request rejected (HTTP " + std::to_string(response.status) +
			                      "). Check do_model_access_key and do_decide_model. Response: " + response.body;
			duckdb_scalar_function_set_error(info, message.c_str());
			return;
		}
		std::string result;
		if (response.status == 200) {
			result = ShapeAnswers(response.body, data.single_question_id);
		} else {
			result = ErrorJson(response.status, response.status == 0 ? response.error : response.body);
		}
		duckdb_vector_assign_string_element_len(output, body_rows[i], result.data(), result.size());
	}
}

void DecideExecute(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	try {
		Execute(info, input, output);
	} catch (std::exception &e) {
		duckdb_scalar_function_set_error(info, (std::string("do_decide: ") + e.what()).c_str());
	}
}

} // namespace

bool RegisterDecideFunction(duckdb_connection connection) {
	ScalarFunctionSetHandle set(duckdb_create_scalar_function_set("do_decide"));
	LogicalTypeHandle varchar(duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
	LogicalTypeHandle any(duckdb_create_logical_type(DUCKDB_TYPE_ANY));
	// Results are JSON text: VARCHAR with the JSON alias.
	LogicalTypeHandle json(duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR));
	duckdb_logical_type_set_alias(json.get(), "JSON");

	auto add_overload = [&](const std::vector<duckdb_logical_type> &parameters) {
		ScalarFunctionHandle function(duckdb_create_scalar_function());
		duckdb_scalar_function_set_name(function.get(), "do_decide");
		for (duckdb_logical_type parameter : parameters) {
			duckdb_scalar_function_add_parameter(function.get(), parameter);
		}
		duckdb_scalar_function_set_return_type(function.get(), json.get());
		// Calls a remote model: never cache, deduplicate or constant-fold the result.
		duckdb_scalar_function_set_volatile(function.get());
		duckdb_scalar_function_set_bind(function.get(), DecideBind);
		duckdb_scalar_function_set_function(function.get(), DecideExecute);
		return duckdb_add_scalar_function_to_set(set.get(), function.get()) == DuckDBSuccess;
	};

	return add_overload({varchar.get(), any.get()}) &&
	       add_overload({varchar.get(), varchar.get(), varchar.get(), any.get()}) &&
	       duckdb_register_scalar_function_set(connection, set.get()) == DuckDBSuccess;
}