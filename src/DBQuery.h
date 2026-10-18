#pragma once

#include "golpe.h"

#include "Subscription.h"
#include "filters.h"
#include "events.h"
#include "SearchIndex.h"


struct DBScan : NonCopyable {
    struct CandidateEvent {
      private:
        uint64_t packed;
        uint64_t levIdStorage;

      public:
        CandidateEvent(uint64_t levId, uint64_t created, uint64_t scanIndex) : packed(scanIndex << 40 | created), levIdStorage(levId) {}

        uint64_t levId() { return levIdStorage; }
        uint64_t created() { return packed & 0xFF'FFFFFFFF; }
        uint64_t scanIndex() { return packed >> 40; }
    };

    enum class KeyMatchResult {
        Yes,
        No,
    };

    struct ScanCursor {
        std::string resumeKey;
        uint64_t resumeVal;
        std::function<KeyMatchResult(std::string_view)> keyMatch;
        uint64_t outstanding = 0; // number of records remaining in eventQueue, decremented in DBScan::scan

        bool active() {
            return resumeKey.size() > 0;
        }

        uint64_t collect(lmdb::txn &txn, DBScan &s, uint64_t scanIndex, uint64_t limit, std::deque<CandidateEvent> &output) {
            uint64_t added = 0;

            while (active() && limit > 0) {
                bool finished = env.generic_foreachFull(txn, s.indexDbi, resumeKey, lmdb::to_sv<uint64_t>(resumeVal), [&](auto k, auto v) {
                    if (limit == 0) {
                        resumeKey = std::string(k);
                        resumeVal = lmdb::from_sv<uint64_t>(v);
                        return false;
                    }

                    auto matched = keyMatch(k);
                    if (matched == KeyMatchResult::No) {
                        resumeKey = "";
                        return false;
                    }

                    uint64_t created;

                    {
                        ParsedKey_StringUint64 parsedKey(k);
                        created = parsedKey.n;

                        if (s.f.since && created < s.f.since) {
                            resumeKey = makeKey_StringUint64(parsedKey.s, 0);
                            resumeVal = 0;
                            return false;
                        }

                        if (s.f.until && created > s.f.until) {
                            resumeKey = makeKey_StringUint64(parsedKey.s, s.f.until);
                            resumeVal = MAX_U64;
                            return false;
                        }
                    }

                    if (matched == KeyMatchResult::Yes) {
                        uint64_t levId = lmdb::from_sv<uint64_t>(v);
                        output.emplace_back(levId, created, scanIndex);
                        added++;
                        limit--;
                    }

                    return true;
                }, true);

                if (finished) resumeKey = "";
            }

            outstanding += added;
            return added;
        }
    };

    const NostrFilter &f;
    bool indexOnly;
    lmdb::dbi indexDbi;
    const char *desc = "?";
    std::vector<ScanCursor> cursors;
    std::deque<CandidateEvent> eventQueue; // sorted descending by created
    uint64_t initialScanDepth;
    uint64_t refillScanDepth;
    uint64_t nextInitIndex = 0;
    uint64_t approxWork = 0;

    // Search scans: candidates are computed up-front from the search index (see initSearch())
    bool isSearch = false;
    bool searchInited = false;
    std::vector<uint64_t> searchCandidates; // levIds, descending
    size_t searchCursor = 0;
    uint64_t searchVerified = 0;
    Decompressor searchDecomp;

    DBScan(const NostrFilter &f) : f(f) {
        indexOnly = f.indexOnlyScans;

        if (f.search) {
            isSearch = true;
            indexOnly = false;
            desc = "Search";
            initialScanDepth = refillScanDepth = 0;
            return;
        }

        if (f.ids) {
            indexDbi = env.dbi_Event__id;
            desc = "ID";

            cursors.reserve(f.ids->size());
            for (uint64_t i = 0; i < f.ids->size(); i++) {
                std::string search = f.ids->at(i);

                cursors.emplace_back(
                    search + std::string(8, '\xFF'),
                    MAX_U64,
                    [search](std::string_view k){
                        return k.starts_with(search) ? KeyMatchResult::Yes : KeyMatchResult::No;
                    }
                );
            }
        } else if (f.tags.size()) {
            indexDbi = env.dbi_Event__tag;
            desc = "Tag";

            char tagName = '\0';
            {
                uint64_t numTags = MAX_U64;
                for (const auto &[tn, filterSet] : f.tags) {
                    if (filterSet.size() < numTags) {
                        numTags = filterSet.size();
                        tagName = tn;
                    }
                }
            }

            const auto &filterSet = f.tags.at(tagName);

            cursors.reserve(filterSet.size());
            for (uint64_t i = 0; i < filterSet.size(); i++) {
                std::string search;
                search += tagName;
                search += filterSet.at(i);

                cursors.emplace_back(
                    search + std::string(8, '\xFF'),
                    MAX_U64,
                    [search](std::string_view k){
                        return k.size() == search.size() + 8 && k.starts_with(search) ? KeyMatchResult::Yes : KeyMatchResult::No;
                    }
                );
            }
        } else if (f.authors && f.kinds && f.authors->size() * f.kinds->size() < 1'000) {
            indexDbi = env.dbi_Event__pubkeyKind;
            desc = "PubkeyKind";

            cursors.reserve(f.authors->size() * f.kinds->size());
            for (uint64_t i = 0; i < f.authors->size(); i++) {
                for (uint64_t j = 0; j < f.kinds->size(); j++) {
                    uint64_t kind = f.kinds->at(j);

                    std::string search = f.authors->at(i);
                    search += lmdb::to_sv<uint64_t>(kind);

                    cursors.emplace_back(
                        search + std::string(8, '\xFF'),
                        MAX_U64,
                        [search, kind](std::string_view k){
                            if (!k.starts_with(search)) return KeyMatchResult::No;
                            return KeyMatchResult::Yes;
                        }
                    );
                }
            }
        } else if (f.authors) {
            if (f.kinds) indexOnly = false; // because of the size limit in previous test

            indexDbi = env.dbi_Event__pubkey;
            desc = "Pubkey";

            cursors.reserve(f.authors->size());
            for (uint64_t i = 0; i < f.authors->size(); i++) {
                std::string search = f.authors->at(i);

                cursors.emplace_back(
                    search + std::string(8, '\xFF'),
                    MAX_U64,
                    [search](std::string_view k){
                        return k.starts_with(search) ? KeyMatchResult::Yes : KeyMatchResult::No;
                    }
                );
            }
        } else if (f.kinds) {
            indexDbi = env.dbi_Event__kind;
            desc = "Kind";

            cursors.reserve(f.kinds->size());
            for (uint64_t i = 0; i < f.kinds->size(); i++) {
                uint64_t kind = f.kinds->at(i);

                cursors.emplace_back(
                    std::string(lmdb::to_sv<uint64_t>(kind)) + std::string(8, '\xFF'),
                    MAX_U64,
                    [kind](std::string_view k){
                        ParsedKey_Uint64Uint64 parsedKey(k);
                        return parsedKey.n1 == kind ? KeyMatchResult::Yes : KeyMatchResult::No;
                    }
                );
            }
        } else {
            indexDbi = env.dbi_Event__created_at;
            desc = "CreatedAt";

            cursors.reserve(1);
            cursors.emplace_back(
                std::string(8, '\xFF'),
                MAX_U64,
                [](std::string_view){
                    return KeyMatchResult::Yes;
                }
            );
        }

        initialScanDepth = std::clamp(f.limit / cursors.size(), uint64_t(5), uint64_t(50));
        refillScanDepth = 10 * initialScanDepth;
    }

    // Posting list for one token, as levIds, ascending
    static void readPostings(lmdb::txn &txn, std::string_view token, std::vector<uint64_t> &out) {
        auto cursor = lmdb::cursor::open(txn, env.dbi_SearchIndex);
        std::string_view k = token, v;

        if (!cursor.get(k, v, MDB_SET_KEY)) return;
        if (!cursor.get(k, v, MDB_GET_MULTIPLE)) return;

        do {
            for (size_t i = 0; i + search::postingBytes <= v.size(); i += search::postingBytes) {
                out.push_back(search::decodePosting(v.substr(i, search::postingBytes)));
            }
        } while (cursor.get(k, v, MDB_NEXT_MULTIPLE));
    }

    // Union of posting lists of all tokens that start with prefix, as levIds, ascending and unique
    static void readPrefixPostings(lmdb::txn &txn, std::string_view prefix, std::vector<uint64_t> &out) {
        auto cursor = lmdb::cursor::open(txn, env.dbi_SearchIndex);
        std::string_view k = prefix, v;
        uint64_t numKeys = 0;

        if (!cursor.get(k, v, MDB_SET_RANGE)) return;

        while (k.starts_with(prefix)) {
            if (++numKeys > cfg().search__maxPrefixTokens) break;

            std::string key(k);
            readPostings(txn, key, out);

            k = key;
            if (!cursor.get(k, v, MDB_SET_KEY)) break;
            if (!cursor.get(k, v, MDB_NEXT_NODUP)) break;
        }

        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    void initSearch(lmdb::txn &txn) {
        searchInited = true;

        // Every token list required by any term. Each list is ascending.
        std::vector<std::vector<uint64_t>> lists;

        for (const auto &term : f.search->terms) {
            for (const auto &tok : term.exact) {
                auto &l = lists.emplace_back();
                readPostings(txn, tok, l);
                if (l.empty()) return; // no results
            }

            for (const auto &tok : term.prefix) {
                auto &l = lists.emplace_back();
                readPrefixPostings(txn, tok, l);
                if (l.empty()) return;
            }
        }

        if (lists.empty()) return;

        std::sort(lists.begin(), lists.end(), [](const auto &a, const auto &b){ return a.size() < b.size(); });

        // Intersect, starting from the smallest list
        std::vector<uint64_t> result = std::move(lists[0]);

        for (size_t i = 1; i < lists.size() && result.size(); i++) {
            std::vector<uint64_t> tmp;
            std::set_intersection(result.begin(), result.end(), lists[i].begin(), lists[i].end(), std::back_inserter(tmp));
            result.swap(tmp);
        }

        approxWork += result.size();

        // Newest first. levIds are assigned in insertion order, which approximates created_at order.
        std::reverse(result.begin(), result.end());

        searchCandidates.swap(result);
    }

    bool scanSearch(lmdb::txn &txn, const std::function<bool(uint64_t)> &handleEvent, const std::function<bool(uint64_t)> &doPause) {
        if (!searchInited) initSearch(txn);

        while (searchCursor < searchCandidates.size()) {
            approxWork += 10;
            if (doPause(approxWork)) return false;

            uint64_t levId = searchCandidates[searchCursor++];

            auto view = env.lookup_Event(txn, levId);
            if (!view) continue; // deleted

            PackedEventView packed(view->buf);
            if (!f.doesMatchExceptSearch(packed)) continue; // cheap checks, before loading the event text

            if (++searchVerified > cfg().search__maxCandidates) return true;

            std::string normText;

            bool matched = f.doesMatch(packed, [&]() -> std::string_view {
                std::string_view eventPayload;
                if (!env.dbi_EventPayload.get(txn, lmdb::to_sv<uint64_t>(levId), eventPayload)) return "";
                normText = searchExtractText(getEventJson(txn, searchDecomp, levId, eventPayload));
                return normText;
            });

            if (matched && handleEvent(levId)) return true;
        }

        return true;
    }

    bool scan(lmdb::txn &txn, const std::function<bool(uint64_t)> &handleEvent, const std::function<bool(uint64_t)> &doPause) {
        if (isSearch) return scanSearch(txn, handleEvent, doPause);

        auto cmp = [](auto &a, auto &b){
            return a.created() == b.created() ? a.levId() > b.levId() : a.created() > b.created();
        };

        while (1) {
            approxWork++;
            if (doPause(approxWork)) return false;

            if (nextInitIndex < cursors.size()) {
                approxWork += cursors[nextInitIndex].collect(txn, *this, nextInitIndex, initialScanDepth, eventQueue);
                nextInitIndex++;

                if (nextInitIndex == cursors.size()) {
                    std::sort(eventQueue.begin(), eventQueue.end(), cmp);
                }

                continue;
            } else if (eventQueue.size() == 0) {
                return true;
            }

            auto ev = eventQueue.front();
            eventQueue.pop_front();
            bool doSend = false;
            uint64_t levId = ev.levId();

            if (indexOnly) {
                if (f.doesMatchTimes(ev.created())) doSend = true;
            } else {
                approxWork += 10;
                auto view = env.lookup_Event(txn, levId);
                if (view && f.doesMatch(PackedEventView(view->buf))) doSend = true;
            }

            if (doSend) {
                if (handleEvent(levId)) return true;
            }

            cursors[ev.scanIndex()].outstanding--;

            if (cursors[ev.scanIndex()].outstanding == 0) {
                std::deque<CandidateEvent> moreEvents;
                std::deque<CandidateEvent> newEventQueue;
                approxWork += cursors[ev.scanIndex()].collect(txn, *this, ev.scanIndex(), refillScanDepth, moreEvents);

                std::merge(eventQueue.begin(), eventQueue.end(), moreEvents.begin(), moreEvents.end(), std::back_inserter(newEventQueue), cmp);
                eventQueue.swap(newEventQueue);
            }
        }
    }
};


struct DBQuery : NonCopyable {
    Subscription sub;

    std::unique_ptr<DBScan> scanner;
    size_t filterGroupIndex = 0;
    bool dead = false; // external flag
    flat_hash_set<uint64_t> sentEventsFull;
    flat_hash_set<uint64_t> sentEventsCurr;
    uint64_t lastWorkChecked = 0;

    uint64_t currScanTime = 0;
    uint64_t currScanSaveRestores = 0;
    uint64_t totalTime = 0;
    uint64_t totalWork = 0;

    DBQuery(Subscription &sub) : sub(std::move(sub)) {}
    DBQuery(const tao::json::value &filter, uint64_t maxLimit = MAX_U64) : sub(Subscription(1, ".", NostrFilterGroup(filter, maxLimit))) {}

    // If scan is complete, returns true
    bool process(lmdb::txn &txn, const std::function<void(const Subscription &, uint64_t)> &cb, uint64_t timeBudgetMicroseconds = MAX_U64, bool logMetrics = false) {
        while (filterGroupIndex < sub.filterGroup.size()) {
            const auto &f = sub.filterGroup.filters[filterGroupIndex];

            if (!scanner) scanner = std::make_unique<DBScan>(f);

            uint64_t startTime = hoytech::curr_time_us();

            bool complete = scanner->scan(txn, [&](uint64_t levId){
                if (f.limit == 0) return true;

                // If this event came in after our query began, don't send it. It will be sent after the EOSE.
                if (levId > sub.latestEventId) return false;

                if (sentEventsFull.find(levId) == sentEventsFull.end()) {
                    sentEventsFull.insert(levId);
                    cb(sub, levId);
                }

                sentEventsCurr.insert(levId);
                return sentEventsCurr.size() >= f.limit;
            }, [&](uint64_t approxWork){
                if (approxWork > lastWorkChecked + 2'000) {
                    lastWorkChecked = approxWork;
                    return hoytech::curr_time_us() - startTime > timeBudgetMicroseconds;
                }
                return false;
            });

            currScanTime += hoytech::curr_time_us() - startTime;

            if (!complete) {
                currScanSaveRestores++;
                return false;
            }

            totalTime += currScanTime;
            totalWork += scanner->approxWork;

            if (logMetrics) {
                LI << "[" << sub.connId << "] REQ='" << sub.subId.sv() << "'"
                   << " scan=" << scanner->desc
                   << " indexOnly=" << scanner->indexOnly
                   << " time=" << currScanTime << "us"
                   << " saveRestores=" << currScanSaveRestores
                   << " recsFound=" << sentEventsCurr.size()
                   << " work=" << scanner->approxWork;
                ;
            }

            scanner.reset();
            filterGroupIndex++;
            sentEventsCurr.clear();

            currScanTime = 0;
            currScanSaveRestores = 0;
        }

        if (logMetrics) {
            LI << "[" << sub.connId << "] REQ='" << sub.subId.sv() << "'"
               << " totalTime=" << totalTime << "us"
               << " totalWork=" << totalWork
               << " recsSent=" << sentEventsFull.size()
            ;
        }

        return true;
    }
};


inline void foreachByFilter(lmdb::txn &txn, const tao::json::value &filter, const std::function<void(uint64_t)> &cb) {
    DBQuery query(filter);

    query.process(txn, [&](const auto &, uint64_t levId){
        cb(levId);
    });
}
