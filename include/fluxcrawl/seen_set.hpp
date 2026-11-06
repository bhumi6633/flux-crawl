#pragma once
#include <fluxcrawl/url.hpp>
#include <fluxcrawl/dedup.hpp>
#include <memory>
#include <vector>
namespace fluxcrawl {
class SqliteStore;
// Coordinator-confined logical membership. SQLite connection serialization belongs
// to the store; no backend mutex is exposed and Bloom words use this same boundary.
class SeenSet {
public:
    virtual ~SeenSet()=default;
    virtual bool insert_if_absent(const Url& url)=0;
    virtual bool contains(const Url& url)=0;
    virtual void rollback(const Url& url)=0;
    virtual std::size_t size() const=0;
    virtual std::vector<Url> export_urls() const=0;
    virtual DedupMetrics metrics()=0;
    virtual void captured(std::uint64_t, const std::vector<Url>&) {}
};
std::unique_ptr<SeenSet> memory_seen(std::size_t reserve=0);
std::unique_ptr<SeenSet> sqlite_seen(std::shared_ptr<SqliteStore> store,const DedupOptions& options);
}
