#include <deque>
#include <fstream>
#include <functional>

#include "RelayServer.h"

#include "PluginEventSifter.h"


void RelayServer::runWriter(ThreadPool<MsgWriter>::Thread &thr) {
    PluginEventSifter writePolicyPlugin;
    NegentropyFilterCache neFilterCache;

    // Per-pubkey posting rate limiter (Writer is a singleton thread, so no locking needed).
    // Maps pubkey (32-byte binary) -> recent post timestamps (seconds) within the sliding window.
    flat_hash_map<std::string, std::deque<uint64_t>> rateLimitBuckets;

    // Banned pubkeys (32-byte binary): all their events are rejected. Loaded from banListFile at
    // startup and (when banOnExceed is set) appended to when a pubkey exceeds the rate limit.
    flat_hash_set<std::string> bannedPubkeys;

    if (cfg().relay__rateLimit__banListFile.size()) {
        std::ifstream f(cfg().relay__rateLimit__banListFile);
        std::string line;
        while (std::getline(f, line)) {
            auto start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            auto end = line.find_last_not_of(" \t\r\n");
            line = line.substr(start, end - start + 1);
            if (line.empty() || line[0] == '#') continue;
            if (line.size() != 64) {
                LW << "Skipping invalid banlist entry (expected 64 hex chars): " << line;
                continue;
            }
            try {
                bannedPubkeys.insert(from_hex(line));
            } catch (std::exception &e) {
                LW << "Skipping invalid banlist hex: " << line;
            }
        }
        LI << "Rate limiter: loaded " << bannedPubkeys.size() << " banned pubkeys from " << cfg().relay__rateLimit__banListFile;
    }

    // Per-IP posting rate limiter. IPv4 keyed by the full 4-byte address; IPv6 keyed by the /64
    // prefix (first 8 bytes) so an attacker can't trivially rotate within their allocation.
    flat_hash_map<std::string, std::deque<uint64_t>> ipRateLimitBuckets;
    flat_hash_set<std::string> bannedIps;

    auto ipKey = [](const std::string &ip) -> std::string {
        if (ip.size() == 16) return ip.substr(0, 8); // IPv6 -> /64 prefix
        if (ip.size() == 4) return ip;               // IPv4 -> full address
        return std::string();                        // unknown/empty -> not rate-limitable
    };

    auto renderIpKey = [](const std::string &key) -> std::string {
        if (key.size() == 8) { std::string padded = key; padded.resize(16, '\0'); return renderIP(padded) + "/64"; }
        if (key.size() == 4) return renderIP(key);
        return std::string();
    };

    if (cfg().relay__ipRateLimit__banListFile.size()) {
        std::ifstream f(cfg().relay__ipRateLimit__banListFile);
        std::string line;
        while (std::getline(f, line)) {
            auto start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            auto end = line.find_last_not_of(" \t\r\n");
            line = line.substr(start, end - start + 1);
            if (line.empty() || line[0] == '#') continue;
            if (line.size() > 4 && line.compare(line.size() - 3, 3, "/64") == 0) line = line.substr(0, line.size() - 3);
            std::string bin = parseIP(line);
            if (bin.empty()) { LW << "Skipping invalid IP banlist entry: " << line; continue; }
            bannedIps.insert(ipKey(bin));
        }
        LI << "Rate limiter: loaded " << bannedIps.size() << " banned IPs from " << cfg().relay__ipRateLimit__banListFile;
    }

    // Which event kinds the rate limiter applies to. includeKinds empty => all kinds. excludeKinds
    // are always exempt and take precedence. These only gate rate-limit counting / auto-ban; an
    // already-banned pubkey is blocked regardless of kind.
    flat_hash_set<uint64_t> rlIncludeKinds, rlExcludeKinds;

    // Parse a simple list of kinds like "[1,1111]" or "1, 7" by extracting all integers.
    auto parseKindList = [](const std::string &str, flat_hash_set<uint64_t> &out) {
        std::string num;
        for (char c : str) {
            if (c >= '0' && c <= '9') {
                num += c;
            } else if (num.size()) {
                out.insert(std::stoull(num));
                num.clear();
            }
        }
        if (num.size()) out.insert(std::stoull(num));
    };

    parseKindList(cfg().relay__rateLimit__kinds, rlIncludeKinds);
    parseKindList(cfg().relay__rateLimit__excludeKinds, rlExcludeKinds);

    auto kindRateLimited = [&](uint64_t kind){
        if (rlExcludeKinds.contains(kind)) return false;
        // Exempt non-accumulating kind classes (so legit high-frequency publishers like WebRTC
        // signaling or live-activity/directory bots aren't limited). See NIP-01 kind ranges.
        if (cfg().relay__rateLimit__exemptEphemeral && isEphemeralKind(kind)) return false;
        if (cfg().relay__rateLimit__exemptReplaceable && isReplaceableKind(kind)) return false;
        if (cfg().relay__rateLimit__exemptAddressable && isParamReplaceableKind(kind)) return false;
        if (rlIncludeKinds.size() && !rlIncludeKinds.contains(kind)) return false;
        return true;
    };

    // Enforce a banlist + sliding-window rate limit for one key (pubkey or IP). Returns true if the
    // event was handled (rejected/shadowed) and should be skipped. Shared by both limiters.
    struct LimitParams {
        bool enabled;
        uint64_t windowSeconds;
        uint64_t maxEvents;
        bool banOnExceed;
        std::string mode;
        std::string banFile;
    };

    auto enforceLimit = [&](const std::string &key, const char *keyLabel,
                            flat_hash_set<std::string> &banned, flat_hash_map<std::string, std::deque<uint64_t>> &buckets,
                            const LimitParams &p, const std::function<std::string(const std::string&)> &renderKey,
                            uint64_t connId, std::string_view eventIdHex, uint64_t kind) -> bool {
        if (key.empty()) return false;

        if (banned.size() && banned.contains(key)) {
            LI << "[" << connId << "] blocked event " << eventIdHex << " from banned " << keyLabel << " " << renderKey(key);
            sendOKResponse(connId, eventIdHex, false, std::string("blocked: ") + keyLabel + " is banned");
            return true;
        }

        if (!p.enabled || !kindRateLimited(kind)) return false;

        uint64_t now = hoytech::curr_time_s();
        auto &bucket = buckets[key];
        while (bucket.size() && bucket.front() + p.windowSeconds <= now) bucket.pop_front();

        if (bucket.size() >= p.maxEvents) {
            if (p.banOnExceed) {
                banned.insert(key);
                buckets.erase(key);

                if (p.banFile.size()) {
                    std::ofstream f(p.banFile, std::ios::app);
                    if (f) f << renderKey(key) << "\n";
                    else LE << "Could not append to banListFile: " << p.banFile;
                }

                LW << "[" << connId << "] BANNED " << keyLabel << " " << renderKey(key) << " (exceeded rate limit); event " << eventIdHex << " rejected";
                sendOKResponse(connId, eventIdHex, false, std::string("blocked: ") + keyLabel + " is banned (rate limit exceeded)");
                return true;
            }

            bool shadow = p.mode == "shadow";
            LI << "[" << connId << "] rate-limited event " << eventIdHex << " from " << keyLabel << " " << renderKey(key) << (shadow ? " (shadow)" : "");
            sendOKResponse(connId, eventIdHex, shadow, shadow ? "" : "rate-limited: too many events, slow down");
            return true;
        }

        bucket.push_back(now);
        return false;
    };

    while(1) {
        auto newMsgs = thr.inbox.pop_all();

        // Filter out messages from already closed sockets

        {
            flat_hash_set<uint64_t> closedConns;

            for (auto &newMsg : newMsgs) {
                if (auto msg = std::get_if<MsgWriter::CloseConn>(&newMsg.msg)) closedConns.insert(msg->connId);
            }

            if (closedConns.size()) {
                decltype(newMsgs) newMsgsFiltered;

                for (auto &newMsg : newMsgs) {
                    if (auto msg = std::get_if<MsgWriter::AddEvent>(&newMsg.msg)) {
                        if (!closedConns.contains(msg->connId)) newMsgsFiltered.emplace_back(std::move(newMsg));
                    }
                }

                std::swap(newMsgs, newMsgsFiltered);
            }
        }

        // Prepare messages

        std::vector<EventToWrite> newEvents;

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgWriter::AddEvent>(&newMsg.msg)) {
                // Per-IP and per-pubkey banlist + rate limiting

                bool ipActive = cfg().relay__ipRateLimit__enabled || bannedIps.size();
                bool pkActive = cfg().relay__rateLimit__enabled || bannedPubkeys.size();

                if (ipActive || pkActive) {
                    PackedEventView packed(msg->packedStr);
                    auto eventIdHex = to_hex(packed.id());
                    uint64_t kind = packed.kind();
                    bool handled = false;

                    // Per-IP first: catches attackers who rotate pubkeys from the same IP

                    if (ipActive) {
                        LimitParams p{ cfg().relay__ipRateLimit__enabled, cfg().relay__ipRateLimit__windowSeconds,
                                       cfg().relay__ipRateLimit__maxEvents, cfg().relay__ipRateLimit__banOnExceed,
                                       cfg().relay__ipRateLimit__mode, cfg().relay__ipRateLimit__banListFile };
                        handled = enforceLimit(ipKey(msg->ipAddr), "IP", bannedIps, ipRateLimitBuckets, p, renderIpKey, msg->connId, eventIdHex, kind);
                    }

                    if (!handled && pkActive) {
                        LimitParams p{ cfg().relay__rateLimit__enabled, cfg().relay__rateLimit__windowSeconds,
                                       cfg().relay__rateLimit__maxEvents, cfg().relay__rateLimit__banOnExceed,
                                       cfg().relay__rateLimit__mode, cfg().relay__rateLimit__banListFile };
                        handled = enforceLimit(std::string(packed.pubkey()), "pubkey", bannedPubkeys, rateLimitBuckets, p,
                                               [](const std::string &k){ return to_hex(k); }, msg->connId, eventIdHex, kind);
                    }

                    if (handled) continue;
                }

                tao::json::value evJson = tao::json::from_string(msg->jsonStr);
                EventSourceType sourceType = msg->ipAddr.size() == 4 ? EventSourceType::IP4 : EventSourceType::IP6;
                std::string okMsg;
                auto res = writePolicyPlugin.acceptEvent(cfg().relay__writePolicy__plugin, evJson, sourceType, msg->ipAddr, okMsg);

                if (res == PluginEventSifterResult::Accept) {
                    newEvents.emplace_back(std::move(msg->packedStr), std::move(msg->jsonStr), msg);
                } else {
                    PackedEventView packed(msg->packedStr);
                    auto eventIdHex = to_hex(packed.id());

                    if (okMsg.size()) LI << "[" << msg->connId << "] write policy blocked event " << eventIdHex << ": " << okMsg;

                    sendOKResponse(msg->connId, eventIdHex, res == PluginEventSifterResult::ShadowReject, okMsg);
                }
            }
        }

        // Evict expired rate-limit buckets to bound memory growth

        auto evictExpired = [](flat_hash_map<std::string, std::deque<uint64_t>> &buckets, uint64_t windowSeconds){
            if (!buckets.size()) return;
            uint64_t now = hoytech::curr_time_s();
            for (auto it = buckets.begin(); it != buckets.end(); ) {
                auto &bucket = it->second;
                while (bucket.size() && bucket.front() + windowSeconds <= now) bucket.pop_front();
                if (bucket.empty()) it = buckets.erase(it);
                else ++it;
            }
        };

        evictExpired(rateLimitBuckets, cfg().relay__rateLimit__windowSeconds);
        evictExpired(ipRateLimitBuckets, cfg().relay__ipRateLimit__windowSeconds);

        if (!newEvents.size()) continue;

        // Do write

        try {
            auto txn = env.txn_rw();
            writeEvents(txn, neFilterCache, newEvents);
            txn.commit();
        } catch (std::exception &e) {
            LE << "Error writing " << newEvents.size() << " events: " << e.what();

            for (auto &newEvent : newEvents) {
                PackedEventView packed(newEvent.packedStr);
                auto eventIdHex = to_hex(packed.id());
                MsgWriter::AddEvent *addEventMsg = static_cast<MsgWriter::AddEvent*>(newEvent.userData);

                std::string message = "Write error: ";
                message += e.what();

                sendOKResponse(addEventMsg->connId, eventIdHex, false, message);
            }

            continue;
        }

        // Log

        for (auto &newEvent : newEvents) {
            PackedEventView packed(newEvent.packedStr);
            auto eventIdHex = to_hex(packed.id());
            std::string message;
            bool written = false;

            if (newEvent.status == EventWriteStatus::Written) {
                LI << "Inserted event. id=" << eventIdHex << " levId=" << newEvent.levId;
                written = true;
            } else if (newEvent.status == EventWriteStatus::Duplicate) {
                message = "duplicate: have this event";
                written = true;
            } else if (newEvent.status == EventWriteStatus::Replaced) {
                message = "replaced: have newer event";
            } else if (newEvent.status == EventWriteStatus::Deleted) {
                message = "deleted: user requested deletion";
            }

            if (newEvent.status != EventWriteStatus::Written) {
                LI << "Rejected event. " << message << ", id=" << eventIdHex;
            }

            MsgWriter::AddEvent *addEventMsg = static_cast<MsgWriter::AddEvent*>(newEvent.userData);

            sendOKResponse(addEventMsg->connId, eventIdHex, written, message);
        }
    }
}
