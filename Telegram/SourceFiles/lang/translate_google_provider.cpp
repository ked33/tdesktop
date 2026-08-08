/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lang/translate_google_provider.h"

#include "boxes/GoogleAppTranslator.h"
#include "main/main_session.h"
#include "settings.h"

namespace Ui {
namespace {

constexpr auto kMaxConcurrent = 4;

[[nodiscard]] QString ResolveTargetCode(const LanguageId &to) {
	if (GetEnhancedBool(u"translate_to_tc"_q)) {
		return u"zh-TW"_q;
	}
	const auto code = to.twoLetterCode();
	return code.isEmpty() ? u"en"_q : code;
}

class GoogleTranslateProvider final : public TranslateProvider {
public:
	explicit GoogleTranslateProvider(not_null<Main::Session*>) {
	}

	[[nodiscard]] bool supportsMessageId() const override {
		return false;
	}

	void request(
			TranslateProviderRequest request,
			LanguageId to,
			Fn<void(TranslateProviderResult)> done) override {
		if (request.text.text.isEmpty()) {
			done(TranslateProviderResult{
				.error = TranslateProviderError::Unknown,
			});
			return;
		}
		const auto text = request.text.text;
		const auto toCode = ResolveTargetCode(to);
		crl::async([=] {
			const auto engine = GoogleAppTranslator::instance()->translate(
				text,
				u"auto"_q,
				toCode);
			const auto ok = !engine.isError && !engine.translation.isEmpty();
			const auto translated = engine.translation;
			crl::on_main([=] {
				if (!ok) {
					done(TranslateProviderResult{
						.error = TranslateProviderError::Unknown,
					});
					return;
				}
				done(TranslateProviderResult{
					.text = TextWithEntities{ translated },
				});
			});
		});
	}

	void requestBatch(
			std::vector<TranslateProviderRequest> requests,
			const LanguageId &to,
			Fn<void(int, TranslateProviderResult)> doneOne,
			Fn<void()> doneAll) override {
		if (requests.empty()) {
			doneAll();
			return;
		}
		struct State {
			std::vector<TranslateProviderRequest> requests;
			LanguageId to;
			Fn<void(int, TranslateProviderResult)> doneOne;
			Fn<void()> doneAll;
			int next = 0;
			int active = 0;
			int remaining = 0;
		};
		const auto state = std::make_shared<State>(State{
			.requests = std::move(requests),
			.to = to,
			.doneOne = std::move(doneOne),
			.doneAll = std::move(doneAll),
			.remaining = 0,
		});
		state->remaining = int(state->requests.size());

		const auto pump = std::make_shared<Fn<void()>>();
		*pump = [=] {
			while (state->active < kMaxConcurrent
				&& state->next < int(state->requests.size())) {
				const auto index = state->next++;
				++state->active;
				request(
					state->requests[index],
					state->to,
					[=](TranslateProviderResult result) {
						state->doneOne(index, std::move(result));
						--state->active;
						if (!--state->remaining) {
							state->doneAll();
							return;
						}
						(*pump)();
					});
			}
		};
		(*pump)();
	}

};

} // namespace

std::unique_ptr<TranslateProvider> CreateGoogleTranslateProvider(
		not_null<Main::Session*> session) {
	return std::make_unique<GoogleTranslateProvider>(session);
}

} // namespace Ui
