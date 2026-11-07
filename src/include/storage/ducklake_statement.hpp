//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/ducklake_statement.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

//! A statement on the metadata catalog, with the values that its text refers to as $1, $2, ...
struct DuckLakeStatement {
	string sql;
	vector<Value> params;

	//! Appends text that refers to its own values as $1, $2, ..., such as a row of an INSERT
	void Append(const DuckLakeStatement &fragment);
	//! The text with each value written as a literal
	string ToSQL() const;
};

//! The statements that DuckLake runs on the metadata catalog in one go, such as those of a commit
class DuckLakeBatch {
public:
	DuckLakeBatch() = default;
	//! Statements without values
	DuckLakeBatch(string sql);                  // NOLINT: allow implicit conversion
	DuckLakeBatch(DuckLakeStatement statement); // NOLINT: allow implicit conversion

	DuckLakeBatch &operator+=(DuckLakeBatch other);

	bool empty() const {
		return statements.empty();
	}
	//! The statements as one text, with each value written as a literal
	string ToSQL() const;

private:
	vector<DuckLakeStatement> statements;
};

//! The reads that DuckLake runs on the metadata catalog in one go, each with a result of its own
class DuckLakeReadBatch {
public:
	//! Adds a read and returns the position of its result
	idx_t Add(DuckLakeStatement read) {
		reads.push_back(std::move(read));
		return reads.size() - 1;
	}
	const vector<DuckLakeStatement> &GetReads() const {
		return reads;
	}

private:
	vector<DuckLakeStatement> reads;
};

} // namespace duckdb
