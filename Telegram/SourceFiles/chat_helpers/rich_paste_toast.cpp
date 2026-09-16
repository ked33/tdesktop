/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "chat_helpers/rich_paste_toast.h"

#include "chat_helpers/message_field.h"
#include "iv/editor/iv_editor_clipboard_import.h"
#include "iv/editor/iv_editor_session.h"
#include "iv/iv_rich_page.h"
#include "lang/lang_keys.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "ui/toast/toast_widget.h"
#include "ui/widgets/buttons.h"
#include "styles/style_chat.h"

#include <QtCore/QMimeData>

namespace ChatHelpers {
namespace {

constexpr auto kToastDuration = 9 * crl::time(1000);

} // namespace

std::optional<RichPasteDecision> MimeDataRichPasteOffer(
		not_null<Main::Session*> session,
		not_null<const QMimeData*> data) {
	if (PasteAsPlainTextRequested()
		|| !Iv::Editor::CanAuthorRichMessages(session)) {
		return std::nullopt;
	}
	const auto limits = Iv::ResolveRichMessageLimits(session);
	if (Iv::Editor::MimeDataHasRichStructure(session, data, limits)) {
		return RichPasteDecision{ .offer = RichPasteOffer::Editor };
	} else if (data->hasHtml() || !data->hasText()) {
		return std::nullopt;
	}
	const auto text = data->text();
	if (auto markdown = Iv::Editor::ComposeFieldMarkdown(
			session,
			text,
			limits)) {
		return RichPasteDecision{
			.offer = RichPasteOffer::Field,
			.markdown = std::move(*markdown),
		};
	} else if (Iv::Editor::TextHasMarkdownStructure(text, limits)) {
		return RichPasteDecision{ .offer = RichPasteOffer::Markdown };
	}
	return std::nullopt;
}

TextWithTags TextWithTagsReplaced(
		const TextWithTags &original,
		int from,
		int till,
		const TextWithTags &with) {
	auto result = TextWithTags();
	result.text = original.text.mid(0, from)
		+ with.text
		+ original.text.mid(till);
	const auto shift = int(with.text.size()) - (till - from);
	for (const auto &tag : original.tags) {
		if (tag.offset + tag.length <= from) {
			result.tags.push_back(tag);
		}
	}
	for (const auto &tag : with.tags) {
		result.tags.push_back({ from + tag.offset, tag.length, tag.id });
	}
	for (const auto &tag : original.tags) {
		if (tag.offset >= till) {
			result.tags.push_back({ tag.offset + shift, tag.length, tag.id });
		}
	}
	return result;
}

std::shared_ptr<QMimeData> CloneMimeData(not_null<const QMimeData*> data) {
	auto result = std::make_shared<QMimeData>();
	for (const auto &format : data->formats()) {
		result->setData(format, data->data(format));
	}
	return result;
}

void ShowRichPasteToast(RichPasteToastArgs &&args) {
	const auto undo = (args.offer == RichPasteOffer::Plain);
	const auto field = (args.offer == RichPasteOffer::Field);
	if (!undo && !field && !Iv::Editor::SessionPremium(args.session)) {
		return;
	}
	const auto markdown = (args.offer == RichPasteOffer::Markdown);
	const auto button = undo
		? tr::lng_rich_paste_toast_undo(tr::now)
		: field
		? tr::lng_rich_paste_toast_apply(tr::now)
		: tr::lng_rich_paste_toast_open(tr::now);
	auto text = tr::bold(undo
		? tr::lng_rich_paste_toast_markdown(tr::now)
		: tr::lng_rich_paste_toast(tr::now)
	).append('\n').append(undo
		? tr::lng_rich_paste_toast_plain(tr::now, tr::rich)
		: field
		? tr::lng_rich_paste_toast_field(tr::now, tr::rich)
		: markdown
		? tr::lng_rich_paste_toast_editor_markdown(tr::now, tr::rich)
		: tr::lng_rich_paste_toast_editor(tr::now, tr::rich));
	const auto st = std::make_shared<style::Toast>(st::historyPremiumToast);
	if (!button.isEmpty()) {
		st->padding.setRight(
			st::historyPremiumViewSet.style.font->width(button)
			- st::historyPremiumViewSet.width);
	}
	const auto weak = Ui::Toast::Show(args.parent, Ui::Toast::Config{
		.text = std::move(text),
		.similarLines = true,
		.st = st.get(),
		.attach = RectPart::Bottom,
		.addToAttachSide = std::move(args.bottomOffset),
		.acceptinput = true,
		.duration = kToastDuration,
	});
	const auto strong = weak.get();
	if (!strong) {
		return;
	}
	const auto widget = strong->widget();
	widget->lifetime().add([st] {});
	if (args.cancel) {
		std::move(args.cancel) | rpl::on_next([=] {
			if (const auto strong = weak.get()) {
				strong->hideAnimated();
			}
		}, widget->lifetime());
	}
	if (button.isEmpty()) {
		return;
	}
	const auto activate = Ui::CreateChild<Ui::RoundButton>(
		widget.get(),
		rpl::single(button),
		st::historyPremiumViewSet);
	activate->show();
	activate->setClickedCallback([=, action = std::move(args.action)] {
		if (const auto strong = weak.get()) {
			strong->hideAnimated();
		}
		if (action) {
			action();
		}
	});
	rpl::combine(
		widget->sizeValue(),
		activate->sizeValue()
	) | rpl::on_next([=](QSize outer, QSize inner) {
		activate->moveToRight(
			0,
			(outer.height() - inner.height()) / 2,
			outer.width());
	}, widget->lifetime());
}

} // namespace ChatHelpers
