#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MessageFolding {

[[nodiscard]] inline std::optional<std::uint64_t> ParseUserId(
		std::u16string_view value) {
	constexpr auto kMaximum = std::uint64_t(0xFFFFFFFFFFFFULL);
	auto result = std::uint64_t(0);
	for (const auto ch : value) {
		if (ch < u'0' || ch > u'9') {
			return std::nullopt;
		}
		const auto digit = std::uint64_t(ch - u'0');
		if (result > (kMaximum - digit) / 10) {
			return std::nullopt;
		}
		result = result * 10 + digit;
	}
	return result ? std::optional(result) : std::nullopt;
}

class KeywordMatcher final {
public:
	KeywordMatcher() = default;
	explicit KeywordMatcher(std::vector<std::u16string> words)
	: _words(std::move(words)) {
		std::erase_if(_words, [](const auto &word) { return word.empty(); });
		if (_words.size() <= kLinearLimit) {
			return;
		}
		_nodes.emplace_back();
		for (const auto &word : _words) {
			auto state = 0;
			for (const auto ch : word) {
				const auto i = _nodes[state].next.find(ch);
				if (i == _nodes[state].next.end()) {
					const auto next = int(_nodes.size());
					_nodes[state].next.emplace(ch, next);
					_nodes.emplace_back();
					state = next;
				} else {
					state = i->second;
				}
			}
			_nodes[state].terminal = true;
		}
		auto queue = std::queue<int>();
		for (const auto &edge : _nodes.front().next) {
			queue.push(edge.second);
		}
		while (!queue.empty()) {
			const auto parent = queue.front();
			queue.pop();
			for (const auto &[ch, child] : _nodes[parent].next) {
				auto failure = _nodes[parent].failure;
				while (failure && !_nodes[failure].next.contains(ch)) {
					failure = _nodes[failure].failure;
				}
				const auto i = _nodes[failure].next.find(ch);
				if (i != _nodes[failure].next.end()) {
					failure = i->second;
				}
				_nodes[child].failure = failure;
				_nodes[child].terminal = _nodes[child].terminal
					|| _nodes[failure].terminal;
				queue.push(child);
			}
		}
	}

	[[nodiscard]] bool empty() const {
		return _words.empty();
	}

	[[nodiscard]] bool matches(std::u16string_view text) const {
		if (_nodes.empty()) {
			for (const auto &word : _words) {
				if (text.find(word) != std::u16string_view::npos) {
					return true;
				}
			}
			return false;
		}
		auto state = 0;
		for (const auto ch : text) {
			while (state && !_nodes[state].next.contains(ch)) {
				state = _nodes[state].failure;
			}
			const auto i = _nodes[state].next.find(ch);
			if (i != _nodes[state].next.end()) {
				state = i->second;
			}
			if (_nodes[state].terminal) {
				return true;
			}
		}
		return false;
	}

private:
	struct Node {
		std::unordered_map<char16_t, int> next;
		int failure = 0;
		bool terminal = false;
	};

	static constexpr auto kLinearLimit = std::size_t(16);
	std::vector<std::u16string> _words;
	std::vector<Node> _nodes;

};

} // namespace MessageFolding
