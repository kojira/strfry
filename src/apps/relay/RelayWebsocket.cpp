#include <fstream>
#include <unordered_map>
#include <sys/stat.h>

#include "RelayServer.h"

#include "StrfryTemplates.h"
#include "app_git_version.h"



static std::string preGenerateHttpResponse(const std::string &contentType, const std::string &content, const std::string &extraHeaders = "") {
    std::string output = "HTTP/1.1 200 OK\r\n";
    output += std::string("Content-Type: ") + contentType + "\r\n";
    output += "Access-Control-Allow-Origin: *\r\n";
    output += extraHeaders;
    output += "Connection: keep-alive\r\n";
    output += "Server: strfry\r\n";
    output += std::string("Content-Length: ") + std::to_string(content.size()) + "\r\n";
    output += "\r\n";
    output += content;
    return output;
};



void RelayServer::runWebsocket(ThreadPool<MsgWebsocket>::Thread &thr) {
    struct Connection {
        uWS::WebSocket<uWS::SERVER> *websocket;
        uint64_t connId;
        uint64_t connectedTimestamp;
        std::string ipAddr;
        struct Stats {
            uint64_t bytesUp = 0;
            uint64_t bytesUpCompressed = 0;
            uint64_t bytesDown = 0;
            uint64_t bytesDownCompressed = 0;
        } stats;

        Connection(uWS::WebSocket<uWS::SERVER> *p, uint64_t connId_)
            : websocket(p), connId(connId_), connectedTimestamp(hoytech::curr_time_us()) { }
        Connection(const Connection &) = delete;
        Connection(Connection &&) = delete;
    };

    uWS::Hub hub;
    uWS::Group<uWS::SERVER> *hubGroup = nullptr;
    flat_hash_map<uint64_t, Connection*> connIdToConnection;
    uint64_t nextConnectionId = 1;
    bool gracefulShutdown = false;

    std::string tempBuf;
    tempBuf.reserve(cfg().events__maxEventSize + MAX_SUBID_SIZE + 100);


    auto supportedNips = []{
        tao::json::value output = tao::json::value::array({ 1, 2, 4, 9, 11, 22, 28, 40, 70, 77 });
        if (cfg().relay__info__nips.size() == 0) return output;

        try {
            output = tao::json::from_string(cfg().relay__info__nips);
        } catch (std::exception &e) {
            LE << "Unable to parse config param relay.info.nips: " << e.what();
        }

        return output;
    };

    auto getServerInfoHttpResponse = [&supportedNips, ver = uint64_t(0), rendered = std::string("")]() mutable {
        if (ver != cfg().version()) {
            tao::json::value nip11 = tao::json::value({
                { "supported_nips", supportedNips() },
                { "software", "git+https://github.com/hoytech/strfry.git" },
                { "version", APP_GIT_VERSION },
                { "negentropy", negentropy::PROTOCOL_VERSION - 0x60 },
                { "limitation", tao::json::value({
                    { "max_message_length", cfg().relay__maxWebsocketPayloadSize },
                    { "max_subscriptions", cfg().relay__maxSubsPerConnection },
                    { "max_limit", cfg().relay__maxFilterLimit },
                }) },
            });

            if (cfg().relay__info__name.size()) nip11["name"] = cfg().relay__info__name;
            if (cfg().relay__info__description.size()) nip11["description"] = cfg().relay__info__description;
            if (cfg().relay__info__contact.size()) nip11["contact"] = cfg().relay__info__contact;
            if (cfg().relay__info__pubkey.size()) nip11["pubkey"] = cfg().relay__info__pubkey;
            if (cfg().relay__info__icon.size()) nip11["icon"] = cfg().relay__info__icon;

            rendered = preGenerateHttpResponse("application/json", tao::json::to_string(nip11));
            ver = cfg().version();
        }

        return std::string_view(rendered); // memory only valid until next call
    };

    auto getLandingPageHttpResponse = [&supportedNips, ver = uint64_t(0), rendered = std::string("")]() mutable {
        if (ver != cfg().version()) {
            struct {
                std::string supportedNips;
                std::string version;
                uint64_t negentropy;
            } ctx = { tao::json::to_string(supportedNips()), APP_GIT_VERSION, negentropy::PROTOCOL_VERSION - 0x60 };

            rendered = preGenerateHttpResponse("text/html", ::strfrytmpl::landing(ctx).str);
            ver = cfg().version();
        }

        return std::string_view(rendered); // memory only valid until next call
    };

    auto getCustomLandingPageHttpResponse = [path = std::string(""), mtime = (int64_t)-1, lastCheck = (uint64_t)0, rendered = std::string("")]() mutable -> std::string_view {
        const std::string &p = cfg().relay__landingPageFile;
        if (p.empty()) { rendered.clear(); return std::string_view(rendered); }

        uint64_t now = hoytech::curr_time_us();
        if (p == path && now - lastCheck < 5'000'000) return std::string_view(rendered);
        lastCheck = now;

        struct stat st;
        if (::stat(p.c_str(), &st) != 0) {
            if (rendered.size()) LW << "landingPageFile unavailable, using built-in page: " << p;
            rendered.clear(); path = p; mtime = -1;
            return std::string_view(rendered);
        }

        int64_t m = (int64_t)st.st_mtime;
        if (p == path && m == mtime) return std::string_view(rendered);

        std::ifstream f(p, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!f.good() && !f.eof()) { rendered.clear(); return std::string_view(rendered); }

        rendered = preGenerateHttpResponse("text/html; charset=utf-8", content, "Cache-Control: max-age=300\r\n");
        path = p; mtime = m;
        LI << "Loaded landingPageFile " << p << " (" << content.size() << " bytes)";
        return std::string_view(rendered);
    };

    // Static files next to landingPageFile: GET /assets/<name> -> <dir of landingPageFile>/assets/<name>
    // (only [A-Za-z0-9._-] names, whitelisted extensions; cached per file and re-read on mtime change)
    auto getLandingAssetHttpResponse = [cache = std::unordered_map<std::string, std::pair<int64_t, std::string>>()](const std::string &url) mutable -> std::string_view {
        static const std::string empty;
        const std::string &lp = cfg().relay__landingPageFile;
        if (lp.empty() || url.rfind("/assets/", 0) != 0) return std::string_view(empty);
        std::string name = url.substr(8);
        auto q = name.find_first_of("?#");
        if (q != std::string::npos) name.resize(q);
        if (name.empty() || name.size() > 128 || name[0] == '.') return std::string_view(empty);
        for (char ch : name) if (!(isalnum((unsigned char)ch) || ch == '.' || ch == '_' || ch == '-')) return std::string_view(empty);

        static const std::vector<std::pair<std::string, std::string>> types = {
            { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".webp", "image/webp" },
            { ".svg", "image/svg+xml" }, { ".ico", "image/x-icon" }, { ".css", "text/css; charset=utf-8" },
            { ".js", "text/javascript; charset=utf-8" }, { ".json", "application/json" }, { ".txt", "text/plain; charset=utf-8" },
        };
        std::string ctype;
        for (auto &t : types) {
            if (name.size() > t.first.size() && name.compare(name.size() - t.first.size(), t.first.size(), t.first) == 0) { ctype = t.second; break; }
        }
        if (ctype.empty()) return std::string_view(empty);

        auto slash = lp.find_last_of('/');
        std::string path = (slash == std::string::npos ? std::string(".") : lp.substr(0, slash)) + "/assets/" + name;

        struct stat st;
        if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 8 * 1024 * 1024) { cache.erase(name); return std::string_view(empty); }
        auto &ent = cache[name];
        if (ent.first == (int64_t)st.st_mtime && ent.second.size()) return std::string_view(ent.second);

        std::ifstream f(path, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!f.good() && !f.eof()) { cache.erase(name); return std::string_view(empty); }
        ent.first = (int64_t)st.st_mtime;
        ent.second = preGenerateHttpResponse(ctype, content, "Cache-Control: public, max-age=600\r\n");
        return std::string_view(ent.second);
    };

    auto getNodeInfoHttpResponse = [ver = uint64_t(0), rendered = std::string("")](std::string host) mutable {
        if (ver != cfg().version()) {
            tao::json::value nodeinfo = tao::json::value({
                { "links", tao::json::value::array({
                    tao::json::value({
                        { "rel", "http://nodeinfo.diaspora.software/ns/schema/2.1" },
                        { "href", "https://" + host + "/nodeinfo/2.1" },
                    }),
                }) },
            });

            rendered = preGenerateHttpResponse("application/json", tao::json::to_string(nodeinfo));
            ver = cfg().version();
        }

        return std::string_view(rendered); // memory only valid until next call
    };

    auto getNodeInfo21HttpResponse = [ver = uint64_t(0), rendered = std::string("")]() mutable {
        if (ver != cfg().version()) {
            // https://github.com/jhass/nodeinfo/blob/main/schemas/2.1/schema.json
            tao::json::value nodeinfo = tao::json::value({
                { "version", "2.1" },
                { "software", tao::json::value({
                    { "name", "strfry" },
                    { "version", APP_GIT_VERSION },
                    { "repository", "https://github.com/hoytech/strfry"},
                    { "homepage", "https://github.com/hoytech/strfry"},
                }) },
                { "protocols", tao::json::value::array({
                    "nostr",
                }) },
                { "services", tao::json::value({
                    { "inbound", tao::json::value::array({}) },
                    { "outbound", tao::json::value::array({}) },
                }) },
                { "openRegistrations", false },
                { "usage", tao::json::value({
                    { "users", tao::json::value({}) },
                }) },
                { "metadata", tao::json::value({
                    { "features", tao::json::value::array({
                        "nostr_relay",
                    }) },
                }) },
            });

            rendered = preGenerateHttpResponse("application/json", tao::json::to_string(nodeinfo));
            ver = cfg().version();
        }

        return std::string_view(rendered); // memory only valid until next call
    };


    {
        int extensionOptions = 0;

        if (cfg().relay__compression__enabled) extensionOptions |= uWS::PERMESSAGE_DEFLATE;
        if (cfg().relay__compression__slidingWindow) extensionOptions |= uWS::SLIDING_DEFLATE_WINDOW;

        hubGroup = hub.createGroup<uWS::SERVER>(extensionOptions, cfg().relay__maxWebsocketPayloadSize);
    }

    if (cfg().relay__autoPingSeconds) hubGroup->startAutoPing(cfg().relay__autoPingSeconds * 1'000);

    hubGroup->onHttpRequest([&](uWS::HttpResponse *res, uWS::HttpRequest req, char *data, size_t length, size_t remainingBytes){
        LI << "HTTP request for [" << req.getUrl().toString() << "]";

        std::string host = req.getHeader("host").toString();
        std::string url = req.getUrl().toString();

        if (url == "/.well-known/nodeinfo") {
            auto nodeInfo = getNodeInfoHttpResponse(host);
            res->write(nodeInfo.data(), nodeInfo.size());
        } else if (url == "/nodeinfo/2.1") {
            auto nodeInfo = getNodeInfo21HttpResponse();
            res->write(nodeInfo.data(), nodeInfo.size());
        } else if (url.rfind("/assets/", 0) == 0) {
            auto asset = getLandingAssetHttpResponse(url);
            if (asset.size()) {
                res->write(asset.data(), asset.size());
            } else {
                static const std::string notFound = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 9\r\n\r\nnot found";
                res->write(notFound.data(), notFound.size());
            }
        } else if (req.getHeader("accept").toStringView() == "application/nostr+json") {
            auto info = getServerInfoHttpResponse();
            res->write(info.data(), info.size());
        } else {
            auto custom = getCustomLandingPageHttpResponse();
            if (custom.size()) {
                res->write(custom.data(), custom.size());
            } else {
                auto landing = getLandingPageHttpResponse();
                res->write(landing.data(), landing.size());
            }
        }
    });

    hubGroup->onConnection([&](uWS::WebSocket<uWS::SERVER> *ws, uWS::HttpRequest req) {
        uint64_t connId = nextConnectionId++;

        Connection *c = new Connection(ws, connId);

        if (cfg().relay__realIpHeader.size()) {
            auto header = req.getHeader(cfg().relay__realIpHeader.c_str()).toString(); // not string_view: parseIP needs trailing 0 byte

            // HACK: uWebSockets strips leading : characters, which interferes with IPv6 parsing.
            // This fixes it for the common ::1 and ::ffff:1.2.3.4 cases. FIXME: fix the underlying library.
            if (header == "1" || header.starts_with("ffff:")) header = std::string("::") + header;

            c->ipAddr = parseIP(header);
            if (c->ipAddr.size() == 0) LW << "Couldn't parse IP from header " << cfg().relay__realIpHeader << ": " << header;
        }

        if (c->ipAddr.size() == 0) c->ipAddr = ws->getAddressBytes();

        ws->setUserData((void*)c);
        connIdToConnection.emplace(connId, c);

        bool compEnabled, compSlidingWindow;
        ws->getCompressionState(compEnabled, compSlidingWindow);
        LI << "[" << connId << "] Connect from " << renderIP(c->ipAddr)
           << " compression=" << (compEnabled ? 'Y' : 'N')
           << " sliding=" << (compSlidingWindow ? 'Y' : 'N')
        ;

        if (cfg().relay__enableTcpKeepalive) {
            int optval = 1;
            if (setsockopt(ws->getFd(), SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval))) {
                LW << "Failed to enable TCP keepalive: " << strerror(errno);
            }
        }
    });

    hubGroup->onDisconnection([&](uWS::WebSocket<uWS::SERVER> *ws, int code, char *message, size_t length) {
        auto *c = (Connection*)ws->getUserData();
        uint64_t connId = c->connId;

        auto upComp = renderPercent(1.0 - (double)c->stats.bytesUpCompressed / c->stats.bytesUp);
        auto downComp = renderPercent(1.0 - (double)c->stats.bytesDownCompressed / c->stats.bytesDown);

        LI << "[" << connId << "] Disconnect from " << renderIP(c->ipAddr)
           << " (" << code << "/" << (message ? std::string_view(message, length) : "-") << ")"
           << " UP: " << renderSize(c->stats.bytesUp) << " (" << upComp << " compressed)"
           << " DN: " << renderSize(c->stats.bytesDown) << " (" << downComp << " compressed)"
        ;

        tpIngester.dispatch(connId, MsgIngester{MsgIngester::CloseConn{connId}});

        connIdToConnection.erase(connId);
        delete c;

        if (gracefulShutdown) {
            LI << "Graceful shutdown in progress: " << connIdToConnection.size() << " connections remaining";
            if (connIdToConnection.size() == 0) {
                LW << "All connections closed, shutting down";
                ::exit(0);
            }
        }
    });

    hubGroup->onMessage2([&](uWS::WebSocket<uWS::SERVER> *ws, char *message, size_t length, uWS::OpCode opCode, size_t compressedSize) {
        auto &c = *(Connection*)ws->getUserData();

        c.stats.bytesDown += length;
        c.stats.bytesDownCompressed += compressedSize;

        tpIngester.dispatch(c.connId, MsgIngester{MsgIngester::ClientMessage{c.connId, c.ipAddr, std::string(message, length)}});
    });


    std::function<void()> asyncCb = [&]{
        auto newMsgs = thr.inbox.pop_all_no_wait();

        auto doSend = [&](uint64_t connId, std::string_view payload, uWS::OpCode opCode){
            auto it = connIdToConnection.find(connId);
            if (it == connIdToConnection.end()) return;
            auto &c = *it->second;

            size_t compressedSize;
            auto cb = [](uWS::WebSocket<uWS::SERVER> *webSocket, void *data, bool cancelled, void *reserved){};
            c.websocket->send(payload.data(), payload.size(), opCode, cb, nullptr, true, &compressedSize);
            c.stats.bytesUp += payload.size();
            c.stats.bytesUpCompressed += compressedSize;
        };

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgWebsocket::Send>(&newMsg.msg)) {
                doSend(msg->connId, msg->payload, uWS::OpCode::TEXT);
            } else if (auto msg = std::get_if<MsgWebsocket::SendBinary>(&newMsg.msg)) {
                doSend(msg->connId, msg->payload, uWS::OpCode::BINARY);
            } else if (auto msg = std::get_if<MsgWebsocket::SendEventToBatch>(&newMsg.msg)) {
                tempBuf.reserve(13 + MAX_SUBID_SIZE + msg->evJson.size());
                tempBuf.resize(10 + MAX_SUBID_SIZE);
                tempBuf += "\",";
                tempBuf += msg->evJson;
                tempBuf += "]";

                for (auto &item : msg->list) {
                    auto subIdSv = item.subId.sv();
                    auto *p = tempBuf.data() + MAX_SUBID_SIZE - subIdSv.size();
                    memcpy(p, "[\"EVENT\",\"", 10);
                    memcpy(p + 10, subIdSv.data(), subIdSv.size());
                    doSend(item.connId, std::string_view(p, 13 + subIdSv.size() + msg->evJson.size()), uWS::OpCode::TEXT);
                }
            } else if (std::get_if<MsgWebsocket::GracefulShutdown>(&newMsg.msg)) {
                LW << "Initiating graceful shutdown: " << connIdToConnection.size() << " connections remaining";
                gracefulShutdown = true;
                hubGroup->stopListening();
            }
        }
    };

    hubTrigger = new uS::Async(hub.getLoop());
    hubTrigger->setData(&asyncCb);

    hubTrigger->start([](uS::Async *a){
        auto *r = static_cast<std::function<void()> *>(a->getData());
        (*r)();
    });



    int port = cfg().relay__port;

    std::string bindHost = cfg().relay__bind;

    if (!hub.listen(bindHost.c_str(), port, nullptr, uS::REUSE_PORT, hubGroup)) throw herr("unable to listen on port ", port);

    LI << "Started websocket server on " << bindHost << ":" << port;

    hub.run();
}
