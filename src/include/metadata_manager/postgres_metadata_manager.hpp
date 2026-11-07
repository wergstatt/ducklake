//===----------------------------------------------------------------------===//
//                         DuckDB
//
// metadata_manager/postgres_metadata_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "storage/ducklake_metadata_manager.hpp"

namespace duckdb {

class PostgresMetadataManager : public DuckLakeMetadataManager {
public:
	explicit PostgresMetadataManager(DuckLakeTransaction &transaction);

	static unique_ptr<DuckLakeMetadataManager> Create(DuckLakeTransaction &transaction) {
		return make_uniq<PostgresMetadataManager>(transaction);
	}

	bool TypeIsNativelySupported(const LogicalType &type) override;
	bool CanInlineValues(DataChunk &chunk) override;
	bool SupportsAppender() const override {
		return false;
	}
	bool DroppedTablesStayReadable() const override {
		return false;
	}
	bool ShowsConcurrentCommits() const override;
	bool ExpiresSnapshotsInCommit() const override;
	bool InsertsInlinedRowsAfterBatch(idx_t row_count, idx_t column_count) const override;
	idx_t MaxIdentifierLength() const override {
		return 63;
	}
	idx_t MaxColumnCount() const override {
		return 1600;
	}
	vector<string> SystemColumnNames() const override {
		return {"tableoid", "xmin", "cmin", "xmax", "cmax", "ctid"};
	}

	string GetColumnTypeInternal(const LogicalType &type) override;
	string GetSnapshotAndStatsAndChangesQuery(DuckLakeSnapshot snapshot, bool include_exactness) const override;
	bool InlinedDeletionTableExists(const Identifier &table_name) override;
	void MigrateInlinedDataTypes() override;
	bool IsRetryableCommitError(const string &message) const override;
	string CommitErrorWithoutQuery(const string &message) const override;
	bool FailedBeforeCommit(const string &message) const override;
	string ConfigOptionLockSql() const override;

	unique_ptr<QueryResult> Execute(DuckLakeSnapshot snapshot, string &query) override;
	unique_ptr<QueryResult> Execute(DuckLakeSnapshot snapshot, const DuckLakeBatch &batch) override;
	unique_ptr<QueryResult> ExecuteAndCommit(DuckLakeSnapshot snapshot, const DuckLakeBatch &batch,
	                                         bool &commit_sent) override;
	unique_ptr<QueryResult> ExecuteDropInTransaction(string &query) override;
	optional_idx BatchRowCount(QueryResult &result, idx_t from_end) override;
	void DropInlinedTablesOfDeletedTables() override;
	using DuckLakeMetadataManager::Query;
	vector<unique_ptr<QueryResult>> Query(DuckLakeSnapshot snapshot, const DuckLakeReadBatch &batch) override;

	void ClearCache() override;
	//! postgres_scanner looks up a table it has not cached
	bool NewTablesRequireCacheClear() const override {
		return false;
	}

protected:
	string GetLatestSnapshotQuery() const override;
	string SnapshotTimeSql() const override {
		// the server's clock, as NOW() is the start of the metadata transaction
		return "clock_timestamp()";
	}
	string GenerateFileListQuery(DuckLakeTableEntry &table, DuckLakeSnapshot snapshot,
	                             const FilterPushdownInfo *filter_info,
	                             const vector<DuckLakeFileListDynamicFilter> &dynamic_filters,
	                             vector<idx_t> &runtime_filter_stats_columns, FileListType file_list_type,
	                             const string &metadata_table_prefix) override;
	bool MaterializeStatsCTEs() const override;
	string CastValueToTarget(const Value &value, const LogicalType &type) override;
	string CastStatsToTarget(const string &stats, const LogicalType &type, StatsCastType cast_type) override;

private:
	//! Where the server error starts in the error of the last Execute, and which of its queries failed
	optional_idx BatchServerErrorStart(const string &message, optional_ptr<idx_t> failed_query = nullptr) const;
	//! Runs statements in one round trip, with their values as parameters, keeping the results of reads if asked.
	//! With commit_sent, the COMMIT of the transaction follows them, and commit_sent is set before it goes out
	unique_ptr<QueryResult> ExecutePipeline(DuckLakeSnapshot snapshot, const vector<DuckLakeStatement> &batch,
	                                        bool keep_results = false, optional_ptr<bool> commit_sent = nullptr);
	//! Runs the reads in one round trip, and returns a query that scans the kept result of each
	vector<string> KeepResults(DuckLakeSnapshot snapshot, const DuckLakeReadBatch &batch);

	//! The queries of the last Execute, one of which the scanner repeats in front of a server error
	vector<string> executed_queries;
};

} // namespace duckdb
