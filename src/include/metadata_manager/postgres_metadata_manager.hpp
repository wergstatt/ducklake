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
	string ConfigOptionLockSql() const override;

	unique_ptr<QueryResult> Execute(DuckLakeSnapshot snapshot, string &query) override;
	unique_ptr<QueryResult> Execute(DuckLakeSnapshot snapshot, const DuckLakeBatch &batch) override;
	unique_ptr<QueryResult> ExecuteDropInTransaction(string &query) override;
	using DuckLakeMetadataManager::Query;
	vector<unique_ptr<QueryResult>> Query(DuckLakeSnapshot snapshot, const DuckLakeReadBatch &batch) override;
	//! Reads the file list, the inlined file deletions and the inlined rows of a scan in one round trip
	vector<DuckLakeFileListEntry> GetFilesForTable(DuckLakeTableEntry &table, DuckLakeSnapshot snapshot,
	                                               const FilterPushdownInfo *filter_info,
	                                               bool include_local_changes) override;
	unique_ptr<QueryResult> ReadInlinedData(DuckLakeSnapshot snapshot, const Identifier &inlined_table_name,
	                                        const vector<string> &columns_to_read) override;

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
	//! Where the server error starts in the error of the last Execute
	optional_idx BatchServerErrorStart(const string &message) const;
	//! Runs statements in one round trip, with their values as parameters, keeping the results of reads if asked
	unique_ptr<QueryResult> ExecutePipeline(DuckLakeSnapshot snapshot, const vector<DuckLakeStatement> &batch,
	                                        bool keep_results = false);
	//! Runs the reads in one round trip, and returns a query that scans the kept result of each
	vector<string> KeepResults(DuckLakeSnapshot snapshot, const DuckLakeReadBatch &batch);

	//! The queries of the last Execute, one of which the scanner repeats in front of a server error
	vector<string> executed_queries;
	//! The kept results of the inlined rows that a scan read with its file list, by snapshot and inlined table
	mutex inlined_rows_lock;
	map<pair<idx_t, string>, string> inlined_rows;
};

} // namespace duckdb
