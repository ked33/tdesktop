#include "core/message_folding_matcher.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>

namespace {

void Require(bool condition, const char *name) {
	if (!condition) {
		std::cerr << name << '\n';
		std::exit(1);
	}
}

void CheckWords(std::vector<std::u16string> words) {
	const auto matcher = MessageFolding::KeywordMatcher(words);
	auto random = std::mt19937(42);
	for (auto sample = 0; sample != 2000; ++sample) {
		auto text = std::u16string();
		for (auto i = 0; i != sample % 137; ++i) {
			text.push_back(char16_t(u'a' + random() % 6));
		}
		const auto expected = std::any_of(words.begin(), words.end(),
			[&](const auto &word) {
				return !word.empty() && text.find(word) != text.npos;
			});
		Require(matcher.matches(text) == expected, "substring equivalence");
	}
}

void Benchmark(std::size_t count) {
	auto words = std::vector<std::u16string>();
	for (auto i = std::size_t(0); i != count; ++i) {
		auto word = std::u16string(u"pattern");
		for (const auto ch : std::to_string(i)) {
			word.push_back(char16_t(ch));
		}
		words.push_back(std::move(word));
	}
	const auto begin = std::chrono::steady_clock::now();
	const auto matcher = MessageFolding::KeywordMatcher(std::move(words));
	const auto compiled = std::chrono::steady_clock::now();
	const auto text = std::u16string(4096, u'x');
	auto hits = 0;
	for (auto i = 0; i != 2000; ++i) {
		hits += matcher.matches(text) ? 1 : 0;
	}
	const auto finished = std::chrono::steady_clock::now();
	Require(hits == 0, "negative workload");
	std::cout << count << " keywords: compile "
		<< std::chrono::duration_cast<std::chrono::microseconds>(
			compiled - begin).count() << " us, 2000 x 4096 units "
		<< std::chrono::duration_cast<std::chrono::microseconds>(
			finished - compiled).count() << " us\n";
}

} // namespace

int main() {
	using MessageFolding::KeywordMatcher;
	using MessageFolding::ParseUserId;
	Require(!KeywordMatcher().matches(u"anything"), "empty rules");
	Require(!KeywordMatcher({ u"" }).matches(u"anything"), "empty keyword");
	Require(KeywordMatcher({ u"vpn", u"\u80a1\u7968" }).matches(
		u"\u80a1\u7968 information"), "unicode substring");
	Require(!KeywordMatcher({ u"vpn" }).matches(u""), "empty message");
	Require(KeywordMatcher({ u"apple pay" }).matches(
		u"text about apple pay"), "screenshot second message");
	Require(!KeywordMatcher({ u"apple pay" }).matches(
		u"first message"), "independent messages");
	Require(KeywordMatcher({ u".*" }).matches(u"literal .*"), "literal pattern");
	Require(!KeywordMatcher({ u".*" }).matches(u"abc"), "not a regex");
	Require(KeywordMatcher({ u"\U0001F600" }).matches(
		u"text \U0001F600 text"), "surrogate pair");
	CheckWords({ u"", u"aab", u"bc", u"bc", u"efabc" });
	auto words = std::vector<std::u16string>{
		u"aab", u"abcd", u"bc", u"cdef", u"defa", u"efabc",
		u"fab", u"abcdef", u"aabcdef", u"bbbc", u"cccd", u"ddde",
		u"eeef", u"fffa", u"abcde", u"bcdef", u"abcdefa", u"",
	};
	CheckWords(words);
	words.push_back(u"\u80a1\u7968");
	words.push_back(u"\U0001F600");
	Require(KeywordMatcher(words).matches(u"\u80a1\u7968"), "unicode automaton");
	Require(KeywordMatcher(words).matches(u"x\U0001F600"), "surrogate automaton");
	Require(ParseUserId(u"123456789") == 123456789, "valid ID");
	Require(ParseUserId(u"000123") == 123, "leading zeroes");
	Require(ParseUserId(u"281474976710655") == 0xFFFFFFFFFFFFULL, "maximum ID");
	for (const auto value : { u"", u"0", u"-1", u"+1", u"1.0", u"1e3",
		u" 123", u"123x", u"\uFF11", u"281474976710656",
		u"184467440737095516160" }) {
		Require(!ParseUserId(value), "invalid ID");
	}
	for (const auto count : { 1, 16, 128, 1024 }) {
		Benchmark(std::size_t(count));
	}
	std::cout << "Message folding checks passed.\n";
}
