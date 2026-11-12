#include <fluxcrawl/version.hpp>
#include <fluxcrawl/sqlite_store.hpp>
#include <curl/curl.h>
#include <libxml/parser.h>
#include <iostream>
int main() {
    std::cout<<"fluxcrawl\t"<<fluxcrawl::version<<"\ncompiler\t"<<__VERSION__
        <<"\nlibcurl\t"<<curl_version()<<"\nsqlite\t"<<fluxcrawl::SqliteStore::library_version()
        <<"\nlibxml2\t"<<xmlParserVersion<<'\n';
}
