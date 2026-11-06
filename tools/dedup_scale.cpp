#include <fluxcrawl/seen_set.hpp>
#include <fluxcrawl/sqlite_store.hpp>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
using namespace fluxcrawl;
using Clock=std::chrono::steady_clock;
double ms(Clock::duration d){return std::chrono::duration<double,std::milli>(d).count();}
Url url(std::size_t n){return Url::parse("https://host.test/page/"+std::to_string(n)).value();}
int main(int argc,char** argv){try{
#ifndef NDEBUG
    throw std::runtime_error("Use Release for measurements");
#endif
    if(argc!=6){throw std::invalid_argument("Usage: dedup_scale MODE COUNT EXPECTED FP NEW_DB_PATH");}
    const std::string mode=argv[1];const auto count=std::stoull(argv[2]),expected=std::stoull(argv[3]);const auto probability=std::stod(argv[4]);const std::filesystem::path path=argv[5];
    if(!count){throw std::invalid_argument("positive count required");}
    DedupOptions options;options.mode=mode=="sqlite"?DedupMode::Sqlite:mode=="bloom-sqlite"?DedupMode::BloomSqlite:DedupMode::Memory;options.expected_elements=expected;options.false_positive_rate=probability;
    options.memory_reserve=mode=="memory-reserve"?count:0;options.validate();
    CrawlCheckpoint metadata{.seed=url(0)};metadata.options.max_pages=count+1;metadata.options.dedup=options;metadata.seen={metadata.seed};metadata.counters.urls_discovered=metadata.counters.resources_completed=metadata.counters.pages_attempted=metadata.counters.responses_received=1;metadata.completed=true;
    std::shared_ptr<SqliteStore> store;std::unique_ptr<SeenSet> seen;
    const auto initialized=Clock::now();
    if(options.mode==DedupMode::Memory){seen=memory_seen(options.memory_reserve);(void)seen->insert_if_absent(metadata.seed);}
    else{store=SqliteStore::create(path,metadata);seen=sqlite_seen(store,options);}
    const auto init_time=Clock::now()-initialized;metadata=CheckpointManager::metadata_only(metadata);
    CrawlMutationBatch batch{metadata,{},{},0,0};Clock::duration insertion{};
    const auto flush=[&]{if(!store||batch.discoveries.empty()){return;}++metadata.sequence;metadata.counters.urls_discovered+=batch.discoveries.size();metadata.counters.resources_completed+=batch.discoveries.size();metadata.counters.pages_attempted+=batch.discoveries.size();metadata.counters.responses_received+=batch.discoveries.size();batch.metadata=metadata;batch.operations=2*batch.discoveries.size();seen->captured(metadata.sequence,batch.discoveries);store->apply(batch);batch.discoveries.clear();batch.changes.clear();};
    const auto began=Clock::now();
    for(std::size_t n=1;n<count;++n){auto u=url(n);const auto start=Clock::now();if(!seen->insert_if_absent(u)){throw std::runtime_error("lost synthetic URL");}insertion+=Clock::now()-start;if(store){batch.discoveries.push_back(u);batch.changes.push_back({u,{},0});if(batch.discoveries.size()==100){flush();}}}
    flush();const auto population=Clock::now()-began;
    if(seen->size()!=count){throw std::runtime_error("seen count mismatch");}
    const auto base_stats=seen->metrics();const auto persistence=store?store->metrics():PersistenceMetrics{};
    struct Row {unsigned absent;double query_ms;std::size_t queries,fp,misses,maybes;};std::vector<Row> rows;
    for(unsigned absent:{95U,20U,50U}){
        const auto before=seen->metrics();Clock::duration query_time{};
        for(std::size_t n=0;n<100000;++n){const bool is_new=n%100<absent;auto u=url(is_new?count+n:(n*2654435761ULL)%count);const auto start=Clock::now();const auto found=seen->contains(u);query_time+=Clock::now()-start;if(found==is_new){throw std::runtime_error("exact membership mismatch");}}
        const auto after=seen->metrics();rows.push_back({absent,ms(query_time),after.exact_db_queries-before.exact_db_queries,after.bloom_false_positives-before.bloom_false_positives,after.bloom_definite_misses-before.bloom_definite_misses,after.bloom_maybe_hits-before.bloom_maybe_hits});
    }
    seen.reset();store.reset();std::size_t bytes=options.mode==DedupMode::Memory?0:std::filesystem::file_size(path);
    double opened=0,loaded=0,rebuild=0,startup=0;
    if(options.mode!=DedupMode::Memory){const auto startup_begin=Clock::now();auto start=startup_begin;store=SqliteStore::open(path);opened=ms(Clock::now()-start);start=Clock::now();auto restored=store->load();loaded=ms(Clock::now()-start);if(!restored.seen.empty()||restored.external_seen_count!=count){throw std::runtime_error("restore materialized exact seen set");}seen=sqlite_seen(store,options);rebuild=ms(seen->metrics().rebuild_time);startup=ms(Clock::now()-startup_begin);for(std::size_t n=0;n<1000;++n){if(!seen->contains(url((n*9973)%count))){throw std::runtime_error("rebuild false negative");}}seen.reset();store.reset();}
    std::cout<<"mode,count,expected,target_fp,absent_percent,initialization_ms,insert_calls_ms,population_ms,lookup_100k_ms,lookup_db_queries,false_positives,definite_misses,maybe_hits,bloom_bytes,bloom_bits,bloom_probes,bloom_set_bits,population_db_queries,pending_cache_peak,transactions,commit_ms,closed_db_bytes,open_ms,load_validation_ms,bloom_rebuild_ms,total_restore_ms\n"<<std::fixed<<std::setprecision(6);
    for(const auto& r:rows){std::cout<<mode<<','<<count<<','<<expected<<','<<probability<<','<<r.absent<<','<<ms(init_time)<<','<<ms(insertion)<<','<<ms(population)<<','<<r.query_ms<<','<<r.queries<<','<<r.fp<<','<<r.misses<<','<<r.maybes<<','<<base_stats.bloom_bytes<<','<<base_stats.bloom_bits<<','<<base_stats.bloom_probes<<','<<base_stats.bloom_set_bits<<','<<base_stats.exact_db_queries<<','<<base_stats.pending_cache_peak<<','<<persistence.transactions<<','<<ms(persistence.commit_time)<<','<<bytes<<','<<opened<<','<<loaded<<','<<rebuild<<','<<startup<<'\n';}
    if(options.mode!=DedupMode::Memory){std::filesystem::remove(path);std::filesystem::remove(path.string()+".owner");}
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
