#include <fluxcrawl/html_parser.hpp>
#include <fluxcrawl/host_scheduler.hpp>
#include <fluxcrawl/checkpoint.hpp>
#include <iostream>
#include <stdexcept>
using namespace fluxcrawl;
void check(bool ok,const char* label){if(!ok){throw std::runtime_error(label);}}
int main() try{
    const auto seed=*Url::parse("http://example.test/");
    std::string html;
    for(int i=0;i<10000;++i){html+="<a href='/same#"+std::to_string(i)+"'>x</a>";}
    html+="<a href='?a=1&amp;b=2'>e</a><a href=''>empty</a><a href='"+std::string(20000,'x')+"'>long</a>";
    const auto links=HtmlLinkExtractor{}.extract_links(html,seed);
    check(links.size()==4,"adversarial link dedup/entity/empty semantics changed");
    for(const auto& link:links){check(Url::parse(link.canonical())==link,"adversarial URL round trip");}
    HostScheduler scheduler{2};scheduler.enqueue({*Url::parse("http://[::1]:8080/"),0});
    check(scheduler.sample(HostScheduler::Clock::now()).hosts[0].origin=="http://[::1]:8080","IPv6 origin double-bracketed");
    std::cout<<"10k duplicate/fragment anchors, entity, long href, IPv6 origin: passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
