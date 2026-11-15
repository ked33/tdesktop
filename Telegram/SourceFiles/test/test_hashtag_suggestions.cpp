#include "chat_helpers/hashtag_suggestions.h"
#include "dialogs/dialogs_search_match.h"
#include "api/api_porn_search_policy.h"

#include "ui/text/text_entity.h"

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
	auto overlap = TextWithEntities{ u"ababa"_q };
	Check(HighlightSearchText(overlap, u"aba") == 0, "overlap first offset");
	Check(overlap.entities.size() == 1 && overlap.entities[0].length() == 5,
		"overlapping matches are merged");
	auto marked = TextWithEntities{ u"abcdef"_q, {
		EntityInText(EntityType::Colorized, 0, 6),
		EntityInText(EntityType::Spoiler, 0, 6),
	} };
	Check(HighlightSearchText(marked, u"cd") == 2, "match inside existing color");
	Check(marked.entities.size() == 4, "existing color split around match");
	Check(std::any_of(begin(marked.entities), end(marked.entities), [](const auto &entity) {
		return entity.type() == EntityType::Spoiler && entity.length() == 6;
	}), "spoiler preserved");
	auto sender = TextWithEntities{ u"搜索测试: 正文搜索测试"_q };
	Check(HighlightSearchText(sender, u"搜索测试", 6) == 8,
		"sender label excluded from matching");
	auto longText = TextWithEntities{ QString(100000, QChar('x')) + u"搜索测试"_q };
	Check(HighlightSearchText(longText, u"搜索测试") == 100000,
		"match offsets do not truncate to uint16");
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
	CheckMessageSearch();
	CheckFilteredPaging();
	CheckMatching();
	CheckUnicode();
	CheckExactAndCompletion();
	CheckCacheAndBenchmark();
	CheckPainting();
	std::cout << "Passed " << Checks << " checks.\n";
	return 0;
}
