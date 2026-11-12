#include <fluxcrawl/observability_server.hpp>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <fstream>
#include <thread>

namespace fluxcrawl {
namespace asio=boost::asio;
namespace beast=boost::beast;
namespace http=beast::http;
namespace ws=beast::websocket;
using tcp=asio::ip::tcp;
struct ObservabilityServer::Impl {
    struct Session;
    asio::io_context io{1};
    tcp::acceptor acceptor{io};
    asio::steady_timer timer{io};
    std::shared_ptr<Observability> source;
    std::filesystem::path assets;
    std::vector<std::weak_ptr<Session>> sessions;
    std::shared_ptr<const std::string> cached, envelope;
    std::size_t serializations=0, dropped=0, stalled=0;
    std::uint64_t cached_sequence=std::numeric_limits<std::uint64_t>::max();
    std::uint16_t bound_port = 0;
    std::thread thread;
    Impl(std::shared_ptr<Observability> input, std::uint16_t port, std::filesystem::path directory)
        : source{std::move(input)}, assets{std::move(directory)}
    {
        if (assets.empty()) { assets=FLUXCRAWL_DASHBOARD_ASSETS; }
        acceptor.open(tcp::v4());
        acceptor.set_option(asio::socket_base::reuse_address(true));
        acceptor.bind({asio::ip::make_address("127.0.0.1"),port});
        acceptor.listen(16);
        bound_port=acceptor.local_endpoint().port();
        accept(); tick();
        thread=std::thread([this] { io.run(); });
    }
    ~Impl() { io.stop(); if (thread.joinable()) { thread.join(); } }
    std::shared_ptr<const std::string> metrics() {
        auto snapshot=source->latest();
        if (!cached || cached_sequence!=snapshot->sequence) {
            cached=std::make_shared<const std::string>(observability_json(*snapshot)); cached_sequence=snapshot->sequence; ++serializations;
            envelope=std::make_shared<const std::string>(boost::json::serialize(boost::json::object{{"type","snapshot"},{"timestamp_ms",snapshot->elapsed_ms},{"data",boost::json::parse(*cached)}}));
        }
        return cached;
    }
    void accept();
    void tick();
};
struct ObservabilityServer::Impl::Session : std::enable_shared_from_this<Session> {
    Impl& owner;
    ws::stream<beast::tcp_stream> stream;
    beast::flat_buffer buffer;
    http::request_parser<http::string_body> parser;
    asio::steady_timer deadline;
    bool websocket=false, writing=false;
    Session(Impl& server,tcp::socket socket) : owner{server},stream{std::move(socket)},deadline{server.io} {
        beast::get_lowest_layer(stream).socket().set_option(asio::socket_base::send_buffer_size(16384));
        parser.body_limit(0); parser.header_limit(8192);
    }
    void close() { beast::error_code ec; beast::get_lowest_layer(stream).socket().close(ec); }
    void start() {
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds{5});
        http::async_read(stream.next_layer(),buffer,parser,[self=shared_from_this()](beast::error_code ec,std::size_t) {
            if (!ec) { self->route(); }
        });
    }
    void route() {
        auto req=parser.release();
        if (req.method()!=http::verb::get) { reply(http::status::method_not_allowed,"text/plain","Read-only API\n"); return; }
        const auto origin=req[http::field::origin];
        if (!origin.empty() && origin!=std::string("http://")+std::string(req[http::field::host])) {
            reply(http::status::forbidden,"text/plain","Origin rejected\n"); return;
        }
        const std::string target{req.target()};
        if (target=="/ws" && ws::is_upgrade(req)) {
            beast::get_lowest_layer(stream).expires_never();
            stream.set_option(ws::stream_base::timeout::suggested(beast::role_type::server));
            stream.read_message_max(1024);
            stream.async_accept(req,[self=shared_from_this()](beast::error_code ec) {
                if (!ec) { self->websocket=true; self->read(); self->push(); }
            });
            return;
        }
        if (target=="/api/metrics") { reply(http::status::ok,"application/json",*owner.metrics()); return; }
        if (target=="/api/status" || target=="/api/hosts" || target=="/api/topology") {
            auto data=boost::json::parse(*owner.metrics()).as_object();
            boost::json::object out{{"schema_version",1}};
            if (target=="/api/status") {
                for (const auto* key:{"running","state","engine","elapsed_ms","sequence"}) { out[key]=data.at(key); }
                out["telemetry"]=boost::json::object{{"serialized_snapshots",owner.serializations},{"dropped_updates",owner.dropped},{"stalled_disconnects",owner.stalled},{"connection_limit",16},{"max_inflight_per_client",1}};
            } else { out[target=="/api/hosts"?"hosts":"topology"]=data.at(target=="/api/hosts"?"hosts":"topology"); }
            reply(http::status::ok,"application/json",boost::json::serialize(out)); return;
        }
        // Only Vite's fixed index and flat hashed asset directory are served.
        std::string path;
        if (target=="/" || target=="/index.html") { path="index.html"; }
        else if (target.starts_with("/assets/") && target.find("..") == std::string::npos && target.find_first_of("%?\\",8)==std::string::npos && target.find('/',8)==std::string::npos) { path=target.substr(1); }
        if (!path.empty()) {
            std::ifstream input{owner.assets/path,std::ios::binary};
            if (input) {
                std::string body{std::istreambuf_iterator<char>{input},{}};
                const auto type=path.ends_with(".js")?"text/javascript":path.ends_with(".css")?"text/css":"text/html";
                reply(http::status::ok,type,std::move(body)); return;
            }
            if (path=="index.html") { reply(http::status::service_unavailable,"text/plain","Build dashboard assets: cd dashboard && npm ci && npm run build\n"); return; }
        }
        reply(http::status::not_found,"text/plain","Not found\n");
    }
    void reply(http::status status, std::string_view type,std::string body) {
        auto response=std::make_shared<http::response<http::string_body>>(status,11);
        response->set(http::field::content_type,type); response->set(http::field::cache_control,"no-store");
        response->set("X-Content-Type-Options","nosniff");
        response->set("Content-Security-Policy","default-src 'self'; connect-src 'self'; style-src 'self' 'unsafe-inline'; object-src 'none'; frame-ancestors 'none'");
        response->body()=std::move(body); response->prepare_payload(); response->keep_alive(false);
        beast::get_lowest_layer(stream).expires_after(std::chrono::seconds{2});
        http::async_write(stream.next_layer(),*response,[self=shared_from_this(),response](beast::error_code,std::size_t) { self->close(); });
    }
    void read() {
        stream.async_read(buffer,[self=shared_from_this()](beast::error_code ec,std::size_t) {
            if (ec) { self->close(); return; }
            self->buffer.consume(self->buffer.size()); self->read();
        });
    }
    void push() {
        if (!websocket || !beast::get_lowest_layer(stream).socket().is_open()) { return; }
        if (writing) { ++owner.dropped; return; }
        writing=true;
        (void)owner.metrics();
        auto payload=owner.envelope;
        stream.text(true);
        deadline.expires_after(std::chrono::seconds{2});
        deadline.async_wait([self=shared_from_this()](beast::error_code ec) { if (!ec) { ++self->owner.stalled; self->close(); } });
        stream.async_write(asio::buffer(*payload),[self=shared_from_this(),payload](beast::error_code ec,std::size_t) {
            self->deadline.cancel(); self->writing=false; if (ec) { self->close(); }
        });
    }
};
void ObservabilityServer::Impl::accept() {
    acceptor.async_accept([this](beast::error_code ec,tcp::socket socket) {
        std::erase_if(sessions,[](const auto& session){return session.expired();});
        if (!ec && sessions.size()<16) {
            auto session=std::make_shared<Session>(*this,std::move(socket)); sessions.push_back(session); session->start();
        }
        if (acceptor.is_open()) { accept(); }
    });
}
void ObservabilityServer::Impl::tick() {
    timer.expires_after(source->interval());
    timer.async_wait([this](beast::error_code ec) {
        if (ec) { return; }
        std::erase_if(sessions,[](const auto& session){return session.expired();});
        for (auto& weak:sessions) { if (auto session=weak.lock()) { session->push(); } }
        tick();
    });
}
ObservabilityServer::ObservabilityServer(std::shared_ptr<Observability> source,std::uint16_t port,std::filesystem::path assets)
    : impl_{std::make_unique<Impl>(std::move(source),port,std::move(assets))} {}
ObservabilityServer::~ObservabilityServer()=default;
std::uint16_t ObservabilityServer::port() const { return impl_->bound_port; }
}
