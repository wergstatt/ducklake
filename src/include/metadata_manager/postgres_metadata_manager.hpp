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
	bool InsertsInlinedRowsAfterBatch(idx_t row_count) const override;
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
	string ConfigOptionLockSql() const override;

	unique_ptr<QueryResult> Execute(DuckLakeBatch &batch) override;
	DuckLakeCatalogInfo GetCatalogForSnapshot(DuckLakeSnapshot snapshot) override;
	unique_ptr<QueryResult> ExecuteAndCommit(DuckLakeSnapshot snapshot, DuckLakeBatch &batch) override;
	//! The expression that orders the bounds of a type on the server, empty if the server cannot merge them
	string BoundOrderExpression(const string &bound, const LogicalType &type);
	bool CanSkipSnapshotFetch(const TransactionChangeInformation &changes) const override;
	void FlushChangesServerSide(DuckLakeTransaction &transaction, DuckLakeSnapshot transaction_snapshot,
	                            const TransactionChangeInformation &transaction_changes,
	                            const DuckLakeRetryConfig &retry_config) override;
	unique_ptr<QueryResult> ExecuteDropInTransaction(string &query) override;

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
	                             const vector<idx_t> &runtime_filter_stats_columns, FileListType file_list_type,
	                             const string &metadata_table_prefix) override;
	bool MaterializeStatsCTEs() const override;
	string CastValueToTarget(const Value &value, const LogicalType &type) override;
	string CastStatsToTarget(const string &stats, const LogicalType &type, StatsCastType cast_type) override;

private:
	//! Runs the statements of a batch in one pipeline, which commits the transaction if commit is set
	unique_ptr<QueryResult> ExecutePipeline(DuckLakeBatch &batch, bool commit);
	//! Whether a failed pipeline commit falls back to the client-side commit
	bool RetriesOnClient(const string &message) const;
	//! The statements of a commit of new data files that resolves its ids and stats on the server, empty if the
	//! commit needs the client-side path. snapshot_statement is the statement that returns the new snapshot id
	vector<Value> PipelineCommitStatements(DuckLakeSnapshot transaction_snapshot,
	                                       const TransactionChangeInformation &changes, idx_t &snapshot_statement);
};

} // namespace duckdb
