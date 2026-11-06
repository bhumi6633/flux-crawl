#include <fluxcrawl/discovery_spool.hpp>
#include <array>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <fcntl.h>
#include <sqlite3.h>
#include <unistd.h>
namespace fluxcrawl {
namespace {
struct IoTimer {
    std::chrono::nanoseconds& total;
    std::chrono::steady_clock::time_point began=std::chrono::steady_clock::now();
    ~IoTimer() { total+=std::chrono::steady_clock::now()-began; }
};
[[noreturn]] void failed() { throw std::runtime_error("Discovery spool I/O or corrupt record"); }
void transfer(int fd, void* data, std::size_t size, std::uint64_t offset, bool writing) {
    auto* bytes = static_cast<char*>(data);
    while (size) {
        auto n = writing ? ::pwrite(fd, bytes, size, static_cast<off_t>(offset)) : ::pread(fd, bytes, size, static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) { continue; }
        if (n <= 0) { failed(); }
        bytes += n; size -= static_cast<std::size_t>(n); offset += static_cast<std::uint64_t>(n);
    }
}
std::uint64_t append_record(int fd, std::uint64_t end, const Url& url, std::uint64_t order) {
    if (url.str().size() > 16 * 1024 * 1024) { throw std::runtime_error("Discovery URL exceeds 16 MiB spool record limit"); }
    std::array<unsigned char,4> header{}; const auto n = url.str().size();
    if (end > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - n - 12) { failed(); }
    for (unsigned i=0;i<4;++i) { header[i] = static_cast<unsigned char>(n >> (8*i)); }
    std::array<unsigned char,8> sequence{};
    for(unsigned i=0;i<8;++i) { sequence[i]=static_cast<unsigned char>(order>>(8*i)); }
    transfer(fd,sequence.data(),8,end,true); transfer(fd,header.data(),4,end+8,true);
    transfer(fd,const_cast<char*>(url.str().data()),n,end+12,true); return end+12+n;
}
Url read_record(int fd, std::uint64_t& begin, std::uint64_t end) {
    if (begin > end || end - begin < 12) { failed(); }
    std::array<unsigned char,4> header{}; transfer(fd,header.data(),4,begin+8,false);
    std::size_t n=0; for(unsigned i=0;i<4;++i) { n |= std::size_t{header[i]} << (8*i); }
    if (n == 0 || n > 16*1024*1024 || n > end-begin-12) { failed(); }
    std::string value(n,'\0'); transfer(fd,value.data(),n,begin+12,false); begin+=12+n;
    auto url=Url::parse(value); if (!url || url->str()!=value) { failed(); } return *url;
}
struct Descriptor {
    int fd;
    explicit Descriptor(const std::filesystem::path& path, bool create = true) : fd{::open(path.c_str(),create ? O_RDWR|O_CREAT|O_CLOEXEC : O_RDONLY|O_CLOEXEC,0600)} { if(fd<0) { failed(); } }
    ~Descriptor(){::close(fd);}
};
std::filesystem::path file(const std::string& directory, std::uint64_t id) { return std::filesystem::path{directory}/(std::to_string(id)+".links"); }
std::string origin(const Url& url) { return url.scheme()+"://"+url.host()+(url.port() ? ":"+std::to_string(*url.port()) : "")+"/"; }
}
std::size_t DeferredState::count() const {
    std::size_t n=0; for(const auto& e:extents) { if(e.count>std::numeric_limits<std::size_t>::max()-n) { failed(); } n+=static_cast<std::size_t>(e.count); } return n;
}
struct DiscoverySpool::Impl {
    mutable std::mutex mutex;
    std::chrono::nanoseconds elapsed{};
    std::string directory;
    bool persistent;
    struct Entry { std::unique_ptr<Descriptor> handle; DeferredExtent extent; bool dirty=false; std::optional<std::uint64_t> head_order{}; };
    std::map<std::pair<std::size_t,std::string>,Entry> files;
    std::uint64_t next_id=0, next_order=0;
    std::size_t count=0;
    bool directory_dirty=true;
    std::string last_origin;
    std::size_t last_depth=0;
    std::vector<Entry*> open;
    int descriptor(Entry& entry) {
        if (!entry.handle) {
            if (open.size() == 16) { for(auto* old:open) { old->handle.reset(); } open.clear(); }
            entry.handle=std::make_unique<Descriptor>(file(directory,entry.extent.file_id)); open.push_back(&entry);
        }
        return entry.handle->fd;
    }
};
DiscoverySpool::DiscoverySpool(const std::filesystem::path& prefix, bool persistent) : impl_{std::make_unique<Impl>()} {
    impl_->persistent=persistent;
    auto name=(prefix.empty() ? std::filesystem::temp_directory_path()/"fluxcrawl-discovery" : std::filesystem::absolute(prefix)).string()+".discovery-XXXXXX";
    if(!::mkdtemp(name.data())) { failed(); } impl_->directory=std::move(name);
}
DiscoverySpool::~DiscoverySpool(){ if(!impl_->persistent) { std::error_code error; std::filesystem::remove_all(impl_->directory,error); } }
void DiscoverySpool::append(const CrawlItem& item) {
    std::lock_guard lock{impl_->mutex}; IoTimer timer{impl_->elapsed};
    auto [it,inserted]=impl_->files.try_emplace(std::pair{item.depth,origin(item.url)});
    auto& entry=it->second;
    if(inserted) { const auto id=++impl_->next_id; entry.extent={item.depth,0,0,0,id,origin(item.url)}; impl_->directory_dirty=true; }
    if(impl_->next_order==std::numeric_limits<std::uint64_t>::max()) { failed(); }
    entry.extent.end=append_record(impl_->descriptor(entry),entry.extent.end,item.url,++impl_->next_order); ++entry.extent.count; ++impl_->count; entry.dirty=true;
}
std::optional<CrawlItem> DiscoverySpool::pop(bool fair) {
    std::lock_guard lock{impl_->mutex}; IoTimer timer{impl_->elapsed};
    auto first=impl_->files.end(), chosen=impl_->files.end();
    for(auto it=impl_->files.begin();it!=impl_->files.end();++it) {
        if(!it->second.extent.count) { continue; }
        if(first==impl_->files.end()) { first=it; }
        if(it->first.first!=first->first.first) { break; }
        if(it->first.first!=impl_->last_depth || it->first.second>impl_->last_origin) { chosen=it; break; }
    }
    if(chosen==impl_->files.end()) { chosen=first; }
    if(!fair && first!=impl_->files.end()) {
        std::uint64_t minimum=std::numeric_limits<std::uint64_t>::max();
        for(auto it=first;it!=impl_->files.end() && it->first.first==first->first.first;++it) {
            if(!it->second.extent.count) { continue; }
            auto& entry=it->second;
            if(!entry.head_order) {
                std::array<unsigned char,8> bytes{}; transfer(impl_->descriptor(entry),bytes.data(),8,entry.extent.begin,false);
                std::uint64_t order=0; for(unsigned i=0;i<8;++i) { order|=std::uint64_t{bytes[i]}<<(8*i); }
                entry.head_order=order;
            }
            if(*entry.head_order<minimum) { minimum=*entry.head_order; chosen=it; }
        }
    }
    if(chosen!=impl_->files.end()) {
        auto& entry=chosen->second; const auto depth=entry.extent.depth;
        auto url=read_record(impl_->descriptor(entry),entry.extent.begin,entry.extent.end); --entry.extent.count; --impl_->count; entry.head_order.reset();
        impl_->last_depth=depth; impl_->last_origin=chosen->first.second;
        return CrawlItem{std::move(url),depth};
    }
    return {};
}
std::size_t DiscoverySpool::size() const { std::lock_guard lock{impl_->mutex}; return impl_->count; }
DeferredState DiscoverySpool::capture() const {
    std::lock_guard lock{impl_->mutex}; DeferredState state{impl_->directory,{},impl_->next_order};
    for(const auto& [depth,entry]:impl_->files) { (void)depth; if(entry.extent.count) { state.extents.push_back(entry.extent); } } return state;
}
void DiscoverySpool::sync() const {
    std::lock_guard lock{impl_->mutex}; IoTimer timer{impl_->elapsed};
    for(auto& [depth,entry]:impl_->files) { (void)depth; if(entry.dirty) { if(::fsync(impl_->descriptor(entry))) { failed(); } entry.dirty=false; } }
    if (impl_->directory_dirty) {
        Descriptor directory{impl_->directory,false}; if(::fsync(directory.fd)) { failed(); }
        Descriptor parent{std::filesystem::path{impl_->directory}.parent_path(),false}; if(::fsync(parent.fd)) { failed(); }
        impl_->directory_dirty=false;
    }
}
void DiscoverySpool::validate(const DeferredState& state, std::size_t max_depth, std::optional<std::string_view> required_host) {
    if(state.extents.empty()) { return; }
    if(state.directory.empty() || !std::filesystem::path{state.directory}.is_absolute()) { failed(); }
    std::pair<std::size_t,std::string> previous; bool first=true; std::set<std::uint64_t> ids;
    for(const auto& e:state.extents) {
        if(e.depth==0 || e.depth>max_depth || (!first && std::pair{e.depth,e.origin}<=previous) || !e.file_id || !ids.insert(e.file_id).second || !Url::parse(e.origin) || origin(*Url::parse(e.origin))!=e.origin || !e.count || e.begin>=e.end || e.end>std::uint64_t{std::numeric_limits<off_t>::max()}) { failed(); }
        Descriptor fd{file(state.directory,e.file_id),false};
        if(e.end>std::filesystem::file_size(file(state.directory,e.file_id))) { failed(); }
        auto cursor=e.begin; std::uint64_t last_order=0;
        for(std::uint64_t n=0;n<e.count;++n) {
            std::array<unsigned char,8> bytes{}; transfer(fd.fd,bytes.data(),8,cursor,false);
            std::uint64_t order=0; for(unsigned i=0;i<8;++i) { order|=std::uint64_t{bytes[i]}<<(8*i); }
            auto url=read_record(fd.fd,cursor,e.end);
            if(!order || order<=last_order || order>state.next_order || origin(url)!=e.origin || (required_host && url.host()!=*required_host)) { failed(); }
            last_order=order;
        }
        if(cursor!=e.end) { failed(); } first=false; previous={e.depth,e.origin};
    }
}
void DiscoverySpool::restore(const DeferredState& state) {
    // Fork immutable prefixes so two snapshot resumes never mutate each other's
    // companion files. Ignore the uncommitted suffix after the captured end.
    std::lock_guard lock{impl_->mutex};
    impl_->next_order=state.next_order;
    for(const auto& e:state.extents) {
        std::filesystem::copy_file(file(state.directory,e.file_id),file(impl_->directory,e.file_id));
        auto handle=std::make_unique<Descriptor>(file(impl_->directory,e.file_id));
        if(::ftruncate(handle->fd,static_cast<off_t>(e.end))) { failed(); }
        impl_->next_id=std::max(impl_->next_id,e.file_id); impl_->count+=static_cast<std::size_t>(e.count);
        impl_->files.emplace(std::pair{e.depth,e.origin},Impl::Entry{{},e,true});
    }
}
std::chrono::nanoseconds DiscoverySpool::io_time() const { std::lock_guard lock{impl_->mutex}; return impl_->elapsed; }
struct DiskLinks::Impl {
    std::chrono::nanoseconds elapsed{};
    struct Close { void operator()(sqlite3* connection) const { if(connection) { sqlite3_close(connection); } } };
    struct Finalize { void operator()(sqlite3_stmt* stmt) const { if(stmt) { sqlite3_finalize(stmt); } } };
    std::unique_ptr<sqlite3,Close> db;
    std::unique_ptr<sqlite3_stmt,Finalize> insert, read;
    void execute(const char* sql) {
        if(sqlite3_exec(db.get(),sql,nullptr,nullptr,nullptr)!=SQLITE_OK) { throw std::runtime_error("Temporary link spool: "+std::string{sqlite3_errmsg(db.get())}); }
    }
    std::unique_ptr<sqlite3_stmt,Finalize> prepare(const char* sql) {
        sqlite3_stmt* stmt=nullptr; const auto code=sqlite3_prepare_v2(db.get(),sql,-1,&stmt,nullptr);
        std::unique_ptr<sqlite3_stmt,Finalize> owned{stmt};
        if(code!=SQLITE_OK) { throw std::runtime_error("Cannot prepare temporary link spool statement"); } return owned;
    }
};
DiskLinks::DiskLinks():impl_{std::make_unique<Impl>()} {
    IoTimer timer{impl_->elapsed};
    sqlite3* db=nullptr; const auto code=sqlite3_open("",&db); impl_->db.reset(db);
    if(code!=SQLITE_OK) { throw std::runtime_error("Cannot create temporary link spool database"); }
    // This per-response spool is disposable until the parent is published. Its
    // 256 KiB cache and on-disk uniqueness index preserve extractor semantics
    // without a document-sized hash set. It is never the global seen index.
    impl_->execute("PRAGMA cache_size=-256; PRAGMA temp_store=FILE; PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF; CREATE TABLE links(url TEXT NOT NULL UNIQUE); BEGIN");
    impl_->insert=impl_->prepare("INSERT OR IGNORE INTO links(url) VALUES(?1)");
}
DiskLinks::~DiskLinks()=default;
std::chrono::nanoseconds DiskLinks::io_time() const { return impl_->elapsed; }
void DiskLinks::append(const Url& url) {
    IoTimer timer{impl_->elapsed};
    auto* statement=impl_->insert.get();
    if(sqlite3_reset(statement)!=SQLITE_OK || sqlite3_clear_bindings(statement)!=SQLITE_OK
        || sqlite3_bind_text64(statement,1,url.str().data(),url.str().size(),SQLITE_TRANSIENT,SQLITE_UTF8)!=SQLITE_OK
        || sqlite3_step(statement)!=SQLITE_DONE) { throw std::runtime_error("Cannot append temporary link spool"); }
}
void DiskLinks::rewind() {
    IoTimer timer{impl_->elapsed};
    if(!impl_->read) { impl_->execute("COMMIT"); impl_->read=impl_->prepare("SELECT url FROM links ORDER BY rowid"); }
    else if(sqlite3_reset(impl_->read.get())!=SQLITE_OK) { throw std::runtime_error("Cannot rewind temporary link spool"); }
}
std::optional<Url> DiskLinks::next() {
    IoTimer timer{impl_->elapsed};
    const auto code=sqlite3_step(impl_->read.get()); if(code==SQLITE_DONE) { return {}; }
    if(code!=SQLITE_ROW) { throw std::runtime_error("Cannot read temporary link spool"); }
    const auto* data=reinterpret_cast<const char*>(sqlite3_column_text(impl_->read.get(),0));
    const auto bytes=sqlite3_column_bytes(impl_->read.get(),0);
    auto url=Url::parse(std::string_view{data,static_cast<std::size_t>(bytes)}); if(!url) { failed(); } return url;
}
}
