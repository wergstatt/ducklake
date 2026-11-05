# Note: tests for postgres_scanner are currently not run. All of them need a postgres server running. One test
#       uses a remote rds server but that's not something we want to run here.
if (NOT MINGW AND NOT ${WASM_ENABLED})
    duckdb_extension_load(postgres_scanner
            GIT_URL https://github.com/wergstatt/duckdb-postgres
            GIT_TAG 1505c82d782dfda996bfd130f5c8fd805e63c0d0
            SUBMODULES database-connector
            )
 endif()
