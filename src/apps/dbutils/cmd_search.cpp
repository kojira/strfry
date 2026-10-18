#include <iostream>

#include <docopt.h>
#include "golpe.h"

#include "events.h"
#include "SearchIndex.h"


static const char USAGE[] =
R"(
    Usage:
      search reindex [--batch=<batch>] [--kinds=<kinds>]
      search stats
      search tokens <text>

    reindex: drops the search index and rebuilds it from all stored events.
             With --kinds (comma-separated), the index is kept and only stored events
             of those kinds are (re-)indexed, read through the kind index. Use this
             after adding kinds to search.kinds.
    stats:   prints the size of the search index
    tokens:  prints the tokens that would be indexed for <text>
)";


void cmd_search(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    if (args["tokens"].asBool()) {
        auto norm = search::normalise(args["<text>"].asString());
        for (const auto &tok : search::indexTokens(norm)) std::cout << tok << "\n";
        return;
    }

    if (args["stats"].asBool()) {
        auto txn = env.txn_ro();
        auto stat = env.dbi_SearchIndex.stat(txn);

        uint64_t pages = stat.ms_branch_pages + stat.ms_leaf_pages + stat.ms_overflow_pages;
        uint64_t numKeys = 0;

        {
            auto cursor = lmdb::cursor::open(txn, env.dbi_SearchIndex);
            std::string_view k, v;
            if (cursor.get(k, v, MDB_FIRST)) {
                do numKeys++; while (cursor.get(k, v, MDB_NEXT_NODUP));
            }
        }

        std::cout << "enabled:  " << (searchEnabled() ? "yes" : "no") << "\n";
        std::cout << "tokens:   " << numKeys << "\n";
        std::cout << "postings: " << stat.ms_entries << "\n";
        std::cout << "size:     " << renderSize(pages * stat.ms_psize) << "\n";
        return;
    }

    if (args["reindex"].asBool()) {
        if (!searchEnabled()) throw herr("search is not enabled: set search.enabled = true in the config");

        uint64_t batchSize = 10'000;
        if (args["--batch"]) batchSize = args["--batch"].asLong();

        Decompressor decomp;

        if (args["--kinds"]) {
            flat_hash_set<uint64_t> kindSet;
            parseCommaSeparatedKinds(args["--kinds"].asString(), kindSet);
            std::vector<uint64_t> kinds(kindSet.begin(), kindSet.end());
            std::sort(kinds.begin(), kinds.end());

            for (auto kind : kinds) {
                if (!searchIsIndexedKind(kind)) throw herr("kind ", kind, " is not in search.kinds");
            }

            uint64_t numIndexed = 0;
            uint64_t startTime = hoytech::curr_time_us();

            for (auto kind : kinds) {
                uint64_t resumeTime = 0, resumeLevId = 0;

                while (1) {
                    auto txn = env.txn_rw();
                    uint64_t inBatch = 0;
                    bool done = true;

                    env.generic_foreachFull(txn, env.dbi_Event__kind, makeKey_Uint64Uint64(kind, resumeTime), lmdb::to_sv<uint64_t>(resumeLevId), [&](auto k, auto v) {
                        ParsedKey_Uint64Uint64 parsedKey(k);
                        if (parsedKey.n1 != kind) return false;

                        uint64_t levId = lmdb::from_sv<uint64_t>(v);

                        if (inBatch >= batchSize) {
                            resumeTime = parsedKey.n2;
                            resumeLevId = levId;
                            done = false;
                            return false;
                        }

                        inBatch++;

                        std::string_view eventPayload;
                        if (!env.dbi_EventPayload.get(txn, lmdb::to_sv<uint64_t>(levId), eventPayload)) return true;

                        searchIndexEvent(txn, levId, kind, getEventJson(txn, decomp, levId, eventPayload));
                        numIndexed++;

                        return true;
                    });

                    txn.commit();

                    LI << "search reindex: kind " << kind << ", indexed " << numIndexed;

                    if (done) break;
                }
            }

            LI << "search reindex complete in " << ((hoytech::curr_time_us() - startTime) / 1'000'000) << "s";
            return;
        }

        {
            auto txn = env.txn_rw();
            env.dbi_SearchIndex.drop(txn, false);
            txn.commit();
        }

        uint64_t startLevId = 0, numEvents = 0, numIndexed = 0;
        uint64_t startTime = hoytech::curr_time_us();

        while (1) {
            auto txn = env.txn_rw();
            uint64_t inBatch = 0;
            bool done = true;

            env.foreach_Event(txn, [&](auto &ev){
                if (inBatch >= batchSize) {
                    startLevId = ev.primaryKeyId;
                    done = false;
                    return false;
                }

                inBatch++;
                numEvents++;

                uint64_t kind = PackedEventView(ev.buf).kind();
                if (!searchIsIndexedKind(kind)) return true;

                std::string_view eventPayload;
                if (!env.dbi_EventPayload.get(txn, lmdb::to_sv<uint64_t>(ev.primaryKeyId), eventPayload)) return true;

                searchIndexEvent(txn, ev.primaryKeyId, kind, getEventJson(txn, decomp, ev.primaryKeyId, eventPayload));
                numIndexed++;

                return true;
            }, false, startLevId);

            txn.commit();

            LI << "search reindex: scanned " << numEvents << " events, indexed " << numIndexed;

            if (done) break;
        }

        LI << "search reindex complete in " << ((hoytech::curr_time_us() - startTime) / 1'000'000) << "s";
    }
}
