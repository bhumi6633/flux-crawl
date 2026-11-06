#include <fluxcrawl/backpressure.hpp>
#include <fluxcrawl/checkpoint.hpp>
#include <fluxcrawl/crawl_policy.hpp>
#include <fluxcrawl/html_parser.hpp>
#include <iostream>
#include <set>
#include <fstream>
using namespace fluxcrawl;
int main() {
    int failures=0;
    const auto check=[&](bool ok,const char* label){if(!ok){++failures;std::cerr<<label<<'\n';}};
    const auto rejects=[&](auto action){try{action();check(false,"expected rejection");}catch(const std::exception&) {}};
    using Clock=PressureGate::Clock; const auto zero=Clock::time_point{};
    PressureGate gate{{10,5,true}}; gate.update(9,zero);check(gate.admits(9),"below high");gate.update(10,zero);
    for (std::size_t n=9;n>5;--n){gate.update(n,zero+std::chrono::seconds{1});check(gate.active()&&!gate.admits(n),"hysteresis holds");}
    gate.update(5,zero+std::chrono::seconds{2}); QueuePressureMetrics metrics;gate.metrics(metrics,zero+std::chrono::seconds{3});
    check(!gate.active()&&metrics.activations==1&&metrics.resumptions==1&&metrics.pressure_time==std::chrono::seconds{2},"transition counts and wall pressure duration");
    rejects([]{BackpressureOptions{0,0,true}.validate();});rejects([]{BackpressureOptions{10,10,true}.validate();});
    auto seed=Url::parse("http://a.test/").value(), a=Url::parse("http://a.test/a").value(), b=Url::parse("http://b.test/b").value();
    CrawlState state{seed,2,{1,0,true}};state.configure_spool({},false);HostScheduler scheduler{8};CrawlPolicy policy{false};CrawlResult result;
    policy.schedule(state,scheduler,result);state.defer(a,1);policy.schedule(state,scheduler,result);
    check(!state.has_seen(a)&&state.seen_count()==1&&scheduler.pending()==1,"saturation does not mark deferred URL seen");
    auto lease=scheduler.acquire(Clock::now());result.resources_completed=1;lease.reset();policy.schedule(state,scheduler,result);
    check(state.has_seen(a)&&scheduler.pending()==1&&state.pressure_metrics().peak_unfinished==1,"capacity before seen, then scheduler home");
    DiscoverySpool spool;for(int i=0;i<30;++i){spool.append({a,1});}spool.append({b,1});spool.append({a,2});
    DiscoverySpool fifo; fifo.append({b,1});fifo.append({a,1});check(fifo.pop(false)->url==b,"FIFO before pressure across origins");
    auto one=spool.pop(),two=spool.pop();check(one&&two&&one->depth==1&&two->depth==1&&one->url.host()!=two->url.host(),"origin round robin under fanout");
    const auto captured=spool.capture();spool.append({b,2});spool.sync();DiscoverySpool::validate(captured,2);
    DiscoverySpool fork;fork.restore(captured);check(fork.size()==captured.count()&&spool.size()==captured.count()+1,"immutable prefix excludes appended suffix");
    std::size_t n=0;while(fork.pop()){++n;}check(n==captured.count(),"lossless fork replay");
    rejects([&]{DiscoverySpool::validate(captured,2,"only.test");});
    auto bad=captured;bad.extents.front().count++;rejects([&]{DiscoverySpool::validate(bad,2);});
    bad=captured;bad.extents.front().origin="http://wrong.test/";rejects([&]{DiscoverySpool::validate(bad,2);});
    bad=captured;bad.extents.front().end=1;rejects([&]{DiscoverySpool::validate(bad,2);});
    // Bound open file descriptors while many origins contribute deferred work.
    DiscoverySpool origins;for(int i=0;i<100;++i){origins.append({Url::parse("http://h"+std::to_string(i)+".test/a").value(),1});}
    check(origins.size()==100,"many-origin spool");origins.sync();DiscoverySpool::validate(origins.capture(),2);
    HtmlLinkExtractor parser;const std::string html="<html><a href='/a'>a</a><div><a href='/a#f'>dup</a><a href='http://b.test/b'>b</a></div><a href='mailto:a'>skip</a></html>";
    std::set<std::string> streamed,legacy;parser.for_each_link(html,seed,[&](const Url& url){streamed.insert(url.str());});for(const auto& url:parser.extract_links(html,seed)){legacy.insert(url.str());}
    DiskLinks disk; parser.for_each_link(html,seed,[&](const Url& url){disk.append(url);});disk.rewind();std::size_t unique_count=0;while(disk.next()){++unique_count;}check(unique_count==legacy.size(),"disk extraction keeps per-document uniqueness");
    check(streamed==legacy&&streamed.size()==2,"incremental DOM traversal matches canonical target set");
    CrawlCheckpoint c{.seed=seed};c.format_version=1;c.options.backpressure.enabled=false;c.seen={seed};c.unfinished={{{seed,0},{},false}};c.counters.urls_discovered=c.counters.urls_pending=1;
    const auto restored=CheckpointManager::deserialize(CheckpointManager::serialize(c));check(restored.format_version==1&&!restored.options.backpressure.enabled,"legacy v1 checkpoints remain readable unbounded");
    c.format_version=2;c.options.backpressure={10,5,true};c.deferred=spool.capture();const auto roundtrip=CheckpointManager::deserialize(CheckpointManager::serialize(c));
    check(roundtrip.deferred.count()==spool.size()&&roundtrip.options.backpressure.high_watermark==10,"v2 captures deferred prefixes and pressure config");
    std::cout<<failures<<" backpressure failures\n";return failures?1:0;
}
