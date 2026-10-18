#pragma once

// Tokenizer for the NIP-50 search index.
//
// Design goals:
//   * Work for both space-delimited scripts (English, etc) and scripts that are
//     written without spaces (Japanese, Chinese, Korean).
//   * Keep the index small: one posting per (token, event), no positions.
//
// Rules (applied to normalised text, see normalise()):
//   * Runs of "word" characters (ASCII alphanumerics, Latin/Greek/Cyrillic/etc letters)
//     become one token each, if they are 2 or more characters and at most maxWordBytes.
//   * Runs of CJK characters (Hiragana, Katakana, Han, Hangul) are split into
//     overlapping bigrams: "東京都" -> "東京", "京都".
//     The final character of each run is also emitted on its own ("都"), so that
//     every character occurrence is the first character of some token. This lets a
//     single-character query be answered with a prefix scan.
//   * URLs and nostr: URIs are skipped: they are long, unique, and rarely searched.
//
// Bigrams can produce false positives ("東京都" also matches "東京と京都" at the index
// level), so every candidate is verified against the event text before being returned.

#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <functional>
#include <cstdint>


namespace search {

static constexpr size_t maxWordBytes = 40;
static constexpr size_t maxQueryBytes = 512;
static constexpr size_t maxQueryTerms = 8;


enum class TokenType {
    Word,     // space-delimited word
    Bigram,   // two CJK characters
    Single,   // CJK run consisting of exactly one character
    RunEnd,   // last character of a CJK run of 2 or more characters
};


inline bool isCJK(uint32_t cp) {
    return (cp >= 0x3005 && cp <= 0x3007)   // 々〆〇
        || (cp >= 0x3040 && cp <= 0x30FF)   // Hiragana, Katakana
        || (cp >= 0x31F0 && cp <= 0x31FF)   // Katakana phonetic extensions
        || (cp >= 0x3400 && cp <= 0x4DBF)   // CJK extension A
        || (cp >= 0x4E00 && cp <= 0x9FFF)   // CJK unified ideographs
        || (cp >= 0xAC00 && cp <= 0xD7AF)   // Hangul syllables
        || (cp >= 0xF900 && cp <= 0xFAFF)   // CJK compatibility ideographs
        || (cp >= 0xFF66 && cp <= 0xFF9F)   // Halfwidth Katakana
        || (cp >= 0x20000 && cp <= 0x3FFFF); // CJK extensions B+
}

inline bool isWordChar(uint32_t cp) {
    if (cp < 0x80) return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
    if (cp == 0xD7 || cp == 0xF7) return false; // multiplication/division signs
    return cp >= 0xC0 && cp < 0x2000; // Latin, Greek, Cyrillic, Armenian, Hebrew, Arabic, Indic, ...
}


// Decodes one UTF-8 code point starting at s[i]. Returns its length in bytes, or 0 if invalid.
inline size_t decodeUtf8(std::string_view s, size_t i, uint32_t &cp) {
    unsigned char c = s[i];
    size_t len;

    if (c < 0x80) { cp = c; return 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else return 0;

    if (i + len > s.size()) return 0;

    for (size_t j = 1; j < len; j++) {
        unsigned char cc = s[i + j];
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3F);
    }

    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return 0;

    return len;
}

inline void encodeUtf8(uint32_t cp, std::string &out) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}


// Normalisation applied to both indexed text and queries:
//   * ASCII is lower-cased
//   * Fullwidth ASCII (U+FF01..U+FF5E) is folded to ASCII
//   * Ideographic space (U+3000) becomes an ASCII space
//   * Invalid UTF-8 bytes become ASCII spaces

inline std::string normalise(std::string_view in) {
    std::string out;
    out.reserve(in.size());

    for (size_t i = 0; i < in.size(); ) {
        uint32_t cp;
        size_t len = decodeUtf8(in, i, cp);

        if (len == 0) {
            out += ' ';
            i++;
            continue;
        }

        i += len;

        if (cp >= 0xFF01 && cp <= 0xFF5E) cp -= 0xFEE0;
        else if (cp == 0x3000) cp = ' ';

        if (cp >= 'A' && cp <= 'Z') cp += 'a' - 'A';

        encodeUtf8(cp, out);
    }

    return out;
}


inline bool isUriStart(std::string_view s) {
    static const char *prefixes[] = { "http://", "https://", "wss://", "ws://", "nostr:", "lightning:" };
    for (auto *p : prefixes) {
        if (s.starts_with(p)) return true;
    }
    return false;
}

inline bool isAsciiSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}


// Calls cb(type, token) for every token in already-normalised text. Tokens may repeat.

inline void tokenise(std::string_view text, const std::function<void(TokenType, std::string_view)> &cb) {
    size_t wordStart = 0, wordChars = 0;
    std::vector<std::pair<size_t, size_t>> run; // CJK run: (offset, length) of each char

    auto flushWord = [&](size_t end){
        if (wordChars >= 2 && end - wordStart <= maxWordBytes) cb(TokenType::Word, text.substr(wordStart, end - wordStart));
        wordChars = 0;
    };

    auto flushRun = [&]{
        if (run.size() == 1) {
            cb(TokenType::Single, text.substr(run[0].first, run[0].second));
        } else if (run.size() > 1) {
            for (size_t i = 0; i + 1 < run.size(); i++) {
                cb(TokenType::Bigram, text.substr(run[i].first, run[i].second + run[i + 1].second));
            }
            cb(TokenType::RunEnd, text.substr(run.back().first, run.back().second));
        }
        run.clear();
    };

    for (size_t i = 0; i < text.size(); ) {
        if (wordChars == 0 && run.empty() && isUriStart(text.substr(i))) {
            while (i < text.size() && !isAsciiSpace(text[i])) i++;
            continue;
        }

        uint32_t cp;
        size_t len = decodeUtf8(text, i, cp);
        if (len == 0) { cp = ' '; len = 1; }

        if (isWordChar(cp)) {
            flushRun();
            if (wordChars == 0) {
                // A URI can follow a CJK character directly, eg "見てhttps://..."
                if (isUriStart(text.substr(i))) {
                    while (i < text.size() && !isAsciiSpace(text[i])) i++;
                    continue;
                }
                wordStart = i;
            }
            wordChars++;
        } else if (isCJK(cp)) {
            flushWord(i);
            run.emplace_back(i, len);
        } else {
            flushWord(i);
            flushRun();
        }

        i += len;
    }

    flushWord(text.size());
    flushRun();
}


// Distinct tokens to be stored in the index for a (normalised) document.

inline std::vector<std::string> indexTokens(std::string_view normText) {
    std::vector<std::string> out;

    tokenise(normText, [&](TokenType, std::string_view tok){
        out.emplace_back(tok);
    });

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());

    return out;
}


struct SearchQuery {
    struct Term {
        std::string text; // normalised, verified as a substring of each result
        std::vector<std::string> exact; // all of these tokens must be present in the index
        std::vector<std::string> prefix; // a token starting with each of these must be present
    };

    std::string raw;
    std::vector<Term> terms;

    SearchQuery() {}

    explicit SearchQuery(std::string_view q) : raw(q) {
        if (q.size() > maxQueryBytes) throw std::runtime_error("search query too long");

        std::string norm = normalise(q);
        std::string_view sv = norm;

        while (sv.size()) {
            while (sv.size() && isAsciiSpace(sv[0])) sv = sv.substr(1);
            size_t end = 0;
            while (end < sv.size() && !isAsciiSpace(sv[end])) end++;
            std::string_view word = sv.substr(0, end);
            sv = sv.substr(end);
            if (word.empty()) continue;

            // NIP-50 extensions ("key:value") are not supported, and are ignored as recommended
            if (isExtension(word)) continue;

            Term t;
            t.text = std::string(word);

            tokenise(word, [&](TokenType type, std::string_view tok){
                if (type == TokenType::Word || type == TokenType::Bigram) t.exact.emplace_back(tok);
                else if (type == TokenType::Single) t.prefix.emplace_back(tok);
            });

            dedup(t.exact);
            dedup(t.prefix);

            if (std::any_of(terms.begin(), terms.end(), [&](const Term &o){ return o.text == t.text; })) continue;
            terms.emplace_back(std::move(t));
        }

        if (terms.size() > maxQueryTerms) throw std::runtime_error("too many search terms");

        bool anyIndexed = std::any_of(terms.begin(), terms.end(), [](const Term &t){ return t.exact.size() || t.prefix.size(); });
        if (!anyIndexed) throw std::runtime_error("search query has no searchable terms");
    }

    // text must already be normalised
    bool matches(std::string_view normText) const {
        for (const auto &t : terms) {
            if (normText.find(t.text) == std::string_view::npos) return false;
        }
        return true;
    }

  private:
    static bool isExtension(std::string_view word) {
        auto colon = word.find(':');
        if (colon == std::string_view::npos || colon == 0 || colon + 1 == word.size()) return false;
        for (size_t i = 0; i < colon; i++) {
            char c = word[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
        }
        return true;
    }

    static void dedup(std::vector<std::string> &v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
};


}
