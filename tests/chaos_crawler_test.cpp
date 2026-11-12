#include <fluxcrawl/sequential_crawler.hpp>
#include <fluxcrawl/thread_pool_crawler.hpp>
#include <fluxcrawl/async_crawler.hpp>
#include <fluxcrawl/observability.hpp>
#include <fluxcrawl/retry_policy.hpp>
#include <set>
#include <ctime>
#include <iostream>
#include <stdexcept>
using namespace fluxcrawl;
void check(bool ok,const char* label) {if(!ok){throw std::runtime_error(label);}}
int main(int argc,char** argv) try {
    if(argc!=2){return 2;}
    for(int engine=0;engine<3;++engine){
        const auto seed=*Url::parse(std::string{argv[1]}+"/"+std::to_string(engine)+"/root");
        CrawlOptions options;options.max_depth=3;options.max_pages=100;options.backpressure={5,2,true};
        options.retry.max_retries=1;options.retry.jitter=false;options.retry.initial_backoff=std::chrono::milliseconds{10};
        options.scheduling.max_per_host=2;
        options.observability=std::make_shared<Observability>(std::chrono::milliseconds{100});
        HttpClientOptions http;http.use_environment_proxy=false;http.max_body_bytes=4096;http.max_redirects=3;
        http.request_timeout=std::chrono::milliseconds{80};http.connect_timeout=std::chrono::milliseconds{500};
        const auto result=engine==0?SequentialCrawler{http}.crawl(seed,options):engine==1?ThreadPoolCrawler{16,http}.crawl(seed,options):AsyncCrawler{32,http}.crawl(seed,options);
        check(result.urls_pending==0 && result.pressure.deferred_urls==0,"chaos premature termination");
        check(result.pages_attempted<=100,"chaos budget exceeded");
        check(result.resources_completed==result.urls_discovered,"chaos logical work lost");
        check(result.retries_exhausted==1,"500 exhaustion mismatch");
        check(result.terminal_fetch_failures==2,"loop and body-limit terminal failures");
        check(result.responses_recovered_after_retry==5,"partial/close/timeout/429/503 recovery mismatch");
        check(result.pressure.peak_unfinished<=5,"chaos pending bound");
        std::size_t oversized=0,loop=0;
        for(const auto& record:result.records){
            if(record.item.url.path().ends_with("/oversized")){
                ++oversized;const auto* error=std::get_if<HttpError>(&record.outcome);
                check(error && error->code==HttpErrorCode::BodyTooLarge && !retryable_error(*error),"oversized classification");
            }
            if(record.item.url.path().ends_with("/loop-a")){++loop;check(std::holds_alternative<HttpError>(record.outcome),"redirect loop did not fail");}
        }
        check(oversized==1 && loop==1,"non-retryable requests retried");
        const auto snapshot=options.observability->latest();check(snapshot->active==0,"active slot leaked");
        std::cout<<engine<<": seen="<<result.urls_discovered<<" attempts="<<result.pages_attempted<<" recovered="<<result.responses_recovered_after_retry<<'\n';
    }
    // Repeated delayed-only states: no active or ready request while the timer
    // owns the remaining logical resource. CPU checks catch an accidental spin.
    CrawlOptions delayed; delayed.max_pages=100; delayed.retry.max_retries=20;
    delayed.retry.jitter=false; delayed.retry.initial_backoff=delayed.retry.max_backoff=std::chrono::milliseconds{25};
    const AsyncCrawler idle_crawler{1};
    const auto cpu_started=std::clock(); const auto started=std::chrono::steady_clock::now();
    const auto result=idle_crawler.crawl(*Url::parse(std::string{argv[1]}+"/idle/always500"),delayed);
    const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    const auto cpu=static_cast<double>(std::clock()-cpu_started)/CLOCKS_PER_SEC;
    check(result.pages_attempted==21 && result.retries_exhausted==1 && result.resources_completed==1,"delayed-only work lost");
    check(elapsed>=.45 && cpu<elapsed*.5+.02,"delayed-only loop busy-spins or wakes early");
    std::cout<<"delayed-only: 20 wakeups, elapsed="<<elapsed<<" cpu="<<cpu<<'\n';
} catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
