#include "storage/ducklake_statement.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/parser.hpp"

namespace duckdb {

//! PostgreSQL accepts at most 65535 parameters in a statement
static constexpr idx_t MAX_STATEMENT_PARAMETERS = 65535;

DuckLakeBatch::DuckLakeBatch(string sql) {
	if (!sql.empty()) {
		statements.push_back({std::move(sql), {}});
	}
}

DuckLakeBatch::DuckLakeBatch(const char *sql) : DuckLakeBatch(string(sql)) {
}

DuckLakeBatch::DuckLakeBatch(DuckLakeStatement statement) {
	statements.push_back(std::move(statement));
}

DuckLakeBatch &DuckLakeBatch::operator+=(DuckLakeBatch other) {
	for (auto &statement : other.statements) {
		if (statement.sql.empty()) {
			continue;
		}
		// text without values is joined, so that it still runs as one query where it did before
		if (statement.params.empty() && !statements.empty() && statements.back().params.empty()) {
			statements.back().sql += statement.sql;
			continue;
		}
		statements.push_back(std::move(statement));
	}
	return *this;
}

DuckLakeBatch DuckLakeBatch::operator+(DuckLakeBatch other) const {
	auto result = *this;
	result += std::move(other);
	return result;
}

bool DuckLakeBatch::empty() const {
	return statements.empty();
}

void DuckLakeBatch::TransformSql(const std::function<void(string &)> &transform) {
	for (auto &statement : statements) {
		transform(statement.sql);
	}
}

static vector<string> SplitAtSemicolons(const string &sql) {
	vector<string> result;
	idx_t start = 0;
	auto add_until = [&](idx_t end) {
		auto statement = sql.substr(start, end - start);
		StringUtil::Trim(statement);
		if (!statement.empty()) {
			result.push_back(std::move(statement));
		}
	};
	for (auto &token : Parser::Tokenize(sql)) {
		if (token.type == SimplifiedTokenType::SIMPLIFIED_TOKEN_OPERATOR && sql[token.start] == ';') {
			add_until(token.start);
			start = token.start + 1;
		}
	}
	add_until(sql.size());
	return result;
}

vector<DuckLakeStatement> DuckLakeBatch::SplitStatements() const {
	vector<DuckLakeStatement> result;
	for (auto &statement : statements) {
		if (!statement.params.empty()) {
			result.push_back(statement);
			continue;
		}
		for (auto &sql : SplitAtSemicolons(statement.sql)) {
			result.push_back({std::move(sql), {}});
		}
	}
	return result;
}

string DuckLakeBatch::ToString() const {
	string result;
	for (auto &statement : statements) {
		result += statement.sql;
		if (!StringUtil::EndsWith(statement.sql, ";")) {
			result += ";";
		}
		result += "\n";
	}
	return result;
}

static string ReplaceParameters(const DuckLakeStatement &statement) {
	string result;
	auto &sql = statement.sql;
	for (idx_t i = 0; i < sql.size(); i++) {
		if (sql[i] != '$' || i + 1 >= sql.size() || !StringUtil::CharacterIsDigit(sql[i + 1])) {
			result += sql[i];
			continue;
		}
		idx_t parameter = 0;
		while (i + 1 < sql.size() && StringUtil::CharacterIsDigit(sql[i + 1])) {
			parameter = parameter * 10 + idx_t(sql[++i] - '0');
		}
		if (parameter == 0 || parameter > statement.params.size()) {
			throw InternalException("DuckLake statement refers to parameter $%d of %d", parameter,
			                        statement.params.size());
		}
		result += statement.params[parameter - 1].ToSQLString();
	}
	return result;
}

DuckLakeBatch DuckLakeBatch::WithLiterals() const {
	DuckLakeBatch result;
	for (auto &statement : statements) {
		auto sql = statement.params.empty() ? statement.sql : ReplaceParameters(statement) + ";\n";
		result += DuckLakeBatch(std::move(sql));
	}
	return result;
}

//! The template with its ? numbered as parameters, from first_parameter on
static string NumberParameters(const string &row_template, idx_t first_parameter) {
	string result;
	auto parameter = first_parameter;
	for (auto c : row_template) {
		if (c == '?') {
			result += "$" + to_string(parameter++);
		} else {
			result += c;
		}
	}
	return result;
}

DuckLakeBatch DuckLakeBatch::InsertRows(const string &qualified_table_name, const string &row_template,
                                        const vector<vector<Value>> &rows) {
	DuckLakeBatch result;
	if (rows.empty()) {
		return result;
	}
	auto values_per_row = MaxValue<idx_t>(rows[0].size(), 1);
	auto rows_per_statement = MAX_STATEMENT_PARAMETERS / values_per_row;
	for (idx_t start = 0; start < rows.size(); start += rows_per_statement) {
		auto end = MinValue<idx_t>(start + rows_per_statement, rows.size());
		DuckLakeStatement statement;
		statement.sql = "INSERT INTO " + qualified_table_name + " VALUES ";
		for (idx_t row = start; row < end; row++) {
			if (row > start) {
				statement.sql += ", ";
			}
			statement.sql += NumberParameters(row_template, statement.params.size() + 1);
			for (auto &value : rows[row]) {
				statement.params.push_back(value);
			}
		}
		result.statements.push_back(std::move(statement));
	}
	return result;
}

} // namespace duckdb
