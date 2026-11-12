#include <fluxcrawl/sequential_crawler.hpp>
#include <fluxcrawl/thread_pool_crawler.hpp>
#include <fluxcrawl/async_crawler.hpp>
#include <fluxcrawl/observability.hpp>
#ifdef FLUXCRAWL_OBSERVABILITY
#include <fluxcrawl/observability_server.hpp>
#endif
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#if defined(__unix__) || defined(__APPLE__)
#include <dirent.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include <fstream>
using namespace fluxcrawl;
void check(bool value,const char* message) { if (!value) { throw std::runtime_error(message); } }
std::size_t descriptors() {
#if defined(__unix__) || defined(__APPLE__)
#ifdef __APPLE__
    const char* path="/dev/fd";
#else
    const char* path="/proc/self/fd";
#endif
    auto* directory=opendir(path); if (!directory) { return 0; }
    std::size_t count=0; while (auto* entry=readdir(directory)) { if (entry->d_name[0]!='.') { ++count; } }
    closedir(directory); return count ? count-1 : 0;
#else
    return 0;
#endif
}
std::size_t rss() {
#ifdef __APPLE__
    mach_task_basic_info_data_t info{}; mach_msg_type_number_t count=MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(),MACH_TASK_BASIC_INFO,reinterpret_cast<task_info_t>(&info),&count)==KERN_SUCCESS) { return info.resident_size; }
#elif defined(__linux__)
    std::ifstream input{"/proc/self/statm"}; std::size_t total=0,resident=0;
    if (input>>total>>resident) { return resident*static_cast<std::size_t>(sysconf(_SC_PAGESIZE)); }
#endif
    return 0;
}
int main(int argc,char** argv) try {
    if (argc<4) { std::cerr<<"seed iterations duration_seconds [workers]\n"; return 2; }
    const auto seed=Url::parse(argv[1]).value(); const auto iterations=std::stoull(argv[2]); const auto seconds=std::stoull(argv[3]);
    const auto parallelism=argc>4?std::stoull(argv[4]):16;
    const auto began=std::chrono::steady_clock::now();
    std::size_t sessions=0,requests=0,fd_baseline=0;
    std::set<std::string> reference;
    while (seconds ? std::chrono::steady_clock::now()-began<std::chrono::seconds{seconds} : sessions<iterations) {
        const auto engine=sessions%3;
        const auto session_started=std::chrono::steady_clock::now();
        {
            CrawlOptions options; options.max_depth=3; options.max_pages=10000;
            options.backpressure={5,2,true}; options.respect_robots=true;
            options.retry.max_retries=1; options.retry.jitter=false;
            options.retry.initial_backoff=std::chrono::milliseconds{10};
            options.scheduling.max_per_host=1;
            if (sessions%7==0) { options.scheduling.rate=RateLimit{100,2}; }
            options.observability=std::make_shared<Observability>(std::chrono::milliseconds{100},true);
#ifdef FLUXCRAWL_OBSERVABILITY
            ObservabilityServer server{options.observability,0};
#endif
            HttpClientOptions http; http.use_environment_proxy=false;
            http.request_timeout=std::chrono::seconds{3};
            const auto result=engine==0?SequentialCrawler{http}.crawl(seed,options):engine==1?
                ThreadPoolCrawler{parallelism,http}.crawl(seed,options):AsyncCrawler{parallelism*2,http}.crawl(seed,options);
            check(result.urls_pending==0 && result.pressure.deferred_urls==0,"premature termination");
            check(result.pages_attempted<=options.max_pages,"attempt budget exceeded");
            check(result.pressure.peak_unfinished<=5,"pending bound exceeded");
            check(result.robots_origins==3 && result.robots_fetches>=3 && result.robots_fetches<=6,"logical robots bootstrap count");
            if (result.fetch_failures || result.robots_fetch_failures) {
                std::cerr<<"session "<<sessions<<" engine "<<engine<<" failures "<<result.fetch_failures<<" terminal "<<result.terminal_fetch_failures<<" robots "<<result.robots_fetch_failures<<'\n';
                for (const auto* records : {&result.records,&result.robots_records}) {
                    for(const auto& record:*records) { if(const auto* error=std::get_if<HttpError>(&record.outcome)) { std::cerr<<"curl="<<error->curl_code<<" "<<error->message<<'\n'; } }
                }
            }
            check(result.terminal_fetch_failures==0 && result.terminal_http_failures==0 && result.robots_fetch_failures==0,"unexpected terminal failure");
            std::set<std::string> logical;
            for (const auto& record:result.records) { logical.insert(record.item.url.canonical()); }
            if (!sessions) { reference=logical; }
            check(logical==reference,"engine accepted/fetched set mismatch");
            check(result.urls_discovered==logical.size()+result.urls_blocked_by_robots,"duplicate acceptance");
            const auto sample=options.observability->latest();
            check(sample->state=="completed" && sample->active==0,"final telemetry mismatch");
            check(sample->counters.urls_discovered==result.urls_discovered,"telemetry changed counters");
            check(sample->max_active<=(engine==0?1:engine==1?parallelism:parallelism*2),"global cap exceeded");
            for (const auto& host:sample->hosts.hosts) { check(host.active<=1,"host cap exceeded"); }
            requests+=result.pages_attempted;
        } // All engine, transport, sampler, server and snapshot owners destroyed.
        // A lifecycle soak measures retained resources, not loopback ephemeral-port
        // exhaustion from recreating connection pools hundreds of times a second.
        if (seconds) { std::this_thread::sleep_until(session_started+std::chrono::milliseconds{200}); }
        ++sessions;
        if (sessions==3) { fd_baseline=descriptors(); }
        if (sessions>=3) { check(descriptors()<=fd_baseline,"descriptor growth across destroyed sessions"); }
        if (sessions%10==0 || (!seconds && sessions==iterations)) {
            const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count();
            std::cout<<"sample\t"<<elapsed<<'\t'<<sessions<<'\t'<<rss()<<'\t'<<descriptors()<<'\t'<<reference.size()<<'\t'<<requests<<'\t'<<static_cast<double>(requests)/elapsed<<std::endl;
        }
    }
    std::cout<<"completed\t"<<sessions<<'\t'<<requests<<'\t'<<rss()<<'\t'<<descriptors()<<std::endl;
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
