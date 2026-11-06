#include <fluxcrawl/seen_set.hpp>
#include <fluxcrawl/bloom_filter.hpp>
#include <fluxcrawl/sqlite_store.hpp>
#include <unordered_set>
#include <unordered_map>
#include <deque>
#include <stdexcept>
namespace fluxcrawl {
namespace {
using Clock=std::chrono::steady_clock;
class MemorySeen final:public SeenSet {
    std::unordered_set<Url> urls_;
    DedupMetrics stats_;
public:
    explicit MemorySeen(std::size_t reserve){if(reserve){urls_.reserve(reserve);}}
    bool insert_if_absent(const Url& url) override {
        ++stats_.insert_attempts;const auto began=Clock::now();const auto buckets=urls_.bucket_count();
        const bool fresh=urls_.insert(url).second;stats_.insert_time+=Clock::now()-began;
        if(urls_.bucket_count()!=buckets){++stats_.growth_rehashes;}
        if(fresh){++stats_.accepted;}else{++stats_.confirmed_duplicates;}return fresh;
    }
    bool contains(const Url& url) override {++stats_.queries;const auto began=Clock::now();const bool found=urls_.contains(url);stats_.lookup_time+=Clock::now()-began;return found;}
    void rollback(const Url& url) override {if(urls_.erase(url)){--stats_.accepted;}}
    std::size_t size() const override{return urls_.size();}
    std::vector<Url> export_urls() const override{return {urls_.begin(),urls_.end()};}
    DedupMetrics metrics() override {auto s=stats_;s.buckets=urls_.bucket_count();s.load_factor=urls_.load_factor();return s;}
};
class SqliteSeen final:public SeenSet {
    std::shared_ptr<SqliteStore> store_;
    std::optional<BloomFilter> bloom_;
    std::unordered_map<Url,std::uint64_t> pending_;
    std::deque<std::pair<std::uint64_t,Url>> captured_;
    std::size_t count_;
    DedupMetrics stats_;
    void prune() {
        const auto committed=store_->committed_sequence();
        while(!captured_.empty() && captured_.front().first<=committed) {
            pending_.erase(captured_.front().second);captured_.pop_front();
        }
    }
    bool check(const Url& url) {
        ++stats_.queries;
        if(bloom_) {
            ++stats_.bloom_queries;
            if(!bloom_->maybe_contains(url.str())) {++stats_.bloom_definite_misses;return false;}
            ++stats_.bloom_maybe_hits;++stats_.exact_checks_after_bloom;
        }
        if(pending_.contains(url)){++stats_.pending_cache_hits;return true;}
        ++stats_.exact_db_queries;const bool found=store_->seen_contains(url);
        if(bloom_&&!found){++stats_.bloom_false_positives;}
        return found;
    }
public:
    SqliteSeen(std::shared_ptr<SqliteStore> store,const DedupOptions& options):store_{std::move(store)},count_{store_->seen_count()} {
        options.validate();
        if(options.mode==DedupMode::BloomSqlite) {
            bloom_.emplace(options.expected_elements,options.false_positive_rate);
            const auto began=Clock::now();store_->visit_seen([&](const Url& url){bloom_->insert(url.str());});stats_.rebuild_time=Clock::now()-began;
        }
    }
    bool contains(const Url& url) override {prune();const auto began=Clock::now();const bool present=check(url);stats_.lookup_time+=Clock::now()-began;return present;}
    bool insert_if_absent(const Url& url) override {
        prune();++stats_.insert_attempts;const auto began=Clock::now();const bool present=check(url);stats_.lookup_time+=Clock::now()-began;
        if(present){++stats_.confirmed_duplicates;return false;}
        const auto inserted=Clock::now();pending_.emplace(url,0);
        if(bloom_){bloom_->insert(url.str());}
        ++count_;++stats_.accepted;stats_.pending_cache_peak=std::max(stats_.pending_cache_peak,pending_.size());stats_.insert_time+=Clock::now()-inserted;return true;
    }
    void rollback(const Url& url) override {if(pending_.erase(url)){--count_;--stats_.accepted;}} // Stray Bloom positives only trigger exact checks.
    void captured(std::uint64_t sequence,const std::vector<Url>& urls) override {
        for(const auto& url:urls){const auto it=pending_.find(url);if(it==pending_.end()||it->second!=0){throw std::logic_error("Uncommitted seen cache disagrees with discovery batch");}it->second=sequence;captured_.emplace_back(sequence,url);}
        prune();
    }
    std::size_t size() const override{return count_;}
    std::vector<Url> export_urls() const override{throw std::logic_error("SQLite membership cannot export a whole RAM seen set; use its database");}
    DedupMetrics metrics() override {
        prune(); auto stats=stats_;stats.pending_cache_size=pending_.size();
        if(bloom_){stats.bloom_bits=bloom_->bit_count();stats.bloom_bytes=bloom_->byte_count();stats.bloom_probes=bloom_->probes();stats.bloom_insertions=bloom_->insertions();stats.bloom_set_bits=bloom_->set_bits();}return stats;
    }
};
}
std::unique_ptr<SeenSet> memory_seen(std::size_t reserve){return std::make_unique<MemorySeen>(reserve);}
std::unique_ptr<SeenSet> sqlite_seen(std::shared_ptr<SqliteStore> store,const DedupOptions& options){return std::make_unique<SqliteSeen>(std::move(store),options);}
}
