#include "api/api_porn_search_policy.h"
#include "chat_helpers/hashtag_suggestions.h"
#include "dialogs/dialogs_search_match.h"

#include "ui/text/text_entity.h"
#include "ui/emoji_config.h"

#include <QtCore/QElapsedTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>

#include <crl/crl_on_main.h>
#include <rpl/never.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using namespace ChatHelpers;
using Tags = QList<QPair<QString, ushort>>;

auto Checks = 0;
auto Normalizations = 0;

void Check(bool condition, const char *name) {
	++Checks;
	if (!condition) {
		std::cerr << "FAILED: " << name << '\n';
		std::exit(1);
	}
}

QString CountedNormalize(const QString &text) {
	++Normalizations;
	return TextUtilities::RemoveAccents(text);
}

void CheckMessageSearch() {
	using namespace Dialogs;
	Check(SearchTextMatches(u"#示例 #demo #测试 这是一段很长的测试文本", u"#示例"),
		"whole hashtag retained");
	Check(SearchTextMatches(u"家电节按实际搜索测试覅偶尔放数据对象", u"搜索测试"),
		"Chinese infix needs no word boundary");
	Check(!SearchTextMatches(u"如何无网络用局域网打dota2\nVia OvO", u"小远ovo"),
		"server partial match excluded");
	Check(SearchTextMatches(u"前文小远OvO后文", u"小远ovo"),
		"full query ignores case");
	Check(!SearchTextMatches(u"搜索 测试", u"搜索测试"), "spaces are literal");
	Check(!SearchTextMatches(u"#示例", u"示_例"), "punctuation is literal");
	Check(SearchTextMatches(u"任意消息", u""), "empty query keeps sender-only results");
	auto text = TextWithEntities{ u"🚢#示例 #示例"_q };
	Check(HighlightSearchText(text, u"#示例") == 2, "UTF-16 offset after emoji");
	Check(text.entities.size() == 2 && text.entities[0].length() == 3
		&& text.entities[1].offset() == 6, "all repeated hashtags highlighted");
	Check(text.entities[0].data() == QString(QChar(1)), "dedicated highlight color");
	auto overlap = TextWithEntities{ u"测试测试测试"_q };
	Check(HighlightSearchText(overlap, u"测试测试") == 0, "overlap first offset");
	Check(overlap.entities.size() == 1 && overlap.entities[0].length() == 6,
		"overlapping matches are merged");
	auto marked = TextWithEntities{ u"ab cd ef"_q, {
		EntityInText(EntityType::Colorized, 0, 8),
		EntityInText(EntityType::Spoiler, 0, 8),
	} };
	Check(HighlightSearchText(marked, u"cd") == 3, "match inside existing color");
	Check(marked.entities.size() == 4, "existing color split around match");
	Check(std::any_of(marked.entities.begin(), marked.entities.end(), [](const auto &entity) {
		return entity.type() == EntityType::Spoiler && entity.length() == 8;
	}), "spoiler preserved");
	auto sender = TextWithEntities{ u"搜索测试: 正文搜索测试"_q };
	Check(HighlightSearchText(sender, u"搜索测试", 6) == 8,
		"sender label excluded from matching");
	auto longText = TextWithEntities{ QString(100000, QChar('x')) + u"搜索测试"_q };
	Check(HighlightSearchText(longText, u"搜索测试") == 100000,
		"match offsets do not truncate to uint16");
}

void CheckSearchQueryRules() {
	using namespace Dialogs;
	struct MatchCase {
		QStringView text;
		QStringView query;
		bool matches = false;
		const char *name = nullptr;
	};
	const auto cases = std::vector<MatchCase>{
		{ u"测试demo", u"测试 demo", true,
			"Chinese and English keywords can share adjacent text" },
		{ u"#测试demo", u"#测试 demo", false,
			"matched hashtag body cannot supply another partial word" },
		{ u"demo_1", u"demo", true,
			"underscore delimits the end of a word" },
		{ u"_demo", u"demo", true,
			"underscore delimits the start of a word" },
		{ u"#测试demo", u"#测试", true,
			"hashtag query retains literal substring matching" },
		{ u"#测试_1", u"#测试", true,
			"hashtag query permits an underscore suffix" },
		{ u"x#测试", u"#测试", true,
			"hashtag query does not require a left word boundary" },
		{ u"1️⃣", u"1", true,
			"plain digit matches the corresponding complete keycap emoji" },
		{ u"demo🚀", u"🚀", true,
			"emoji-only keyword does not require a Latin word boundary" },
		{ u"demo-test", u"demo test", true,
			"hyphen separates two message words" },
		{ u"demo test", u"demo-test", true,
			"hyphen in query separates two AND keywords" },
		{ u"café", u"cafe", true,
			"precomposed accents are folded for matching" },
		{ u"ＤＥＭＯ", u"demo", true,
			"fullwidth letters are folded for matching" },
		{ u"#测试demo", u"demo", true,
			"unrequested hashtag body can supply a plain word" },
		{ u"1", u"1️⃣", true,
			"keycap query matches the corresponding plain digit" },
		{ u"dsfsadjfasdlkfuiej demo 测试", u"测试 demo", true,
			"unordered Chinese and English example" },
		{ u"dsfsadjfasdlkfuiej demo #测试", u"#测试 demo", true,
			"unordered hashtag and English example" },
		{ u"#demo dsfsadjfasdlkfuiej  #测试", u"#测试 #demo", true,
			"unordered hashtags example" },
		{ u"demox fdsfkddsa #测试", u"#测试 demo", false,
			"English suffix excludes a multi-keyword result" },
		{ u"demo #其他", u"#测试 demo", false,
			"every keyword must occur in the same message" },
		{ u"#测试", u"#测试 demo", false,
			"matching hashtag cannot replace missing word" },
		{ u"demox xdemo demo123 1demo", u"demo", false,
			"Latin letters and plain digits extend words" },
		{ u"demo\u0301", u"demo", true,
			"trailing combining accent is folded with the matched word" },
		{ u"\U0001D400demo", u"demo", false,
			"supplementary Unicode letter blocks the left boundary" },
		{ u"demo\U0001D400", u"demo", false,
			"supplementary Unicode letter blocks the right boundary" },
		{ u"(demo)", u"demo", true, "parentheses delimit a word" },
		{ u"demo-test", u"demo", true, "hyphen delimits a word" },
		{ u"demo🚀", u"demo", true, "emoji delimits a word" },
		{ u"demo❤️", u"demo", true,
			"variation-selector emoji delimits the right boundary" },
		{ u"❤️demo", u"demo", true,
			"variation-selector emoji delimits the left boundary" },
		{ u"demo1️⃣", u"demo", true,
			"keycap digit is an emoji boundary rather than a word suffix" },
		{ u"1️⃣demo", u"demo", true,
			"keycap combining mark is an emoji boundary" },
		{ u"👩‍💻demo", u"demo", true,
			"joined emoji delimits a word" },
		{ u"👍🏽demo", u"demo", true,
			"skin-tone emoji delimits a word" },
		{ u"#测试123 #测试扩展 x#测试", u"#测试", true,
			"Chinese hashtag query permits literal prefix and infix matches" },
		{ u"#demo123 #demox", u"#demo", true,
			"English hashtag query permits literal prefix matches" },
		{ u"测试 demo", u"#测试 demo", false,
			"hash remains required input content" },
		{ u"❤️#测试", u"#测试", true,
			"emoji delimits a complete hashtag" },
		{ u"(#测试),demo!", u"#测试 demo", true,
			"punctuation separates both keywords without whitespace" },
		{ u"#demo", u"demo", true,
			"plain word may match the word in a hashtag" },
		{ u"demo", u"#demo", false,
			"hashtag cannot match a plain word" },
		{ u"demox … demo … #测试", u"#测试 demo", true,
			"invalid first occurrence does not hide a later complete word" },
		{ u"这是测试消息 测试扩展", u"测试", true,
			"ordinary Chinese words retain substring matching" },
		{ u"前文小远OvO后文", u"小远ovo", true,
			"mixed Chinese word retains case-insensitive substring matching" },
		{ u"x\U00020000y", u"\U00020000", true,
			"supplementary Han character uses Chinese substring matching" },
		{ u"#测试 DEMO", u"\t#测试\u00A0demo\u202F DEMO \u3000", true,
			"Unicode whitespace and case-insensitive duplicate keywords" },
		{ u"#测试\r\ndemo", u"  demo \t #测试 \n", true,
			"line breaks and surrounding query whitespace are accepted" },
		{ u"demo", u"demo DEMO demo", true,
			"repeated keywords do not require repeated occurrences" },
		{ u"任意消息", u" \t\r\n\u3000", true,
			"whitespace-only query preserves unfiltered results" },
		{ u"", u"demo", false, "nonempty query cannot match empty text" },
		{ u"demo", u"demo❤️", false,
			"emoji in query cannot be dropped" },
		{ u"demo❤️", u"demo❤️", true,
			"complete emoji in query is literal content" },
		{ u"demo1️⃣", u"1", true,
			"keycap digit can match beside a Latin word" },
		{ u"demo1", u"1", false,
			"plain digit query cannot match a Latin word suffix" },
		{ u"demo1", u"1️⃣", false,
			"keycap query uses the same numeric word boundary as a plain digit" },
		{ u"demo1️⃣", u"1️⃣", true,
			"keycap query uses the complete message emoji as a word boundary" },
		{ u"1", u"1 1️⃣", true,
			"equivalent digit and keycap keywords need only one occurrence" },
		{ u"demo1", u"1 1️⃣", false,
			"equivalent numeric keywords retain the plain text suffix rejection" },
		{ u"demo1️⃣", u"1 1️⃣", true,
			"equivalent numeric keywords share one complete message keycap" },
		{ u"❤️", u"❤", false,
			"non-keycap emoji cannot discard a required variation selector" },
		{ u"❤", u"❤️", false,
			"non-keycap emoji query retains its variation selector" },
		{ u"demo❤️", u"demo❤", false,
			"query cannot end within an emoji" },
		{ u"👩‍💻", u"👩", false,
			"query cannot select part of a joined emoji" },
		{ u"这是测试❤️消息", u"测试❤", false,
			"Chinese substring cannot truncate an emoji" },
		{ u"demo", u"\"demo\"", false,
			"double quotes remain literal characters" },
		{ u"\"demo\"", u"\"demo\"", true,
			"literal quoted keyword can match literal quoted text" },
		{ u"\"测试 filler demo\"", u"\"测试 demo\"", true,
			"quotes do not introduce continuous phrase matching" },
		{ u"cafe", u"café", true,
			"accent folding is symmetric for the query" },
		{ u"cafe\u0301", u"cafe", true,
			"decomposed accent folds to its plain word" },
		{ u"cafe", u"cafe\u0301", true,
			"decomposed accent in query folds to its plain word" },
		{ u"demo", u"ＤＥＭＯ", true,
			"fullwidth folding is symmetric for the query" },
		{ u"＃测试＿1", u"#测试", true,
			"fullwidth hash and underscore are folded in message text" },
		{ u"#测试_1", u"＃测试", true,
			"fullwidth hash is folded in query text" },
		{ u"test demo", u"demo－test", true,
			"fullwidth hyphen in query separates unordered AND keywords" },
		{ u"demo－test", u"demo test", true,
			"fullwidth hyphen separates two message words" },
		{ u"demo＿1", u"demo", true,
			"fullwidth underscore delimits a word after folding" },
		{ u"1demo", u"1_demo", false,
			"underscore in query remains literal required content" },
		{ u"1_demo", u"1_demo", true,
			"underscore is not a query keyword separator" },
		{ u"#测试demo demo", u"#测试 demo", true,
			"standalone word can satisfy a query beside its matched hashtag" },
		{ u"#测试demo demo", u"demo #测试", true,
			"hashtag body protection does not depend on query order" },
		{ u"#测试 #其他demo", u"#测试 demo", true,
			"another unrequested hashtag can supply the plain word" },
		{ u"#demo", u"demo #demo", true,
			"plain word can share an entire matched hashtag body" },
		{ u"＃测试demo", u"#测试 demo", false,
			"folded hashtag body cannot supply another partial word" },
		{ u"#测试ＤＥＭＯ", u"#测试 demo", false,
			"folded suffix inside the matched hashtag remains protected" },
		{ u"测试demo", u"demo", true,
			"Han character delimits a Latin word on the left" },
		{ u"demo测试", u"demo", true,
			"Han character delimits a Latin word on the right" },
		{ u"测试demo扩展", u"demo", true,
			"Han characters delimit both ends of a Latin word" },
		{ u"xdemo测试", u"demo", false,
			"Han suffix cannot override a Latin prefix" },
		{ u"测试demox", u"demo", false,
			"Han prefix cannot override a Latin suffix" },
		{ u"caféx", u"cafe", false,
			"accent folding does not remove a Latin suffix" },
		{ u"cafe\u0301teria", u"cafe", false,
			"decomposed accent cannot create a word boundary" },
		{ u"21 12 demo1", u"1", false,
			"plain numeric word still requires surrounding boundaries" },
		{ u"demo", u" -- \t- \n", true,
			"query containing only keyword separators has no terms" },
		{ u"a.b", u"a.b", true,
			"period in query is matched literally" },
		{ u"axb", u"a.b", false,
			"period in query is not a wildcard" },
		{ u"c++", u"c++", true,
			"plus signs in query are matched literally" },
		{ u"c", u"c++", false,
			"plus signs in query are required content" },
	};
	for (const auto &entry : cases) {
		Check(
			SearchTextMatches(entry.text, entry.query) == entry.matches,
			entry.name);
	}
	const auto query = ParseSearchQuery(u"#测试 demo DEMO");
	Check(
		SearchTextMatches(u"demo #测试", query),
		"parsed query can be reused for matching messages");
	Check(
		!SearchTextMatches(u"demox #测试", query),
		"reused parsed query does not cache the previous message result");
	auto source = u"#测试 demo"_q;
	const auto owned = ParseSearchQuery(source);
	source.clear();
	Check(
		SearchTextMatches(u"demo #测试", owned),
		"parsed query owns its keywords after input text is released");
}

void CheckSearchHighlights() {
	using namespace Dialogs;
	const auto query = ParseSearchQuery(u"#测试 demo DEMO");
	auto text = TextWithEntities{ u"demox demo #测试 demo"_q };
	Check(HighlightSearchText(text, query) == 6,
		"highlight skips an incomplete first occurrence");
	Check(text.entities.size() == 3,
		"all keywords and all repeated valid occurrences are highlighted");
	Check(text.entities[0].offset() == 6 && text.entities[0].length() == 4
		&& text.entities[1].offset() == 11 && text.entities[1].length() == 3
		&& text.entities[2].offset() == 15 && text.entities[2].length() == 4,
		"unordered and deduplicated highlights retain original offsets");
	auto overlap = TextWithEntities{ u"这是测试消息"_q };
	Check(HighlightSearchText(overlap, u"测试 测试消息") == 2,
		"multiple Chinese keywords can overlap");
	Check(overlap.entities.size() == 1
		&& overlap.entities[0].length() == 4,
		"overlap across different keywords produces one highlight");
	auto tag = TextWithEntities{ u"#demo"_q };
	Check(HighlightSearchText(tag, u"demo #demo") == 0,
		"plain word and hashtag may match the same message span");
	Check(tag.entities.size() == 1
		&& tag.entities[0].length() == 5,
		"plain word highlight merges into its containing hashtag");
	auto emoji = TextWithEntities{ u"❤️demo1️⃣ #测试"_q };
	Check(HighlightSearchText(emoji, query) == 2,
		"composite emoji leaves original UTF-16 highlight offset");
	Check(emoji.entities.size() == 2
		&& emoji.entities[0].offset() == 2 && emoji.entities[0].length() == 4
		&& emoji.entities[1].offset() == 10 && emoji.entities[1].length() == 3,
		"keycap emoji is kept intact beside highlighted words");
	auto incomplete = TextWithEntities{ u"demox #测试扩展"_q };
	Check(HighlightSearchText(incomplete, query) == 6
		&& incomplete.entities.size() == 1
		&& incomplete.entities[0].length() == 3,
		"hashtag substring is highlighted while Latin suffix is rejected");
	auto protectedBody = TextWithEntities{ u"#测试demo demo"_q };
	Check(HighlightSearchText(protectedBody, query) == 0,
		"matched hashtag prefix remains highlighted");
	Check(protectedBody.entities.size() == 2
		&& protectedBody.entities[0].offset() == 0
		&& protectedBody.entities[0].length() == 3
		&& protectedBody.entities[1].offset() == 8
		&& protectedBody.entities[1].length() == 4,
		"only the independent ordinary word is highlighted beside a matched tag");
	auto clipped = TextWithEntities{ u"xdemo demo"_q };
	Check(HighlightSearchText(clipped, u"demo", 1) == 6,
		"start offset cannot manufacture a complete left boundary");
	auto sender = TextWithEntities{ u"demo: #测试 demo"_q };
	Check(HighlightSearchText(sender, query, 6) == 6,
		"all keywords exclude the sender label");
	Check(sender.entities.size() == 2
		&& sender.entities[0].offset() == 6
		&& sender.entities[1].offset() == 10,
		"sender exclusion preserves both body keyword offsets");
	auto empty = TextWithEntities{ u"demo"_q };
	Check(HighlightSearchText(empty, u" \t") == -1
		&& empty.entities.empty(),
		"empty parsed query adds no highlight entities");
	auto longText = TextWithEntities{
		QString(100000, QChar('x')) + u" demo #测试"_q,
	};
	Check(HighlightSearchText(longText, query) == 100001,
		"complete-word highlights retain offsets larger than uint16");
	Check(longText.entities.size() == 2
		&& longText.entities[0].offset() == 100001
		&& longText.entities[1].offset() == 100006,
		"long multi-keyword highlight offsets do not truncate");
}

void CheckFoldedSearchHighlights() {
	using namespace Dialogs;
	struct HighlightRange {
		int offset = 0;
		int length = 0;
	};
	const auto query = ParseSearchQuery(u"cafe 1 demo #测试");
	auto text = TextWithEntities{ u"cafe\u0301 1️⃣ ＤＥＭＯ #测试 demo"_q };
	Check(HighlightSearchText(text, query) == 0,
		"folded keywords retain the first original text position");
	const auto expected = std::vector<HighlightRange>{
		{ 0, 5 }, { 6, 3 }, { 10, 4 }, { 15, 3 }, { 19, 4 },
	};
	Check(text.entities.size() == expected.size(),
		"all folded keywords retain distinct original highlight ranges");
	for (auto i = size_t(0); i < expected.size(); ++i) {
		Check(
			text.entities[i].offset() == expected[i].offset
				&& text.entities[i].length() == expected[i].length,
			"sparse normalization mapping preserves original UTF-16 ranges");
	}
	auto precomposed = TextWithEntities{ u"café"_q };
	Check(HighlightSearchText(precomposed, u"cafe") == 0
		&& precomposed.entities.size() == 1
		&& precomposed.entities[0].length() == 4,
		"precomposed accent highlight retains the original glyph length");
	auto keycap = TextWithEntities{ u"demo1️⃣"_q };
	Check(HighlightSearchText(keycap, u"1") == 4
		&& keycap.entities.size() == 1
		&& keycap.entities[0].length() == 3,
		"numeric highlight covers the entire keycap emoji");
	auto plainDigit = TextWithEntities{ u"1"_q };
	Check(HighlightSearchText(plainDigit, u"1️⃣") == 0
		&& plainDigit.entities.size() == 1
		&& plainDigit.entities[0].length() == 1,
		"keycap query maps back to a plain numeric glyph");
	auto rocket = TextWithEntities{ u"demo🚀"_q };
	Check(HighlightSearchText(rocket, u"🚀") == 4
		&& rocket.entities.size() == 1
		&& rocket.entities[0].length() == 2,
		"emoji-only keyword highlights the complete surrogate pair");
	auto accentStart = TextWithEntities{ u"cafe\u0301 cafe"_q };
	Check(HighlightSearchText(accentStart, u"cafe", 4) == 6
		&& accentStart.entities.size() == 1
		&& accentStart.entities[0].offset() == 6,
		"start inside a folded accent cannot expose its earlier word");
	auto keycapStart = TextWithEntities{ u"1️⃣ 1"_q };
	Check(HighlightSearchText(keycapStart, u"1", 1) == 4
		&& keycapStart.entities.size() == 1
		&& keycapStart.entities[0].offset() == 4,
		"start inside a folded keycap cannot expose its earlier emoji");
	auto emojiStart = TextWithEntities{ u"🚀 🚀"_q };
	Check(HighlightSearchText(emojiStart, u"🚀", 1) == 3
		&& emojiStart.entities.size() == 1
		&& emojiStart.entities[0].offset() == 3,
		"start inside a surrogate pair cannot create an emoji match");
	auto sender = TextWithEntities{ u"cafe\u0301: ＤＥＭＯ 1️⃣ #测试"_q };
	Check(HighlightSearchText(sender, query, 7) == 7,
		"sender start uses original offsets after normalization shrinks text");
	Check(sender.entities.size() == 3
		&& sender.entities[0].offset() == 7
		&& sender.entities[1].offset() == 12
		&& sender.entities[1].length() == 3
		&& sender.entities[2].offset() == 16,
		"sender exclusion remains precise across several sparse mapping spans");
	auto marked = TextWithEntities{ u"cafe\u0301 demo"_q, {
		EntityInText(EntityType::Colorized, 0, 10),
		EntityInText(EntityType::Spoiler, 0, 10),
	} };
	Check(HighlightSearchText(marked, u"cafe demo") == 0
		&& marked.entities.size() == 4,
		"folded highlights split existing color using original ranges");
	Check(std::any_of(marked.entities.begin(), marked.entities.end(), [](const auto &entity) {
		return entity.type() == EntityType::Spoiler && entity.length() == 10;
	}), "folded highlights preserve the original spoiler span");
}

void CheckFilteredPaging() {
	struct Source {
		int oldestDate = 0;
		int task = 0;
		bool exhausted = false;
		bool failed = false;
		bool loadNext = false;
	};
	auto sources = std::map<int, Source>{
		{ 1, { .oldestDate = 100 } },
		{ 2, { .oldestDate = 90 } },
		{ 3, { .oldestDate = 100 } },
		{ 4, { .oldestDate = 110 } },
	};
	const auto next = [&] {
		return Api::PornSearchPolicy::QueueNextSearchPage(sources,
			[](int peer, const Source &) { return peer == 4; });
	};
	Check(next(), "all-hidden page can request more");
	Check(sources[1].loadNext && sources[3].loadNext
		&& !sources[2].loadNext && !sources[4].loadNext,
		"advance newest remaining frontier and skip excluded chat");
	sources[1].loadNext = sources[3].loadNext = false;
	Check(next() && sources[1].loadNext, "same timestamp advances by raw message id");
	sources[1].exhausted = sources[3].failed = true;
	Check(next() && sources[2].loadNext, "finished and failed sources do not block others");
	sources[2].exhausted = true;
	Check(!next(), "all exhausted stops automatic continuation");
	Check(!Api::PornSearchPolicy::PageAdvanced(50, 50), "repeated page stops");
	Check(Api::PornSearchPolicy::PageAdvanced(50, 1), "raw id advances after filtered page");
}

void CheckMatching() {
	auto tags = Tags{
		{ u"abcd"_q, 9 },
		{ u"xxbcyy"_q, 8 },
		{ u"bcdef"_q, 7 },
		{ u"BCstart"_q, 6 },
		{ u"other"_q, 5 },
	};
	auto suggestions = HashtagSuggestions();
	const auto matches = suggestions.filter(tags, u"bc"_q, 10);
	Check(matches.size() == 4, "prefix, infix and case-insensitive matches");
	Check(matches[0].tag == u"bcdef"_q && matches[1].tag == u"BCstart"_q,
		"prefixes first with stable history order");
	Check(matches[2] == HashtagMatch{ u"abcd"_q, 1, 2 }, "infix offsets");
	Check(matches[3].tag == u"xxbcyy"_q, "infixes keep history order");
	Check(suggestions.filter(tags, u"cd"_q, 10)[0].tag == u"abcd"_q,
		"suffix match");
	Check(suggestions.filter(tags, u"ac"_q, 10).empty(), "no subsequence match");
	Check(suggestions.filter(tags, u"abcd"_q, 10).empty(),
		"completed tag is omitted");
	Check(suggestions.filter(tags, u"bc"_q, 1)[0].tag == u"bcdef"_q,
		"early infix cannot displace later prefix at limit");
	const auto all = suggestions.filter(tags, QString(), 10);
	Check(all.size() == tags.size() && all[0].tag == tags[0].first,
		"bare hash keeps history order");
	Check(all[0].length == 0, "bare hash has no highlight");
	Check(suggestions.filter(tags, u"bc"_q, 0).empty(), "zero result limit");
	Check(suggestions.filter(tags, u"bc"_q, 2).size() == 2,
		"limit change invalidates cached results");
	tags.removeAt(2);
	suggestions.invalidate();
	Check(suggestions.filter(tags, u"bc"_q, 10)[0].tag == u"BCstart"_q,
		"deletion invalidates cache");
	tags.push_front({ u"bcnew"_q, 10 });
	suggestions.invalidate();
	Check(suggestions.filter(tags, u"bc"_q, 10)[0].tag == u"bcnew"_q,
		"insert and reorder invalidate cache");
	Check(suggestions.filter(tags, u"b"_q, 10).size() == 4,
		"backspace searches full history");
}

void CheckUnicode() {
	const auto tags = Tags{
		{ u"caf\u00e9s"_q, 4 },
		{ u"cafe\u0301s"_q, 3 },
		{ u"\U0001d400bcd"_q, 2 },
		{ u"中文标签"_q, 1 },
	};
	auto sent = HashtagSuggestions(TextUtilities::RemoveAccents);
	const auto accent = sent.filter(tags, u"fes"_q, 10);
	Check(accent.size() == 2, "accent-insensitive substring");
	Check(accent[0] == HashtagMatch{ tags[0].first, 2, 3 },
		"precomposed accent highlight");
	Check(accent[1] == HashtagMatch{ tags[1].first, 2, 4 },
		"combining mark maps back to original span");
	const auto ending = sent.filter(tags, u"fe"_q, 10);
	Check(ending[1].length == 3, "highlight includes trailing combining mark");
	const auto afterMark = sent.filter(tags, u"s"_q, 10);
	Check(afterMark[1].offset == 5, "offset after removed combining mark");
	const auto surrogate = sent.filter(tags, u"bc"_q, 10);
	Check(surrogate.size() == 1 && surrogate[0].offset == 2,
		"UTF-16 offset after supplementary character");
	const auto chinese = sent.filter(tags, u"文标"_q, 10);
	Check(chinese.size() == 1 && chinese[0].offset == 1,
		"Chinese substring");
	Check(sent.filter(tags, u"cafes"_q, 10).empty(),
		"completed normalized tags are omitted");
	auto search = HashtagSuggestions();
	Check(search.filter(tags, u"fes"_q, 10).empty(),
		"search history retains accent-sensitive behavior");
}

void CheckExactAndCompletion() {
	const auto tags = Tags{
		{ u"测试效果abc"_q, 9 },
		{ u"其他测试效果"_q, 8 },
		{ u"测试效果"_q, 1 },
	};
	auto suggestions = HashtagSuggestions();
	const auto matches = suggestions.filter(tags, u"测试效果"_q, 10);
	Check(matches.size() == 2 && matches[0].tag == u"测试效果abc"_q
		&& matches[1].tag == u"其他测试效果"_q,
		"pasted complete hashtag leaves only longer and infix matches");
	Check(suggestions.filter(tags, u"测试效果"_q, 1)[0].tag == u"测试效果abc"_q,
		"exact match does not consume the result limit");

	const auto original = u"#测试效果"_q;
	for (auto position = 1; position <= original.size(); ++position) {
		auto document = QTextDocument();
		document.setPlainText(original);
		auto cursor = QTextCursor(&document);
		cursor.setPosition(position);
		const auto range = FindHashtagCompletion(cursor);
		Check(range && range->query == original,
			"moving inside a token keeps the full matching query");
		Check(CompleteHashtag(cursor, u"#测试效果"_q),
			"accept exact match from every cursor position");
		Check(document.toPlainText() == original && cursor.position() == 5,
			"left then Enter leaves no suffix or trailing space");
	}
	const auto complete = [](const QString &before,
			int position,
			const QString &replacement,
			const QString &expected) {
		auto document = QTextDocument();
		document.setPlainText(before);
		auto cursor = QTextCursor(&document);
		cursor.setPosition(position);
		Check(CompleteHashtag(cursor, replacement), "completion accepted");
		Check(document.toPlainText() == expected,
			"complete token replaced while preserving surrounding text");
		document.undo();
		Check(document.toPlainText() == before, "one undo restores input");
		document.redo();
		Check(document.toPlainText() == expected, "one redo reapplies completion");
	};
	complete(u"#测试效果abc"_q, 4, u"#测试效果"_q, u"#测试效果"_q);
	complete(u"#测试效果"_q, 4, u"#测试效果abc"_q, u"#测试效果abc"_q);
	complete(u"前文 #测试效果 后文"_q, 7, u"#测试效果abc"_q,
		u"前文 #测试效果abc 后文"_q);
	complete(u"#测试效果，后文"_q, 4, u"#测试效果abc"_q,
		u"#测试效果abc，后文"_q);
	complete(u"前文\n#测试效果\n后文"_q, 7, u"#测试效果abc"_q,
		u"前文\n#测试效果abc\n后文"_q);
	complete(u"#cafe\u0301s"_q, 4, u"#cafes"_q, u"#cafes"_q);
	complete(u"#\U0001d400bc"_q, 3, u"#other"_q, u"#other"_q);
	complete(u"#"_q, 1, u"#测试效果"_q, u"#测试效果"_q);
	complete(u"#测试效果 #其他"_q, 4, u"#测试效果abc"_q,
		u"#测试效果abc #其他"_q);

	for (const auto &text : { u"测试效果"_q, u"前文#测试效果"_q,
		u"#测试效果 "_q, u"#测试效果\n"_q, u"#"_q + QString(65, 'a') }) {
		auto document = QTextDocument();
		document.setPlainText(text);
		auto cursor = QTextCursor(&document);
		cursor.movePosition(QTextCursor::End);
		Check(!CompleteHashtag(cursor, u"#测试效果"_q),
			"completion requires an active bounded hashtag token");
		Check(document.toPlainText() == text, "invalid completion changes nothing");
	}
	auto document = QTextDocument();
	document.setHtml(u"前文 #测<b>试效</b>果 后文"_q);
	auto cursor = QTextCursor(&document);
	cursor.setPosition(7);
	Check(CompleteHashtag(cursor, u"#测试效果abc"_q)
		&& document.toPlainText() == u"前文 #测试效果abc 后文"_q,
		"formatting fragments do not split hashtag replacement");
	cursor.setPosition(4);
	cursor.setPosition(6, QTextCursor::KeepAnchor);
	Check(!CompleteHashtag(cursor, u"#测试效果"_q),
		"selected text never triggers automatic token replacement");
}

void CheckCacheAndBenchmark() {
	for (const auto count : { 1000, 5000, 10000 }) {
		auto tags = Tags();
		tags.reserve(count);
		for (auto i = 0; i < count; ++i) {
			tags.push_back({ u"topic_%1_中文_caf\u00e9"_q.arg(i), 1 });
		}
		auto cache = HashtagSuggestions(CountedNormalize);
		auto timer = QElapsedTimer();
		timer.start();
		const auto coldCount = cache.filter(tags, u"caf"_q, count).size();
		const auto cold = timer.nsecsElapsed();
		Check(coldCount == count, "all 10k matches remain available");
		const auto before = Normalizations;
		timer.restart();
		for (auto i = 0; i < 1000; ++i) {
			Check(cache.filter(tags, u"caf"_q, count).size() == count,
				"same-query cache");
		}
		const auto cached = timer.nsecsElapsed() / 1000.;
		Check(Normalizations - before == 1000,
			"cached queries never normalize candidates again");
		const auto queries = {
			u"caf"_q, u"中文"_q, u"topic"_q, u"_99"_q, u"absent"_q,
		};
		auto times = std::vector<qint64>();
		auto checksum = qsizetype(0);
		for (auto i = 0; i < 20; ++i) {
			for (const auto &query : queries) {
				timer.restart();
				checksum += cache.filter(tags, query, count).size();
				times.push_back(timer.nsecsElapsed());
			}
		}
		std::sort(times.begin(), times.end());
		Check(checksum > 0, "benchmark consumes results");
		std::cout << "tags=" << count
			<< " cold_ms=" << cold / 1e6
			<< " query_median_ms=" << times[times.size() / 2] / 1e6
			<< " query_p95_ms=" << times[times.size() * 95 / 100] / 1e6
			<< " cached_us=" << cached / 1e3 << '\n';
	}
}

void CheckPainting() {
	auto image = QImage(240, 64, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::white);
	auto painter = QPainter(&image);
	auto font = QFont(u"Arial"_q, 12);
	// Subpixel fringes on black glyphs must not count as red highlights.
	font.setStyleStrategy(QFont::NoAntialias);
	for (const auto width : { 0, 1, 15, 40, 200 }) {
		const auto used = PaintHashtagMatch(
			painter, font, { u"abcd"_q, 1, 2 }, { 0, 32 }, width,
			Qt::black, Qt::red);
		Check(used >= 0 && used <= width + 1, "elided text stays within width");
	}
	Check(PaintHashtagMatch(
		painter, font, { u"中文标签"_q, 1, 2 }, { 0, 32 }, 200,
		Qt::black, Qt::red) > 0, "Chinese matched span renders");
	Check(PaintHashtagMatch(
		painter, font, { u"cafe\u0301s"_q, 2, 3 }, { 0, 32 }, 200,
		Qt::black, Qt::red) > 0, "combining mark span renders");
	painter.end();
	image.fill(Qt::white);
	painter.begin(&image);
	PaintHashtagMatch(
		painter, font, { u"abcd"_q, 1, 2 }, { 0, 32 }, 200,
		Qt::black, Qt::red);
	painter.end();
	auto firstRed = image.width();
	auto lastRed = -1;
	auto firstBlack = image.width();
	auto lastBlack = -1;
	for (auto y = 0; y < image.height(); ++y) {
		for (auto x = 0; x < image.width(); ++x) {
			const auto color = image.pixelColor(x, y);
			if (color.red() > color.green() + 50) {
				firstRed = std::min(firstRed, x);
				lastRed = std::max(lastRed, x);
			} else if (color.red() < 100 && color.green() < 100) {
				firstBlack = std::min(firstBlack, x);
				lastBlack = std::max(lastBlack, x);
			}
		}
	}
	Check(firstRed <= lastRed && firstBlack < firstRed && lastBlack > lastRed,
		"only the middle matched span is highlighted");
}

} // namespace

namespace crl {

rpl::producer<> on_main_update_requests() {
	// This QImage-only test has no widget update events.
	return rpl::never<>();
}

} // namespace crl

int main(int argc, char *argv[]) {
	const auto app = QGuiApplication(argc, argv);
	Ui::Emoji::internal::Init();
	CheckMessageSearch();
	CheckSearchQueryRules();
	CheckSearchHighlights();
	CheckFoldedSearchHighlights();
	CheckFilteredPaging();
	CheckMatching();
	CheckUnicode();
	CheckExactAndCompletion();
	CheckCacheAndBenchmark();
	CheckPainting();
	std::cout << "Passed " << Checks << " checks.\n";
	return 0;
}
