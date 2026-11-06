#pragma once
#include <fluxcrawl/crawl_types.hpp>
#include <ostream>
namespace fluxcrawl::detail {
inline void print_database_metrics(std::ostream& out, const CrawlResult& result)
{
    const auto& m = result.persistence;
    out << "Persistence mode: sqlite\nSQLite transactions: " << m.transactions
        << "\nSQLite mutations: " << m.mutations << "\nSQLite row changes: " << m.row_changes
        << "\nSQLite average batch mutations: " << (m.transactions ? static_cast<double>(m.mutations) / static_cast<double>(m.transactions) : 0)
        << "\nSQLite commit time ms: " << std::chrono::duration<double, std::milli>(m.commit_time).count()
        << "\nSQLite max commit ms: " << std::chrono::duration<double, std::milli>(m.max_commit_time).count()
        << "\nSQLite initialization ms: " << std::chrono::duration<double, std::milli>(m.initialization_time).count()
        << "\nSQLite bytes (main + WAL): " << m.file_bytes
        << "\nSQLite persistence errors: " << result.persistence_errors.size() << '\n';
}
} // namespace fluxcrawl::detail
