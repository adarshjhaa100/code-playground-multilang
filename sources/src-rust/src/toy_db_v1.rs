/*
Goal: We want to store, retrieve and query data efficiently and reliably compared to a simple file
Build an append only database with create (schema system), insert, update, delete, query ops (OLTP and OLAP). Add indexing, single core executor, multi client inputs.

Write test cases first, then keep on improving

Proposed Features (not limited to)
- Storage
    - Single file table, one db, one folder, one file one table
    - Separator based storage
    - Row wise storage
    - Efficient packing
    - In memory as well as persistent
- types ( int, float, text )
- CRUD ops allowed
- Select querying - fast fetch rows, cols with a query plan in place
- OLTP - ACID (mimic, not necessarily true)
- OLAP operations like sum, avg, concat etc.
- query using syntax from functional lang, redis or sql your choice
 */
pub fn db_impl() {
    println!("Hello, World");
}
