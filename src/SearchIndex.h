#pragma once

// NIP-50 full-text search index.
//
// Storage: one LMDB table (SearchIndex), MDB_DUPSORT | MDB_DUPFIXED.
//   key:   a token (see SearchTokenizer.h)
//   value: levId of an event containing that token, as a 5-byte big-endian integer
//
// There is exactly one posting per (token, event) and no positional information, which
// keeps the index small. Phrase/substring semantics are recovered at query time by
// verifying each candidate against the event's text (see DBScan).

#include <string>
#include <string_view>
#include <vector>

#include "golpe.h"

#include "SearchTokenizer.h"


namespace search {

static constexpr size_t postingBytes = 5;
static constexpr uint64_t maxPostingLevId = (uint64_t(1) << (8 * postingBytes)) - 1;

inline std::string encodePosting(uint64_t levId) {
    if (levId > maxPostingLevId) throw herr("levId too large for search index");
    std::string out(postingBytes, '\0');
    for (size_t i = 0; i < postingBytes; i++) out[postingBytes - 1 - i] = (char)((levId >> (8 * i)) & 0xFF);
    return out;
}

inline uint64_t decodePosting(std::string_view v) {
    if (v.size() != postingBytes) throw herr("unexpected search posting size");
    uint64_t out = 0;
    for (size_t i = 0; i < postingBytes; i++) out = (out << 8) | (uint8_t)v[i];
    return out;
}

}


bool searchEnabled();
bool searchIsIndexedKind(uint64_t kind);

// Returns the normalised searchable text of an event (content, selected tags, profile fields)
std::string searchExtractText(const tao::json::value &eventJson);
std::string searchExtractText(std::string_view eventJsonStr);

// Add/remove the postings of an event. No-ops if search is disabled or kind is not indexed.
void searchIndexEvent(lmdb::txn &txn, uint64_t levId, uint64_t kind, std::string_view eventJsonStr);
void searchUnindexEvent(lmdb::txn &txn, uint64_t levId, uint64_t kind, std::string_view eventJsonStr);
