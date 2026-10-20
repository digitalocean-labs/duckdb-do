#include "common/bind_args.hpp"

#include "common/config.hpp"
#include "common/duckdb_handles.hpp"

bool FoldConstantText(duckdb_bind_info info, duckdb_client_context context, idx_t index, std::string &out,
                      std::string &error) {
	const std::string position = "argument " + std::to_string(index + 1);
	ExpressionHandle expression(duckdb_scalar_function_bind_get_argument(info, index));
	if (!duckdb_expression_is_foldable(expression.get())) {
		error = position + " must be a constant, not a column";
		return false;
	}
	duckdb_value folded = nullptr;
	ErrorDataHandle fold_error(duckdb_expression_fold(context, expression.get(), &folded));
	ValueHandle value(folded);
	if (fold_error) {
		error = position + ": " + duckdb_error_data_message(fold_error.get());
		return false;
	}
	if (!value || duckdb_is_null_value(value.get())) {
		error = position + " must not be NULL";
		return false;
	}
	out = TakeDuckDBString(duckdb_get_varchar(value.get()));
	return true;
}

bool BindInference(duckdb_bind_info info, const char *model_setting, InferenceBind &out, std::string &error) {
	duckdb_client_context raw_context = nullptr;
	duckdb_scalar_function_get_client_context(info, &raw_context);
	ClientContextHandle context(raw_context);

	out.access_key = GetModelAccessKey(context.get());
	if (out.access_key.empty()) {
		error = "no model access key. Set the DO_MODEL_ACCESS_KEY environment variable";
		return false;
	}
	out.max_concurrent = GetIntSetting(context.get(), kMaxConcurrentRequestsSetting, 32);
	if (duckdb_scalar_function_bind_get_argument_count(info) > 1) {
		if (!FoldConstantText(info, context.get(), 1, out.model, error)) {
			return false;
		}
	} else {
		out.model = GetStringSetting(context.get(), model_setting);
	}
	if (out.model.empty()) {
		error = "no model given";
		return false;
	}
	return true;
}
