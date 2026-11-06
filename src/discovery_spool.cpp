#include <fluxcrawl/discovery_spool.hpp>
#include <array>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>
namespace fluxcrawl {
namespace {
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
std::uint64_t append_record(int fd, std::uint64_t end, const Url& url) {
    if (url.str().size() > 16 * 1024 * 1024) { throw std::runtime_error("Discovery URL exceeds 16 MiB spool record limit"); }
    std::array<unsigned char,4> header{}; const auto n = url.str().size();
    for (unsigned i=0;i<4;++i) { header[i] = static_cast<unsigned char>(n >> (8*i)); }
    transfer(fd,header.data(),4,end,true);
    transfer(fd,const_cast<char*>(url.str().data()),n,end+4,true); return end+4+n;
}
Url read_record(int fd, std::uint64_t& begin, std::uint64_t end) {
    if (end - begin < 4) { failed(); }
    std::array<unsigned char,4> header{}; transfer(fd,header.data(),4,begin,false);
    std::size_t n=0; for(unsigned i=0;i<4;++i) { n |= std::size_t{header[i]} << (8*i); }
    if (n == 0 || n > 16*1024*1024 || n > end-begin-4) { failed(); }
    std::string value(n,'\0'); transfer(fd,value.data(),n,begin+4,false); begin+=4+n;
    auto url=Url::parse(value); if (!url || url->str()!=value) { failed(); } return *url;
}
struct Descriptor {
    int fd;
    explicit Descriptor(const std::filesystem::path& path, bool create = true) : fd{::open(path.c_str(),create ? O_RDWR|O_CREAT|O_CLOEXEC : O_RDONLY|O_CLOEXEC,0600)} { if(fd<0) { failed(); } }
    ~Descriptor(){::close(fd);}
};
std::filesystem::path file(const std::string& directory, std::size_t depth) { return std::filesystem::path{directory}/(std::to_string(depth)+".links"); }
}
std::size_t DeferredState::count() const {
    std::size_t n=0; for(const auto& e:extents) { if(e.count>std::numeric_limits<std::size_t>::max()-n) { failed(); } n+=static_cast<std::size_t>(e.count); } return n;
}
struct DiscoverySpool::Impl {
    mutable std::mutex mutex;
    std::string directory;
    bool persistent;
    struct Entry { std::unique_ptr<Descriptor> handle; DeferredExtent extent; };
    std::map<std::size_t,Entry> files;
};
DiscoverySpool::DiscoverySpool(const std::filesystem::path& prefix, bool persistent) : impl_{std::make_unique<Impl>()} {
    impl_->persistent=persistent;
    auto name=(prefix.empty() ? std::filesystem::temp_directory_path()/"fluxcrawl-discovery" : std::filesystem::absolute(prefix)).string()+".discovery-XXXXXX";
    if(!::mkdtemp(name.data())) { failed(); } impl_->directory=std::move(name);
}
DiscoverySpool::~DiscoverySpool(){ if(!impl_->persistent) { std::error_code error; std::filesystem::remove_all(impl_->directory,error); } }
void DiscoverySpool::append(const CrawlItem& item) {
    std::lock_guard lock{impl_->mutex};
    auto [it,inserted]=impl_->files.try_emplace(item.depth);
    auto& entry=it->second;
    if(inserted) { entry.handle=std::make_unique<Descriptor>(file(impl_->directory,item.depth)); entry.extent={item.depth,0,0,0}; }
    entry.extent.end=append_record(entry.handle->fd,entry.extent.end,item.url); ++entry.extent.count;
}
std::optional<CrawlItem> DiscoverySpool::pop() {
    std::lock_guard lock{impl_->mutex};
    for(auto& [depth,entry]:impl_->files) {
        if(!entry.extent.count) { continue; }
        auto url=read_record(entry.handle->fd,entry.extent.begin,entry.extent.end); --entry.extent.count;
        return CrawlItem{std::move(url),depth};
    }
    return {};
}
std::size_t DiscoverySpool::size() const { return capture().count(); }
DeferredState DiscoverySpool::capture() const {
    std::lock_guard lock{impl_->mutex}; DeferredState state{impl_->directory,{}};
    for(const auto& [depth,entry]:impl_->files) { (void)depth; if(entry.extent.count) { state.extents.push_back(entry.extent); } } return state;
}
void DiscoverySpool::sync() const {
    std::lock_guard lock{impl_->mutex};
    for(const auto& [depth,entry]:impl_->files) { (void)depth; if(::fsync(entry.handle->fd)) { failed(); } }
    Descriptor directory{impl_->directory,false}; if(::fsync(directory.fd)) { failed(); }
    Descriptor parent{std::filesystem::path{impl_->directory}.parent_path(),false}; if(::fsync(parent.fd)) { failed(); }
}
void DiscoverySpool::validate(const DeferredState& state, std::size_t max_depth) {
    if(state.extents.empty()) { return; }
    if(state.directory.empty() || !std::filesystem::path{state.directory}.is_absolute()) { failed(); }
    std::size_t previous=0; bool first=true;
    for(const auto& e:state.extents) {
        if(e.depth>max_depth || (!first && e.depth<=previous) || !e.count || e.begin>=e.end || e.end>std::uint64_t{std::numeric_limits<off_t>::max()}) { failed(); }
        Descriptor fd{file(state.directory,e.depth),false};
        if(e.end>std::filesystem::file_size(file(state.directory,e.depth))) { failed(); }
        auto cursor=e.begin; for(std::uint64_t n=0;n<e.count;++n) { (void)read_record(fd.fd,cursor,e.end); }
        if(cursor!=e.end) { failed(); } first=false; previous=e.depth;
    }
}
void DiscoverySpool::restore(const DeferredState& state) {
    // Fork immutable prefixes so two snapshot resumes never mutate each other's
    // companion files. Ignore the uncommitted suffix after the captured end.
    std::lock_guard lock{impl_->mutex};
    for(const auto& e:state.extents) {
        std::filesystem::copy_file(file(state.directory,e.depth),file(impl_->directory,e.depth));
        auto handle=std::make_unique<Descriptor>(file(impl_->directory,e.depth));
        if(::ftruncate(handle->fd,static_cast<off_t>(e.end))) { failed(); }
        impl_->files.emplace(e.depth,Impl::Entry{std::move(handle),e});
    }
}
struct DiskLinks::Impl { FILE* file=std::tmpfile(); std::uint64_t cursor=0,end=0; ~Impl(){ if(file) {std::fclose(file);} } };
DiskLinks::DiskLinks():impl_{std::make_unique<Impl>()}{if(!impl_->file){failed();}}
DiskLinks::~DiskLinks()=default;
void DiskLinks::append(const Url& url){impl_->end=append_record(::fileno(impl_->file),impl_->end,url);}
void DiskLinks::rewind(){impl_->cursor=0;}
std::optional<Url> DiskLinks::next(){if(impl_->cursor==impl_->end){return {};}return read_record(::fileno(impl_->file),impl_->cursor,impl_->end);}
}
