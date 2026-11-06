#include <fluxcrawl/url.hpp>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <unordered_set>
using Clock=std::chrono::steady_clock;
double ms(Clock::duration d){return std::chrono::duration<double,std::milli>(d).count();}
int main(int argc,char** argv){
#ifndef NDEBUG
    std::cerr<<"Use Release\n";return 2;
#endif
    if(argc!=3){return 2;}
    const auto count=std::stoull(argv[1]);const bool reserve=std::string_view{argv[2]}=="reserve";
    std::unordered_set<fluxcrawl::Url> seen;
    auto started=Clock::now();if(reserve){seen.reserve(count);}const auto reserved=Clock::now()-started;
    Clock::duration insertion{};std::size_t rehashes=0;
    for(std::size_t n=0;n<count;++n){
        auto url=fluxcrawl::Url::parse("https://host.test/page/"+std::to_string(n)).value();
        const auto buckets=seen.bucket_count();started=Clock::now();const bool inserted=seen.insert(url).second;insertion+=Clock::now()-started;
        if(!inserted){return 1;}if(seen.bucket_count()!=buckets){++rehashes;}
    }
    Clock::duration lookup{};std::size_t hits=0;
    for(std::size_t n=0;n<100000;++n){
        auto url=fluxcrawl::Url::parse("https://host.test/page/"+std::to_string(n%2==0?n%std::max(count,1ULL):count+n)).value();
        started=Clock::now();const bool present=seen.contains(url);lookup+=Clock::now()-started;hits+=present;
    }
    if(count && hits!=50000){return 1;}
    std::cout<<std::fixed<<std::setprecision(3)<<"count,reserve,reserve_ms,insert_ms,mixed_100k_lookup_ms,buckets,load_factor,growth_rehashes\n"<<count<<','<<reserve<<','<<ms(reserved)<<','<<ms(insertion)<<','<<ms(lookup)<<','<<seen.bucket_count()<<','<<seen.load_factor()<<','<<rehashes<<'\n';
}
