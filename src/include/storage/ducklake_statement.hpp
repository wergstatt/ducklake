//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_statement.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/value.hpp"

namespace duckdb {

//! A statement on the metadata, with the values it refers to as $1, $2, ...
//! Text without values may hold several statements
struct DuckLakeStatement {
	string sql;
	vector<Value> params;
};

//! The statements that DuckLake writes to the metadata in one go
class DuckLakeBatch {
public:
	DuckLakeBatch() = default;
	DuckLakeBatch(string sql);                  // NOLINT: text without values converts implicitly
	DuckLakeBatch(const char *sql);             // NOLINT
	DuckLakeBatch(DuckLakeStatement statement); // NOLINT

	DuckLakeBatch &operator+=(DuckLakeBatch other);
	DuckLakeBatch operator+(DuckLakeBatch other) const;

	bool empty() const;
	vector<DuckLakeStatement> &Statements() {
		return statements;
	}
	const vector<DuckLakeStatement> &Statements() const {
		return statements;
	}
	//! Applies a function to the text of every statement, such as the substitution of placeholders
	void TransformSql(const std::function<void(string &)> &transform);
	//! The statements one by one, with text without values split at its semicolons
	vector<DuckLakeStatement> SplitStatements() const;
	//! The text of all statements, for logs and errors
	string ToString() const;
	//! The batch as text without values, with each value written as a literal, for servers that cannot bind them
	DuckLakeBatch WithLiterals() const;

	//! Inserts rows into a table, in statements that stay within the parameter limit of servers. The row template
	//! gives the expression of each column, with a ? for each value of a row, such as "(?, {SNAPSHOT_ID}, ?)"
	static DuckLakeBatch InsertRows(const string &qualified_table_name, const string &row_template,
	                                const vector<vector<Value>> &rows);

private:
	vector<DuckLakeStatement> statements;
};

} // namespace duckdb
