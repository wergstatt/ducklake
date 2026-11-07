#include "storage/ducklake_statement.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parser.hpp"

namespace duckdb {

//! The text with each parameter $n replaced, skipping literals, quoted identifiers and comments
static string ReplaceParameters(const string &sql, const std::function<string(idx_t)> &replace) {
	string result;
	idx_t copied = 0;
	for (auto &token : Parser::Tokenize(sql)) {
		if (token.type != SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR || sql[token.start] != '$') {
			continue;
		}
		auto end = token.start + 1;
		while (end < sql.size() && StringUtil::CharacterIsDigit(sql[end])) {
			end++;
		}
		if (end == token.start + 1) {
			continue;
		}
		result += sql.substr(copied, token.start - copied);
		result += replace(std::stoull(sql.substr(token.start + 1, end - token.start - 1)));
		copied = end;
	}
	return result + sql.substr(copied);
}

void DuckLakeStatement::Append(const DuckLakeStatement &fragment) {
	auto offset = params.size();
	sql += ReplaceParameters(fragment.sql, [&](idx_t parameter) { return "$" + to_string(offset + parameter); });
	params.insert(params.end(), fragment.params.begin(), fragment.params.end());
}

string DuckLakeStatement::ToSQL() const {
	if (params.empty()) {
		return sql;
	}
	return ReplaceParameters(sql, [&](idx_t parameter) {
		if (parameter == 0 || parameter > params.size()) {
			throw InternalException("DuckLake statement refers to parameter $%d of %d", parameter, params.size());
		}
		auto &value = params[parameter - 1];
		return value.IsNull() ? string("NULL") : value.ToSQLString();
	});
}

DuckLakeBatch::DuckLakeBatch(string sql) {
	if (!sql.empty()) {
		statements.push_back({std::move(sql), {}});
	}
}

DuckLakeBatch::DuckLakeBatch(DuckLakeStatement statement) {
	statements.push_back(std::move(statement));
}

DuckLakeBatch &DuckLakeBatch::operator+=(DuckLakeBatch other) {
	for (auto &statement : other.statements) {
		statements.push_back(std::move(statement));
	}
	return *this;
}

string DuckLakeBatch::ToSQL() const {
	string result;
	for (auto &statement : statements) {
		result += statement.ToSQL();
	}
	return result;
}

} // namespace duckdb
