#include <fluxcrawl/sequential_crawler.hpp>
#include <fluxcrawl/thread_pool_crawler.hpp>
#include <fluxcrawl/async_crawler.hpp>
#include <fluxcrawl/observability.hpp>
#include <fluxcrawl/sqlite_store.hpp>
#include "sqlite_fault_vfs.hpp"
#include <iostream>
using namespace fluxcrawl;
void check(bool ok,const char* label){if(!ok){throw std::runtime_error(label);}}
int main(int argc,char** argv) try {
    if(argc!=3){return 2;}
    const auto seed=*Url::parse(argv[1]); const auto directory=std::filesystem::path{argv[2]};
    std::filesystem::create_directories(directory);
    for(int engine=0;engine<3;++engine) for(int error:{SQLITE_FULL,SQLITE_IOERR_WRITE}) {
        const auto path=directory/(std::to_string(engine)+"-"+std::to_string(error)+".db");
        {
            certification::FaultVfs vfs;vfs.arm_first_commit(error);
            CrawlOptions options;options.max_depth=2;options.max_pages=100;options.backpressure={5,2,true};
            options.dedup.mode=DedupMode::BloomSqlite;options.dedup.expected_elements=2;options.dedup.false_positive_rate=.2;
            options.sqlite.path=path;options.sqlite.batch_size=1;
            options.observability=std::make_shared<Observability>(std::chrono::milliseconds{100});
            HttpClientOptions http;http.use_environment_proxy=false;
            const auto result=engine==0?SequentialCrawler{http}.crawl(seed,options):engine==1?ThreadPoolCrawler{4,http}.crawl(seed,options):AsyncCrawler{4,http}.crawl(seed,options);
            check(!result.persistence_errors.empty(),"engine hid durable write failure");
            check(result.pages_attempted<=4,"engine continued claiming after persistence failure");
            check(options.observability->latest()->state=="error","telemetry hid persistence failure");
        }
        std::shared_ptr<CrawlCheckpoint> saved;
        {auto store=SqliteStore::open(path);saved=std::make_shared<CrawlCheckpoint>(store->load());}
        check(saved->sequence==0 && saved->external_seen_count==1 && saved->unfinished.size()==1,"failed engine commit exposed half a prefix");
        auto restored=saved->options;restored.resume_from=saved;restored.sqlite.path=path;restored.sqlite.resume=true;restored.sqlite.batch_size=4;
        const auto recovered=AsyncCrawler{4,saved->http}.crawl(saved->seed,restored);
        check(recovered.persistence_errors.empty() && recovered.urls_pending==0 && recovered.pressure.deferred_urls==0,"resume after engine write failure failed");
        check(recovered.urls_discovered==16 && recovered.resources_completed==16,"recovery lost logical work");
        {auto store=SqliteStore::open(path);const auto final=store->load();check(final.completed && final.unfinished.empty() && store->seen_count()==16,"recovery resurrected completed work");}
        std::cout<<"engine "<<engine<<" SQLite error "<<error<<": failed prefix -> async recovery passed\n";
    }
    std::filesystem::remove_all(directory);
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
