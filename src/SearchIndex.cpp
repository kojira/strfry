#include <mutex>

#include "golpe.h"

#include "SearchIndex.h"


bool searchEnabled() {
    return cfg().search__enabled;
}

bool searchIsIndexedKind(uint64_t kind) {
    static std::once_flag once;
    static flat_hash_set<uint64_t> kinds;

    std::call_once(once, []{
        parseCommaSeparatedKinds(cfg().search__kinds, kinds);
    });

    return kinds.contains(kind);
}


static void appendText(std::string &out, std::string_view s) {
    if (s.empty()) return;
    if (out.size()) out += '\n';
    out += s;
}

std::string searchExtractText(const tao::json::value &eventJson) {
    std::string text;

    const auto *kindV = eventJson.find("kind");
    const auto *contentV = eventJson.find("content");
    uint64_t kind = kindV && kindV->is_unsigned() ? kindV->get_unsigned() : 0;

    if (contentV && contentV->is_string()) {
        if (kind == 0) {
            // Profile metadata: index selected fields of the JSON-encoded content
            try {
                auto meta = tao::json::from_string(contentV->get_string());
                if (meta.is_object()) {
                    for (const char *field : { "name", "display_name", "displayName", "about", "nip05" }) {
                        const auto *f = meta.find(field);
                        if (f && f->is_string()) appendText(text, f->get_string());
                    }
                }
            } catch (std::exception &) {
                // Invalid profile JSON: nothing to index
            }
        } else {
            appendText(text, contentV->get_string());
        }
    }

    const auto *tagsV = eventJson.find("tags");
    if (tagsV && tagsV->is_array()) {
        for (const auto &tag : tagsV->get_array()) {
            if (!tag.is_array() || tag.get_array().size() < 2) continue;
            const auto &name = tag.get_array()[0];
            const auto &val = tag.get_array()[1];
            if (!name.is_string() || !val.is_string()) continue;
            const auto &n = name.get_string();
            if (n == "title" || n == "subject" || n == "summary") appendText(text, val.get_string());
        }
    }

    return search::normalise(text);
}

std::string searchExtractText(std::string_view eventJsonStr) {
    return searchExtractText(tao::json::from_string(eventJsonStr));
}


static void forEachPosting(uint64_t levId, uint64_t kind, std::string_view eventJsonStr, const std::function<void(std::string_view, std::string_view)> &cb) {
    if (!searchEnabled() || !searchIsIndexedKind(kind)) return;

    auto tokens = search::indexTokens(searchExtractText(eventJsonStr));
    if (tokens.empty()) return;

    auto posting = search::encodePosting(levId);

    for (const auto &tok : tokens) cb(tok, posting);
}

void searchIndexEvent(lmdb::txn &txn, uint64_t levId, uint64_t kind, std::string_view eventJsonStr) {
    forEachPosting(levId, kind, eventJsonStr, [&](std::string_view tok, std::string_view posting){
        env.dbi_SearchIndex.put(txn, tok, posting, MDB_NODUPDATA);
    });
}

void searchUnindexEvent(lmdb::txn &txn, uint64_t levId, uint64_t kind, std::string_view eventJsonStr) {
    forEachPosting(levId, kind, eventJsonStr, [&](std::string_view tok, std::string_view posting){
        env.dbi_SearchIndex.del(txn, tok, posting);
    });
}
