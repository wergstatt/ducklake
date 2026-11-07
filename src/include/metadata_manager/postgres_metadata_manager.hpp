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
	unique_ptr<QueryResult> Query(const DuckLakeStatement &query) override;
	vector<unique_ptr<QueryResult>> Query(DuckLakeSnapshot snapshot, const DuckLakeReadBatch &batch) override;
	//! Reads the rows of every inlined table of a flush in one round trip, without looking up the tables
	void ReadInlinedTablesForFlush(DuckLakeSnapshot snapshot, const vector<Identifier> &inlined_table_names) override;
	unique_ptr<QueryResult> ReadAllInlinedDataForFlush(DuckLakeSnapshot snapshot, const Identifier &inlined_table_name,
	                                                   const DuckLakeTableEntry &table, const string &sort_order_sql,
	                                                   const vector<string> &columns_to_read) override;
	unique_ptr<QueryResult> ExecuteAndQuery(DuckLakeSnapshot snapshot, const string &writes,
	                                        const DuckLakeReadBatch &reads,
	                                        vector<unique_ptr<QueryResult>> &results) override;
	DuckLakeCatalogInfo GetCatalogForSnapshot(DuckLakeSnapshot snapshot) override;
	vector<DuckLakeGlobalStatsInfo> GetGlobalTableStats(DuckLakeSnapshot snapshot,
	                                                    const vector<TableIndex> &table_ids) override;
	bool HoldsGlobalTableStats(DuckLakeSnapshot snapshot) override;
	using DuckLakeMetadataManager::GetSnapshot;
	//! Reads the latest snapshot together with each read that the caches miss for it, in one round trip
	unique_ptr<DuckLakeSnapshot> GetSnapshot() override;
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
	//! Where the server error starts in the error of the last Execute, and which of its queries failed
	optional_idx BatchServerErrorStart(const string &message, optional_ptr<idx_t> failed_query = nullptr) const;
	//! Runs statements in one round trip, with their values as parameters, keeping the results of reads if asked.
	//! With commit_sent, the COMMIT of the transaction follows them, and commit_sent is set before it goes out
	unique_ptr<QueryResult> ExecutePipeline(DuckLakeSnapshot snapshot, const vector<DuckLakeStatement> &batch,
	                                        bool keep_results = false, optional_ptr<bool> commit_sent = nullptr);
	//! Runs the reads in one round trip, and returns a query that scans the kept result of each
	vector<string> KeepResults(DuckLakeSnapshot snapshot, const DuckLakeReadBatch &batch);
	//! A query that scans each result that a pipeline kept
	vector<string> KeptResultScans(QueryResult &pipeline);
	//! Builds the catalog from the kept results of the reads of its tables
	DuckLakeCatalogInfo BuildCatalog(DuckLakeSnapshot snapshot, const vector<string> &tables,
	                                 const vector<string> &scans);
	//! The query that lists the inlined deletion tables
	string InlinedDeletionTablesQuery() const;

	//! The queries of the last Execute, one of which the scanner repeats in front of a server error
	vector<string> executed_queries;
	//! The kept results of the inlined tables that a flush read ahead, by snapshot and inlined table
	mutex flush_rows_lock;
	map<pair<idx_t, string>, string> flush_rows;
	//! The stats of every table are read with the snapshot for a catalog of at most this many tables
	static constexpr idx_t MAX_TABLES_WITH_STATS_READ_AHEAD = 100;
	mutex read_ahead_lock;
	//! The catalog read with the snapshot, until it is loaded, and its schema version
	unique_ptr<pair<idx_t, DuckLakeCatalogInfo>> read_ahead_catalog;
	//! The stats of every table read with the snapshot, and its snapshot id
	unique_ptr<pair<idx_t, vector<DuckLakeGlobalStatsInfo>>> read_ahead_stats;
	//! The kept results of the inlined rows that a scan read with its file list, by snapshot and inlined table
	mutex inlined_rows_lock;
	map<pair<idx_t, string>, string> inlined_rows;
};

} // namespace duckdb
