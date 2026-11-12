#include <fluxcrawl/checkpoint.hpp>
#include <fluxcrawl/sqlite_store.hpp>
#include "fault_plan.hpp"
#include "sqlite_fault_vfs.hpp"
#include <fstream>
#include <iostream>
using namespace fluxcrawl;
void check(bool ok,const char* label) { if(!ok) {throw std::runtime_error(label);} }
CrawlCheckpoint initial() {
    const auto seed=*Url::parse("http://example.test/"); CrawlCheckpoint c{.seed=seed};
    c.options.backpressure.enabled=false; c.seen={seed}; c.unfinished={{{seed,0},{},false}};
    c.counters.urls_discovered=c.counters.urls_pending=1; return c;
}
int main(int argc,char** argv) try {
    if(argc!=2) {return 2;}
    const auto directory=std::filesystem::path{argv[1]}; std::filesystem::create_directories(directory);
    auto c=initial(); const auto path=directory/"primary.chk"; CheckpointManager::write(path,c);
    certification::FaultPlan fault;
    // Existing replacement hook preserves the primary and removes its temp file.
    fault.arm(); bool failed=false;
    try { CheckpointManager::write(path,c,[&]{fault.hit();}); } catch(const std::exception&) {failed=true;}
    check(failed && CheckpointManager::read(path).seen==c.seen,"failed replace lost primary");
    // A deterministic write failure after opening the temp file also preserves it.
    fault.arm(); failed=false;
    try { CheckpointManager::write(path,c,{},[&]{fault.hit();}); } catch(const std::exception&) {failed=true;}
    check(failed && CheckpointManager::read(path).seen==c.seen,"failed write lost primary");
    for(const auto& entry:std::filesystem::directory_iterator(directory)) {check(entry.path().extension()==".chk","checkpoint temp leaked");}
    for(int code:{SQLITE_FULL,SQLITE_IOERR_WRITE}) {
        certification::FaultVfs vfs;
        const auto db=directory/(std::to_string(code)+".db"); auto store=SqliteStore::create(db,c);
        const auto url=*Url::parse("http://example.test/next"); auto metadata=CheckpointManager::metadata_only(c);
        ++metadata.sequence; ++metadata.counters.urls_discovered; ++metadata.counters.urls_pending;
        CrawlMutationBatch batch{metadata,{url},{{url,CheckpointWork{{url,1},{},false},1}},2,0};
        vfs.arm(code); failed=false;
        try {store->apply(batch);} catch(const std::exception& error) {
            failed=true; std::cout<<"injected "<<code<<": "<<error.what()<<'\n';
            check(std::string{error.what()}.find("SQLite")!=std::string::npos,"SQLite error not surfaced");
        }
        check(failed,"SQLite write fault did not fire"); store.reset();
        store=SqliteStore::open(db); const auto loaded=store->load();
        check(loaded.seen==c.seen && loaded.unfinished.size()==1 && loaded.sequence==0,"partial SQLite commit restored");
        store->apply(batch); store.reset(); store=SqliteStore::open(db);
        check(store->load().seen.size()==2,"recovery after write fault failed");
    }
    std::filesystem::remove_all(directory);
    std::cout<<"Checkpoint failure and SQLite FULL/I/O atomic-prefix certification passed\n";
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
