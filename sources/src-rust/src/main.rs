// Run: rustc src/main.rs -o target/main && ./target/main
// Build an append only database with insert, update, delete, query ops (OLTP and OLAP). Add indexing, single core executor, multi client inputs
mod toy_db_v1;

fn formatted_print() {}

fn main() {
    // Hello, World
    println!("Hello, World!");

    toy_db_v1::db_impl();
}
