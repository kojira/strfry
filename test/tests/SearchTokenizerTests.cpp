#include "SearchTokenizer.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

std::string join(const std::vector<std::string> &v) {
    std::string out;
    for (const auto &s : v) {
        if (out.size()) out += " ";
        out += s;
    }
    return out;
}

void expectTokens(std::string_view name, std::string_view text, std::string_view expected) {
    auto got = join(search::indexTokens(search::normalise(text)));
    if (got != expected) {
        std::cerr << name << ": expected [" << expected << "] got [" << got << "]\n";
        failures++;
    }
}

void expectMatch(std::string_view name, std::string_view query, std::string_view text, bool expected) {
    search::SearchQuery q(query);
    bool got = q.matches(search::normalise(text));
    if (got != expected) {
        std::cerr << name << ": expected " << expected << " got " << got << "\n";
        failures++;
    }
}

void expectQueryFailure(std::string_view name, std::string_view query) {
    try {
        search::SearchQuery q(query);
        std::cerr << name << ": expected failure\n";
        failures++;
    } catch (const std::exception &) {
    }
}

} // namespace

int main() {
    expectTokens("ascii words", "Hello, World! a bc", "bc hello world");
    expectTokens("fullwidth folded", "ＮＯＳＴＲ", "nostr");
    expectTokens("cjk bigrams", "東京都", "京都 東京 都");
    expectTokens("cjk single", "猫", "猫");
    expectTokens("mixed", "猫cat", "cat 猫");
    expectTokens("urls skipped", "見て https://example.com/東京 nostr:npub1xyz", "て 見て");
    expectTokens("url glued to cjk", "見てhttps://example.com/x", "て 見て");
    expectTokens("long word skipped", std::string(41, 'a'), "");
    expectTokens("invalid utf8", "ab\xff" "cd", "ab cd");
    expectTokens("hangul", "안녕하세요", "녕하 세요 안녕 요 하세");

    expectMatch("substring verified", "東京都", "東京と京都", false);
    expectMatch("substring ok", "東京都", "東京都の猫", true);
    expectMatch("case folded", "NOSTR", "nostr is fun", true);
    expectMatch("all terms required", "東京 猫", "東京の犬", false);
    expectMatch("extensions ignored", "東京 lang:ja", "東京", true);

    {
        search::SearchQuery q("猫 nostr");
        if (q.terms.size() != 2 || q.terms[0].prefix.size() != 1 || q.terms[1].exact.size() != 1) {
            std::cerr << "query terms: unexpected structure\n";
            failures++;
        }
    }

    expectQueryFailure("no searchable terms", "!!!");
    expectQueryFailure("too long", std::string(513, 'a'));
    expectQueryFailure("too many terms", "aa bb cc dd ee ff gg hh ii");

    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "SearchTokenizer tests passed\n";
    return EXIT_SUCCESS;
}
