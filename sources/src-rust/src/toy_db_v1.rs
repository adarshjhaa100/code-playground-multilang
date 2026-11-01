/*
Goal: We want to store, retrieve and query data efficiently and reliably compared to a simple file
Build an append only database with create (schema system), insert, update, delete, query ops (OLTP and OLAP). Add indexing, single core executor, multi client inputs.

Write test cases first, then keep on improving

Proposed Features (not limited to)
- Follow data oriented design to fit CPU cache sizes
- Storage
    - Single file table, one db, one folder, one file one table
    - Custom data structure to represent pages of db kindof tree/linkedlist like structure
        - Page -> Metadata - what table, data size per row | Row pointer list | Actual data rows
        - DO NOT EVER SHIFT EXISTING DATA FROM PAGES
        - On delete, just mark it as free, maybe a new insert will occupy that space
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


Architecture:

Client ( TCP/HTTP/RPC/IPC ) - Multithreaded Green threads - takes input, parses, sanitizes, passes to the core, parses and returns output
|
v
Orchestrator (run processes like core | garbage collector | metadata-maintainer | maintain signals on table/row/database level locks, what's running what | manage request queue to the core in case of same entity actions  etc)
|
v
Core ( Plan, Load, Execute, Store ) - Paging - linked list or some shit | Plan what to execute | Call/INFORM metadata-maintainer | Load into memory+disk space (based on available storage) | execute | persist if required | create/remove table if required, returns value
metadata-maintainer (let's make it part of core): Maintains index, list of tables, Paging metadata , positions of rows, partitions, dead data etc - Knowledge of what's in the system for fast querying
Garbage Collector: Clean up dead data (MK2 solution, first delete to be synchronous)
*/

pub fn test_cases() {
    // Create table
}

pub fn db_impl() {
    println!("Hello, World");
}
