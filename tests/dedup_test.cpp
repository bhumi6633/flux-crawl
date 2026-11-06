#include <fluxcrawl/bloom_filter.hpp>
#include <fluxcrawl/seen_set.hpp>
#include <fluxcrawl/sqlite_store.hpp>
#include <filesystem>
#include <iostream>
#include <limits>
#include <cmath>
using namespace fluxcrawl;
void check(bool value,const char* label){if(!value){throw std::runtime_error(label);}}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,"expected rejection");}
Url url(std::size_t n){return Url::parse("https://host.test/page/"+std::to_string(n)).value();}
void common(SeenSet& seen){
    const auto count=seen.size();auto a=url(17);check(!seen.contains(a),"new absent");check(seen.insert_if_absent(a),"first accepted");check(!seen.insert_if_absent(a),"duplicate before commit rejected");
    check(!seen.insert_if_absent(Url::parse("HTTPS://HOST.TEST:443/page/17#fragment").value()),"canonical equivalent duplicate");
    auto query=Url::parse("https://host.test/page/17?q=1").value();check(seen.insert_if_absent(query),"query identity distinct");check(seen.size()==count+2,"logical size");seen.rollback(query);check(!seen.contains(query),"rollback does not lose future acceptance");
    seen.rollback(a);check(seen.size()==count,"rollback size");
}
int main(int argc,char** argv){
    if(argc!=2){return 2;}
    const auto directory=std::filesystem::path{argv[1]}/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());std::filesystem::create_directories(directory);
    try{
        for(double p:{0.0,1.0,-0.1,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}){rejects([&]{BloomFilter filter{100,p};});}
        rejects([]{BloomFilter f{0,.01};});rejects([]{BloomFilter f{std::numeric_limits<std::size_t>::max(),.001};});
        BloomFilter bloom{10000,.01};check(bloom.bit_count()==95851&&bloom.probes()==7,"mathematical sizing");check(!bloom.maybe_contains(url(0).str()),"empty definitely absent");
        for(std::size_t i=0;i<10000;++i){bloom.insert(url(i).str());}
        for(std::size_t i=0;i<10000;++i){check(bloom.maybe_contains(url(i).str()),"no false negatives");}
        std::size_t fp=0;for(std::size_t i=10000;i<110000;++i){fp+=bloom.maybe_contains(url(i).str());}
        check(fp>200&&fp<4000,"broad deterministic false-positive sanity bound");check(bloom.insertions()==10000&&bloom.set_bits()>0,"insertion and occupancy diagnostics");
        check(BloomFilter::hash("stable",123)==4261848254438514678ULL&&BloomFilter::hash("stable",123)!=BloomFilter::hash("other",123),"stable byte hashing");
        auto memory=memory_seen();common(*memory);check(memory->export_urls().empty(),"memory exact export");
        for(auto mode:{DedupMode::Sqlite,DedupMode::BloomSqlite}){
            auto seed=url(0);CrawlCheckpoint initial{.seed=seed};initial.options.dedup.mode=mode;initial.options.dedup.expected_elements=1;initial.options.dedup.false_positive_rate=.5;
            initial.seen={seed};initial.unfinished={{{seed,0},{},false}};initial.counters.urls_discovered=initial.counters.urls_pending=1;
            const auto path=directory/(std::string{dedup_name(mode)}+".db");auto store=std::shared_ptr<SqliteStore>{SqliteStore::create(path,initial)};
            auto seen=sqlite_seen(store,initial.options.dedup);common(*seen);auto a=url(1);check(seen->insert_if_absent(a)&&!seen->insert_if_absent(a),"uncommitted exact membership");
            auto metadata=CheckpointManager::metadata_only(initial);metadata.sequence=1;metadata.counters.urls_discovered=metadata.counters.urls_pending=2;
            CrawlMutationBatch batch{metadata,{a},{{a,CheckpointWork{{a,1},{},false},1}},2,0};seen->captured(1,batch.discoveries);
            rejects([&]{store->apply_with_hook(batch,[](std::size_t){throw std::runtime_error("injected");});});
            check(store->committed_sequence()==0&&!store->seen_contains(a)&&seen->contains(a),"failed SQL transaction retains logical cache without durable seen-only row");
            store->apply(batch);auto duplicate=batch;++duplicate.metadata.sequence;++duplicate.metadata.counters.urls_discovered;++duplicate.metadata.counters.urls_pending;rejects([&]{store->apply(duplicate);});check(store->metrics().seen_constraint_conflicts==1,"unique constraint diagnostics");check(seen->contains(a)&&seen->metrics().pending_cache_size==0,"committed cache evicted by sequence");
            auto bulk=metadata;++bulk.sequence;bulk.counters.urls_discovered=bulk.counters.urls_pending=100;
            CrawlMutationBatch population{bulk,{},{},196,0};
            for(std::size_t i=200;i<298;++i){auto u=url(i);check(seen->insert_if_absent(u),"bulk exact acceptance");population.discoveries.push_back(u);population.changes.push_back({u,CheckpointWork{{u,1},{},false},i});}
            seen->captured(bulk.sequence,population.discoveries);store->apply(population);
            if(mode==DedupMode::BloomSqlite){
                bool found=false;
                for(std::size_t i=100;i<1000&&!found;++i){const auto before=seen->metrics().bloom_false_positives;check(seen->insert_if_absent(url(i)),"unseen always accepted despite Bloom");found=seen->metrics().bloom_false_positives>before;}
                check(found,"deterministic tiny-filter false positive accepted by exact authority");
            }
            seen.reset();store.reset();store=std::shared_ptr<SqliteStore>{SqliteStore::open(path)};auto restored=store->load();
            check(restored.seen.empty()&&restored.external_seen_count==100&&restored.unfinished.size()==100,"SQLite resume does not materialize durable seen set");
            rejects([&]{(void)CheckpointManager::serialize(restored);});
            seen=sqlite_seen(store,restored.options.dedup);check(seen->size()==100&&seen->contains(seed)&&seen->contains(a),"reopened exact set and rebuilt Bloom preserve all committed entries");
            for(std::size_t i=200;i<298;++i){check(seen->contains(url(i)),"every rebuilt stored URL has no false negative");}
            check(seen->metrics().pending_cache_peak==0,"rebuild uses no whole-state uncommitted cache");
        }
        std::filesystem::remove_all(directory);std::cout<<"Bloom FP sample "<<fp<<"/100000; exact backends, atomicity, cache and rebuild passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
