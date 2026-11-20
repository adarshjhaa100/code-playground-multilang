
#include <stdint.h>
#include<stdio.h>

struct MasterMetadata {
    struct DB* database_list;
    // other stuff
};

enum RowType {
    TEXT,
    INTEGER,
    FLOAT,
    BOOLEAN
};

enum IndexDSType {
    BTREE,
    HASHMAP,
};

struct TableRowSchema {
    char* name;
    enum RowType type;
    uint32_t width;
};

struct TableSchema {
    struct TableRowSchema* rows;
};

struct BtreeIndex {
    // Btree impl
};

struct IndexDS {
    enum IndexDSType type;
    struct BtreeIndex* indexDS;
};

struct DBTableIndex {
    // Index on Rows
    struct TableRowSchema* rows;
    struct Table* table;
    struct IndexDS* indexDS;
};

struct TableDataRow {
    uint64_t id;
    char *data; // row separator ;-;, ends with ;el;
};

struct Table {
    char* name;
    uint32_t page_no_start;
    struct TableSchema schema;
    struct TableDataRow* tableDataRow;
    struct DB* db; // looks like cyclic reference, need to understand this
    struct DBPage* start_page; // what about deleted pages????
};

struct DBPage {
    // DB dat file: page1, page2, ....
    uint32_t no;
    uint64_t file_offset;
};

// this itself shall be stored in a master metadata
struct DB {
    // DB is basically a list of pages and metadata
    char* name;
    char* path;
    char* metadata_path;
    struct DBPage* page_list;
    struct Table* table_list;
    // some other stuff
};

struct DB create_db(struct MasterMetadata* masterMetadata, char *name) {
    // create metadata with index, tables, rows etc.
    // create main_db file (allocate with init size): Allocate a fixed initial size on the main_db file using posix_fallocate and mmap (some specific number of pages)
    return (struct DB) {};
}

struct Table create_table(struct DB* db) {
    // create metadata with schema, posn, index, rows
    // allocate size for table in db
    //
    return (struct Table) {};
}

void lock_table_entity(){
    // based on entitytype
    // could be a specific row(s) of a table, or a page or a table or the DB itself
    // Maintain a global concurrently accesible DS, GlobalDBLock
    // Any crud operation checks up on it
}

void insert_to_table(struct Table* table, struct TableDataRow* tableDataRow){
    // calculate page, and insert
    // if first entry in table or not enough space on table -> new page
    // Check for next empty page
    // calculate id
    // calculate posn and size to insert
    // Allocate and lock space for write
    // save data
    // Update index
    // Release lock
}

struct TableQueryPlan {
    char* rawQuery;
    struct Table* table;
    // query plan Graph/DAG
};

struct QueryResult {
    char** columns;
    struct TableDataRow* rows;
};

struct QueryResult query_table(struct Table* table, char* raw_query) {
    // read select statement
    // parse statement
    // Create a query plan Graph/Tree, use index if calculation says otherwise fulltable scan
    // Calculate and lock rows, Execute query plan
    // return results
    return (struct QueryResult) {};
}

void vaccum() {
    // go over the global garbage collector
    // calculate
    // acquire locks to pages and rows
    // move rows to empty spaces
    // VACCUM per table only don't move data of two tables in same page
}

void delete_table_rows(struct Table* table, char* raw_query) {
    // read delete statement
    // Parse statement
    // Calculate rows to delete (posns to clear)
    // Acquire locks on rows/pages
    // perform operation (replace the data with 0's instead of blank space)
    // Mark delete in a global garbage collector, this will be later cleared up using recurring vaccum()
    // update index
}

void test_cases() {
    // run: create_db(struct MasterMetadata* masterMetadata, char *name)
    // run: create_table(struct DB* db)
    // run: insert_to_table(struct Table* table)
    // run: query_table
    // run: delete_rows
}

// gcc
int main(int argc, char *argv[]) {
    // everything to have a metadata file and dat file: DB, Table, indexes
    return 0;
}
