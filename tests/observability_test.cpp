#include <fluxcrawl/observability.hpp>
#include <fluxcrawl/observability_server.hpp>
#include <fluxcrawl/crawl_state.hpp>
#include <fluxcrawl/crawl_policy.hpp>
#include <boost/json.hpp>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace fluxcrawl;
void check(bool ok,const char* message) { if(!ok) { throw std::runtime_error(message); } }
int main(int argc, char**) {
    if (argc>1) {
        const auto seed=*Url::parse("http://seed.test/");
        CrawlOptions options;
        CrawlState state{seed,2}; HostScheduler scheduler{4}; CrawlPolicy policy{false}; CrawlResult r;
        auto source=std::make_shared<Observability>(std::chrono::milliseconds{100},true);
        source->begin("async",4);
        for (int i=0;i<256;++i) {
            auto url=*Url::parse("http://host"+std::to_string(i)+"."+std::string(60,'a')+"."+std::string(60,'b')+"."+std::string(60,'c')+".test/");
            (void)state.discover(url,1); source->discovery(seed,url);
        }
        policy.schedule(state,scheduler,r); source->capture(state,scheduler,policy,r,options);
        ObservabilityServer server{source,0}; std::cout<<server.port()<<std::endl;
        for (int i=0;i<80;++i) { std::this_thread::sleep_for(std::chrono::milliseconds{100}); source->capture(state,scheduler,policy,r,options); }
        return 0;
    }

    const auto seed=*Url::parse("http://example.test/?private=value");
    CrawlOptions options; options.backpressure={3,1,true};
    CrawlState state{seed,2,options.backpressure}; HostScheduler scheduler{2,{{1},{}}}; CrawlPolicy policy{false}; CrawlResult r;
    auto source=std::make_shared<Observability>(std::chrono::milliseconds{100},true);
    source->begin("threaded",2); policy.schedule(state,scheduler,r);
    auto lease=scheduler.acquire(HostScheduler::Clock::now());
    source->capture(state,scheduler,policy,r,options,true);
    auto a=source->latest(); check(a->active==1,"active claim"); check(a->counters.urls_discovered==1,"seen matches exact set");
    check(a->counters.urls_pending==scheduler.resource_pending(),"pending matches scheduler");
    scheduler.delay({seed,0,1},HostScheduler::Clock::now()+std::chrono::seconds{1});
    source->capture(state,scheduler,policy,r,options,true);
    auto b=source->latest(); check(b->delayed==1 && b->hosts.hosts[0].delayed==1,"delayed host counters");
    check(b->hosts.hosts[0].pending==1 && b->hosts.hosts[0].active==1,"host gauges");
    check(a->delayed==0,"published snapshots immutable");
    for (int i=0;i<6000;++i) { source->request_finished({{seed,0},HttpError{},std::chrono::milliseconds{i%100}}); }
    for (int i=0;i<1000;++i) { source->discovery(seed,*Url::parse("http://h"+std::to_string(i)+".test/")); }
    source->capture(state,scheduler,policy,r,options,true); auto c=source->latest();
    check(c->latency.samples<=4096,"bounded latency"); check(c->errors.size()<=16,"bounded errors");
    check(c->topology_hosts.size()<=64 && c->topology_edges.size()<=128,"bounded topology");
    auto json=boost::json::parse(observability_json(*c)).as_object();
    check(json.at("schema_version").as_int64()==1,"schema version");
    check(!json.at("engine").as_object().contains("active_transfers"),"engine-specific fields");
    check(observability_json(*c).find("private=value")==std::string::npos,"no URL query exposure");
    std::atomic<bool> done{false}; std::thread reader{[&] { while(!done) { check(bool(source->latest()),"concurrent immutable reads"); } }};
    for (int i=0;i<100;++i) { source->capture(state,scheduler,policy,r,options,true); }
    done=true; reader.join();
    std::cout<<"Observability snapshots passed\n";
}
