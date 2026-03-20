/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/history_view_context_menu.h"

#include "api/api_attached_stickers.h"
#include "api/api_common.h"
#include "api/api_editing.h"
#include "api/api_global_privacy.h"
#include "api/api_polls.h"
#include "api/api_report.h"
#include "api/api_ringtones.h"
#include "api/api_sending.h"
#include "api/api_transcribes.h"
#include "api/api_who_reacted.h"
#include "api/api_toggling_media.h" // Api::ToggleFavedSticker
#include "base/qt/qt_key_modifiers.h"
#include "base/unixtime.h"
#include "history/view/history_view_list_widget.h"
#include "history/view/history_view_cursor_state.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_text.h"
#include "history/history_item_components.h"
#include "history/view/history_view_schedule_box.h"
#include "history/view/media/history_view_media.h"
#include "history/view/media/history_view_save_document_action.h"
#include "history/view/media/history_view_web_page.h"
#include "history/view/reactions/history_view_reactions_list.h"
#include "info/info_memento.h"
#include "info/profile/info_profile_widget.h"
#include "main/main_session.h"
#include "main/session/send_as_peers.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/menu/menu_add_action_callback_factory.h"
#include "ui/widgets/menu/menu_action.h"
#include "ui/widgets/menu/menu_common.h"
#include "ui/widgets/menu/menu_multiline_action.h"
#include "ui/image/image.h"
#include "ui/painter.h"
#include "ui/toast/toast.h"
#include "ui/text/format_song_document_name.h"
#include "ui/text/text_utilities.h"
#include "ui/controls/delete_message_context_action.h"
#include "ui/controls/who_reacted_context_action.h"
#include "ui/boxes/edit_factcheck_box.h"
#include "ui/boxes/report_box_graphics.h"
#include "ui/ui_utility.h"
#include "menu/menu_item_download_files.h"
#include "menu/menu_item_rate_transcribe.h"
#include "menu/menu_item_rate_transcribe_session.h"
#include "menu/menu_send.h"
#include "ui/boxes/confirm_box.h"
#include "ui/boxes/show_or_premium_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/power_saving.h"
#include "boxes/delete_messages_box.h"
#include "boxes/moderate_messages_box.h"
#include "boxes/report_messages_box.h"
#include "boxes/sticker_set_box.h"
#include "boxes/stickers_box.h"
#include "boxes/translate_box.h"
#include "data/components/factchecks.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_forum_topic.h"
#include "data/data_session.h"
#include "data/data_stories.h"
#include "data/data_groups.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_user.h"
#include "data/data_file_click_handler.h"
#include "data/data_file_origin.h"
#include "data/data_message_reactions.h"
#include "data/data_user.h"
#include "data/stickers/data_custom_emoji.h"
#include "chat_helpers/message_field.h" // FactcheckFieldIniter.
#include "core/file_utilities.h"
#include "core/mime_type.h"
#include "core/click_handler_types.h"
#include "base/platform/base_platform_info.h"
#include "base/call_delayed.h"
#include "settings/sections/settings_premium.h"
#include "window/window_peer_menu.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "lang/lang_keys.h"
#include "core/application.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "spellcheck/spellcheck_types.h"
#include "iv/iv_instance.h" 
#include "facades.h"
#include "apiwrap.h"
#include "ui/effects/ripple_animation.h"
#include "ui/text/format_values.h"
#include "styles/style_chat.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_menu_icons.h"

#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>
#include <QtGui/QtEvents>

#include "data/data_saved_sublist.h"

namespace HistoryView {
namespace {

constexpr auto kRescheduleLimit = 20;
constexpr auto kTagNameLimit = 12;
constexpr auto kPublicPostLinkToastDuration = 4 * crl::time(1000);

const TextParseOptions kMenuTextOptions = {
	TextParseLinks,
	0,
	0,
	Qt::LayoutDirectionAuto,
};

class TwoTextAction final : public Ui::Menu::ItemBase {
public:
	TwoTextAction(
		not_null<Ui::Menu::Menu*> parent,
		const style::Menu &st,
		const QString &text1,
		const QString &text2,
		Fn<void()> callback,
		const style::icon *icon,
		const style::icon *iconOver)
	: ItemBase(parent, st)
	, _dummyAction(Ui::CreateChild<QAction>(parent))
	, _st(st)
	, _icon(icon)
	, _iconOver(iconOver)
	, _text2(text2)
	, _height(st::ttlItemPadding.top()
		+ _st.itemStyle.font->height
		+ st::ttlItemTimerFont->height
		+ st::ttlItemPadding.bottom()) {
		fitToMenuWidth();
		setActionTriggered(std::move(callback));

		paintRequest(
		) | rpl::on_next([=] {
			Painter p(this);
			paint(p);
		}, lifetime());

		enableMouseSelecting();
		prepare(text1);
	}

	[[nodiscard]] bool isEnabled() const override {
		return true;
	}

	[[nodiscard]] not_null<QAction*> action() const override {
		return _dummyAction;
	}

	void handleKeyPress(not_null<QKeyEvent*> e) override {
		if (!isSelected()) {
			return;
		}
		const auto key = e->key();
		if (key == Qt::Key_Enter || key == Qt::Key_Return) {
			setClicked(Ui::Menu::TriggeredSource::Keyboard);
		}
	}

private:
	[[nodiscard]] QPoint prepareRippleStartPosition() const override {
		return mapFromGlobal(QCursor::pos());
	}

	[[nodiscard]] QImage prepareRippleMask() const override {
		return Ui::RippleAnimation::RectMask(size());
	}

	[[nodiscard]] int contentHeight() const override {
		return _height;
	}

	void paint(Painter &p) {
		const auto selected = isSelected();
		if (selected && _st.itemBgOver->c.alpha() < 255) {
			p.fillRect(0, 0, width(), _height, _st.itemBg);
		}
		p.fillRect(
			0,
			0,
			width(),
			_height,
			selected ? _st.itemBgOver : _st.itemBg);
		if (isEnabled()) {
			paintRipple(p, 0, 0);
		}

		const auto normalHeight = _st.itemPadding.top()
			+ _st.itemStyle.font->height
			+ _st.itemPadding.bottom();
		const auto deltaHeight = _height - normalHeight;
		if (const auto icon = selected ? _iconOver : _icon) {
			icon->paint(
				p,
				_st.itemIconPosition + QPoint(0, deltaHeight / 2),
				width());
		}

		p.setPen(selected ? _st.itemFgOver : _st.itemFg);
		_text1.drawLeftElided(
			p,
			_st.itemPadding.left(),
			st::ttlItemPadding.top(),
			_textWidth1,
			width());

		p.setFont(st::ttlItemTimerFont);
		p.setPen(selected ? _st.itemFgShortcutOver : _st.itemFgShortcut);
		p.drawTextLeft(
			_st.itemPadding.left(),
			st::ttlItemPadding.top() + _st.itemStyle.font->height,
			width(),
			_text2);
	}

	void prepare(const QString &text1) {
		_text1.setMarkedText(_st.itemStyle, { text1 }, kMenuTextOptions);
		const auto textWidth1 = _text1.maxWidth();
		const auto textWidth2 = st::ttlItemTimerFont->width(_text2);
		const auto &padding = _st.itemPadding;
		const auto goodWidth = padding.left()
			+ std::max(textWidth1, textWidth2)
			+ padding.right();
		const auto w = std::clamp(
			goodWidth,
			_st.widthMin,
			_st.widthMax);
		_textWidth1 = w - (goodWidth - textWidth1);
		setMinWidth(w);
		update();
	}

	const not_null<QAction*> _dummyAction;
	const style::Menu &_st;
	const style::icon *_icon;
	const style::icon *_iconOver;
	Ui::Text::String _text1;
	QString _text2;
	int _textWidth1 = 0;
	const int _height;
};

[[nodiscard]] base::unique_qptr<Ui::Menu::ItemBase> CreateTwoTextAction(
		not_null<Ui::Menu::Menu*> menu,
		const style::icon *icon,
		const QString &text1,
		const QString &text2,
		Fn<void()> callback = nullptr) {
	if (!callback) {
		const auto copy = text2;
		callback = [=] {
			QGuiApplication::clipboard()->setText(copy);
		};
	}
	return base::make_unique_q<TwoTextAction>(
		menu,
		menu->st(),
		text1,
		text2,
		std::move(callback),
		icon,
		icon);
}

[[nodiscard]] bool IsDetailsActionText(const QString &text) {
	return text == tr::lng_context_details(tr::now);
}

[[nodiscard]] bool IsPostLinkActionText(const QString &text) {
	return (text == tr::lng_context_copy_message_link(tr::now))
		|| (text == tr::lng_context_copy_post_link(tr::now));
}

[[nodiscard]] int RateTranscribeInsertIndex(not_null<Ui::PopupMenu*> menu) {
	const auto &actions = menu->actions();
	if (actions.empty()) {
		return 0;
	}
	if (IsPostLinkActionText(actions.front()->text())) {
		return (actions.size() > 1 && IsDetailsActionText(actions[1]->text()))
			? 2
			: 1;
	}
	return IsDetailsActionText(actions.front()->text()) ? 1 : 0;
}

[[nodiscard]] QString FormatDetailsDateTime(const QDateTime &date) {
	return date.isValid() ? Ui::FormatDateTime(date) : QString();
}

[[nodiscard]] QString DatacenterName(int dc) {
	const auto name = [=] {
		switch (dc) {
		case 1:
		case 3: return u"Miami FL, USA"_q;
		case 2:
		case 4: return u"Amsterdam, NL"_q;
		case 5: return u"Singapore, SG"_q;
		default: return u"UNKNOWN"_q;
		}
	}();
	return (dc < 1)
		? u"DC_UNKNOWN"_q
		: QString(u"DC%1, %2"_q).arg(dc).arg(name);
}

[[nodiscard]] int64 MediaSizeBytes(not_null<HistoryItem*> item) {
	const auto media = item->media();
	if (!media) {
		return -1;
	}
	const auto document = media->document();
	const auto photo = media->photo();
	if (document) {
		return document->size;
	} else if (photo && photo->hasVideo()) {
		auto size = int64(photo->videoByteSize(Data::PhotoSize::Large));
		if (size == 0) {
			size = photo->videoByteSize(Data::PhotoSize::Small);
		}
		if (size == 0) {
			size = photo->videoByteSize(Data::PhotoSize::Thumbnail);
		}
		return size;
	} else if (photo) {
		auto size = int64(photo->imageByteSize(Data::PhotoSize::Large));
		if (size == 0) {
			size = photo->imageByteSize(Data::PhotoSize::Small);
		}
		if (size == 0) {
			size = photo->imageByteSize(Data::PhotoSize::Thumbnail);
		}
		return size;
	}
	return -1;
}

[[nodiscard]] QString MediaSizeText(HistoryItem *item) {
	if (!item) {
		return {};
	}
	const auto size = MediaSizeBytes(not_null(item));
	return (size >= 0) ? Ui::FormatSizeText(size) : QString();
}

[[nodiscard]] QString MediaMime(HistoryItem *item) {
	if (!item) {
		return {};
	}
	const auto media = item->media();
	if (!media) {
		return {};
	}
	if (const auto document = media->document()) {
		return document->mimeString();
	} else if (const auto photo = media->photo()) {
		return photo->hasVideo() ? u"video/mp4"_q : u"image/jpeg"_q;
	}
	return {};
}

[[nodiscard]] QString MediaName(HistoryItem *item) {
	if (!item) {
		return {};
	}
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	return document ? document->filename() : QString();
}

[[nodiscard]] QString FormatResolution(QSize size) {
	return (size.isValid() && !size.isEmpty())
		? QString(u"%1x%2"_q).arg(size.width()).arg(size.height())
		: QString();
}

[[nodiscard]] QString MediaResolution(HistoryItem *item) {
	if (!item) {
		return {};
	}
	const auto media = item->media();
	if (!media) {
		return {};
	}
	if (const auto document = media->document()) {
		return FormatResolution(document->dimensions);
	} else if (const auto photo = media->photo()) {
		auto size = photo->size(Data::PhotoSize::Large);
		if (!size) {
			size = photo->size(Data::PhotoSize::Small);
		}
		if (!size) {
			size = photo->size(Data::PhotoSize::Thumbnail);
		}
		return size ? FormatResolution(*size) : QString();
	}
	return {};
}

[[nodiscard]] QString MediaDC(HistoryItem *item) {
	if (!item) {
		return {};
	}
	const auto media = item->media();
	if (!media) {
		return {};
	}
	if (const auto document = media->document()) {
		return DatacenterName(document->getDC());
	} else if (const auto photo = media->photo()) {
		return DatacenterName(photo->getDC());
	}
	return {};
}

[[nodiscard]] QString VideoCodec(HistoryItem *item) {
	if (!item) {
		return {};
	}
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	const auto video = document ? document->video() : nullptr;
	return (document && document->isVideoFile() && video)
		? video->codec
		: QString();
}

[[nodiscard]] uint64 UserIdFromPackId(uint64 id) {
	auto ownerId = id >> 32;
	if ((id >> 16) & 0xFF) {
		if (((id >> 16) & 0xFF) == 0x3F) {
			ownerId |= 0x80000000;
		}
	}
	if ((id >> 24) & 0xFF) {
		ownerId += 0x100000000;
	}
	return ownerId;
}

void AddPackOwnerRow(
		not_null<Ui::PopupMenu*> menu,
		uint64 packId,
		not_null<Window::SessionController*> controller) {
	if (!packId) {
		return;
	}
	const auto ownerId = UserIdFromPackId(packId);
	const auto peer = controller->session().data().peerLoaded(PeerId(ownerId));
	const auto value = peer
		? peer->name()
		: QString::number(ownerId);
	menu->addAction(CreateTwoTextAction(
		menu->menu(),
		&st::menuIconStickers,
		tr::lng_context_details_pack_owner(tr::now),
		value,
		[=] {
			if (peer) {
				controller->showPeerInfo(peer);
			} else {
				QGuiApplication::clipboard()->setText(QString::number(ownerId));
				Ui::Toast::Show(tr::lng_code_copied(tr::now));
			}
		}));
}

void FillDetailsSubmenu(
		not_null<Ui::PopupMenu*> menu,
		HistoryItem *item,
		Element *view,
		not_null<Window::SessionController*> controller) {
	if (!item) {
		return;
	}
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	const auto views = item->Get<HistoryMessageViews>();
	const auto forwarded = item->Get<HistoryMessageForwarded>();
	const auto edited = item->Get<HistoryMessageEdited>();
	const auto emojiPacks = CollectEmojiPacks(not_null(item), EmojiPacksSource::Message);
	const auto singleEmojiPackId = (emojiPacks.size() == 1)
		? emojiPacks.front().id
		: uint64(0);
	const auto messageId = QString::number(item->id.bare);
	const auto messageDate = base::unixtime::parse(item->date());
	const auto messageEditTime = view
		? view->displayedEditDate()
		: (edited ? edited->date : TimeId(0));
	const auto messageForwardTime = forwarded ? forwarded->originalDate : TimeId(0);
	const auto messageEditDate = messageEditTime
		? FormatDetailsDateTime(base::unixtime::parse(messageEditTime))
		: QString();
	const auto messageForwardDate = messageForwardTime
		? FormatDetailsDateTime(base::unixtime::parse(messageForwardTime))
		: QString();
	const auto messageViews = (item->hasViews() && item->viewsCount() > 0)
		? QString::number(item->viewsCount())
		: QString();
	const auto messageShares = (views && views->forwardsCount > 0)
		? QString::number(views->forwardsCount)
		: QString();
	const auto mediaSize = MediaSizeText(item);
	const auto mediaMime = MediaMime(item);
	const auto mediaMimeName = [&] {
		if (mediaMime.isEmpty()) {
			return QString();
		}
		const auto mime = Core::MimeTypeForName(mediaMime);
		return mime.name().isEmpty() ? mediaMime : mime.name();
	}();
	const auto mediaName = MediaName(item);
	const auto mediaResolution = MediaResolution(item);
	const auto mediaDC = MediaDC(item);
	const auto videoCodec = VideoCodec(item);
	const auto hasAnyPostField = !messageViews.isEmpty() || !messageShares.isEmpty();
	const auto hasAnyMediaField = !mediaSize.isEmpty()
		|| !mediaMimeName.isEmpty()
		|| !mediaName.isEmpty()
		|| !mediaResolution.isEmpty()
		|| !mediaDC.isEmpty()
		|| !videoCodec.isEmpty();
	if (hasAnyPostField) {
		if (!messageViews.isEmpty()) {
			menu->addAction(CreateTwoTextAction(
				menu->menu(),
				&st::menuIconShowInChat,
				tr::lng_context_details_views(tr::now),
				messageViews));
		}
		if (!messageShares.isEmpty()) {
			menu->addAction(CreateTwoTextAction(
				menu->menu(),
				&st::menuIconViewReplies,
				tr::lng_context_details_shares(tr::now),
				messageShares));
		}
		menu->addSeparator(&st::expandedMenuSeparator);
	}
	menu->addAction(CreateTwoTextAction(
		menu->menu(),
		&st::menuIconInfo,
		tr::lng_context_details_id(tr::now),
		messageId));
	menu->addAction(CreateTwoTextAction(
		menu->menu(),
		&st::menuIconSchedule,
		tr::lng_context_details_date(tr::now),
		FormatDetailsDateTime(messageDate)));
	if (!messageEditDate.isEmpty()) {
		menu->addAction(CreateTwoTextAction(
			menu->menu(),
			&st::menuIconEdit,
			tr::lng_context_details_edit_date(tr::now),
			messageEditDate));
	}
	if (!messageForwardDate.isEmpty()) {
		menu->addAction(CreateTwoTextAction(
			menu->menu(),
			&st::menuIconTTL,
			tr::lng_context_details_forward_date(tr::now),
			messageForwardDate));
	}
	if (media && hasAnyMediaField) {
		menu->addSeparator(&st::expandedMenuSeparator);
		if (!mediaSize.isEmpty()) {
			menu->addAction(CreateTwoTextAction(
				menu->menu(),
				&st::menuIconDownload,
				tr::lng_context_details_file_size(tr::now),
				mediaSize));
		}
		if (!mediaMimeName.isEmpty()) {
			menu->addAction(CreateTwoTextAction(
				menu->menu(),
				&st::menuIconShowAll,
				tr::lng_context_details_mime_type(tr::now),
				mediaMimeName));
		}
		if (!mediaName.isEmpty()) {
			const auto shortName = (mediaName.size() > 20)
				? (u"…"_q + mediaName.mid(mediaName.size() - 20))
				: mediaName;
			menu->addAction(CreateTwoTextAction(
				menu->menu(),
				&st::menuIconEdit,
				tr::lng_context_details_file_name(tr::now),
				shortName,
				[=] {
					QGuiApplication::clipboard()->setText(mediaName);
				}));
		}
		if (!mediaResolution.isEmpty()) {
			menu->addAction(CreateTwoTextAction(
				menu->menu(),
				&st::menuIconStats,
				tr::lng_context_details_resolution(tr::now),
				mediaResolution));
		}
		if (!videoCodec.isEmpty()) {
			menu->addAction(CreateTwoTextAction(
				menu->menu(),
				&st::menuIconShowAll,
				tr::lng_context_details_video_codec(tr::now),
				videoCodec));
		}
		if (!mediaDC.isEmpty()) {
			menu->addAction(CreateTwoTextAction(
				menu->menu(),
				&st::menuIconBoosts,
				tr::lng_context_details_datacenter(tr::now),
				mediaDC));
		}
		if (document && document->sticker() && document->sticker()->set.id) {
			AddPackOwnerRow(menu, document->sticker()->set.id, controller);
		}
	}
	if (singleEmojiPackId
		&& (!document
			|| !document->sticker()
			|| document->sticker()->set.id != singleEmojiPackId)) {
		AddPackOwnerRow(menu, singleEmojiPackId, controller);
	}
}

void AddRateTranscribeAction(
		not_null<Ui::PopupMenu*> menu,
		HistoryItem *item) {
	if (!item || !Menu::HasRateTranscribeItem(item) || menu->empty()) {
		return;
	}
	menu->insertAction(
		RateTranscribeInsertIndex(menu),
		base::make_unique_q<Menu::RateTranscribe>(
			menu,
			menu->st().menu,
			Menu::RateTranscribeCallbackFactory(item)));
}

bool HasEditMessageAction(
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	const auto item = request.item;
	const auto context = list->elementContext();
	if (!item
		|| item->isSending()
		|| item->hasFailed()
		|| item->isEditingMedia()
		|| !request.selectedItems.empty()
		|| (context != Context::History
			&& context != Context::Replies
			&& context != Context::ShortcutMessages
			&& context != Context::ScheduledTopic
			&& context != Context::Monoforum)) {
		return false;
	}
	const auto peer = item->history()->peer;
	if (const auto channel = peer->asChannel()) {
		if (!channel->isMegagroup() && !channel->canEditMessages()) {
			return false;
		}
	}
	return true;
}

void SavePhotoToFile(not_null<PhotoData*> photo) {
	const auto media = photo->activeMediaView();
	if (photo->isNull() || !media || !media->loaded()) {
		return;
	}

	const auto image = media->image(Data::PhotoSize::Large)->original(); // clazy:exclude=unused-non-trivial-variable
	FileDialog::GetWritePath(
		Core::App().getFileDialogParent(),
		tr::lng_save_photo(tr::now),
		u"JPEG Image (*.jpg);;"_q + FileDialog::AllFilesFilter(),
		filedialogDefaultName(u"photo"_q, u".jpg"_q),
		crl::guard(&photo->session(), [=](const QString &result) {
			if (!result.isEmpty()) {
				media->saveToFile(result);
			}
		}));
}

void CopyImage(not_null<PhotoData*> photo) {
	const auto media = photo->activeMediaView();
	if (photo->isNull() || !media || !media->loaded()) {
		return;
	}
	media->setToClipboard();
}

void ShowStickerPackInfo(
		not_null<DocumentData*> document,
		not_null<ListWidget*> list) {
	StickerSetBox::Show(list->controller()->uiShow(), document);
}

void ToggleFavedSticker(
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document,
		FullMsgId contextId) {
	Api::ToggleFavedSticker(controller->uiShow(), document, contextId);
}

void AddPhotoActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<PhotoData*> photo,
		HistoryItem *item,
		not_null<ListWidget*> list) {
	const auto contextId = item ? item->fullId() : FullMsgId();
	if (!list->hasCopyMediaRestriction(item)) {
		menu->addAction(
			tr::lng_context_save_image(tr::now),
			base::fn_delayed(
				st::defaultDropdownMenu.menu.ripple.hideDuration,
				&photo->session(),
				[=] { SavePhotoToFile(photo); }),
			&st::menuIconSaveImage);
		menu->addAction(tr::lng_context_copy_image(tr::now), [=] {
			const auto item = photo->owner().message(contextId);
			if (!list->showCopyMediaRestriction(item)) {
				CopyImage(photo);
			}
		}, &st::menuIconCopy);
	}
	if (photo->hasAttachedStickers()) {
		const auto controller = list->controller();
		auto callback = [=] {
			auto &attached = photo->session().api().attachedStickers();
			attached.requestAttachedStickerSets(controller, photo);
		};
		menu->addAction(
			tr::lng_context_attached_stickers(tr::now),
			std::move(callback),
			&st::menuIconStickers);
	}
}

void SaveGif(
		not_null<Window::SessionController*> controller,
		FullMsgId itemId) {
	if (const auto item = controller->session().data().message(itemId)) {
		if (const auto media = item->media()) {
			if (const auto document = media->document()) {
				Api::ToggleSavedGif(
					controller->uiShow(),
					document,
					item->fullId(),
					true);
			}
		}
	}
}

void OpenGif(not_null<ListWidget*> list, FullMsgId itemId) {
	const auto controller = list->controller();
	if (const auto item = controller->session().data().message(itemId)) {
		if (const auto media = item->media()) {
			if (const auto document = media->document()) {
				list->elementOpenDocument(document, itemId, true);
			}
		}
	}
}

void ShowInFolder(not_null<DocumentData*> document) {
	const auto filepath = document->filepath(true);
	if (!filepath.isEmpty()) {
		File::ShowInFolder(filepath);
	}
}

void AddDocumentActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<DocumentData*> document,
		HistoryItem *item,
		not_null<ListWidget*> list) {
	if (document->loading()) {
		menu->addAction(tr::lng_context_cancel_download(tr::now), [=] {
			document->cancel();
		}, &st::menuIconCancel);
		return;
	}
	const auto controller = list->controller();
	const auto contextId = item ? item->fullId() : FullMsgId();
	const auto session = &document->session();
	if (item && document->isGifv()) {
		const auto notAutoplayedGif = !Data::AutoDownload::ShouldAutoPlay(
			document->session().settings().autoDownload(),
			item->history()->peer,
			document);
		if (notAutoplayedGif) {
			const auto weak = base::make_weak(list.get());
			menu->addAction(tr::lng_context_open_gif(tr::now), [=] {
				if (const auto strong = weak.get()) {
					OpenGif(strong, contextId);
				}
			}, &st::menuIconShowInChat);
		}
		if (!list->hasCopyMediaRestriction(item)) {
			menu->addAction(tr::lng_context_save_gif(tr::now), [=] {
				SaveGif(list->controller(), contextId);
			}, &st::menuIconGif);
		}
	}
	if (document->sticker() && document->sticker()->set) {
		menu->addAction(
			(document->isStickerSetInstalled()
				? tr::lng_context_pack_info(tr::now)
				: tr::lng_context_pack_add(tr::now)),
			[=] { ShowStickerPackInfo(document, list); },
			&st::menuIconStickers);
		const auto isFaved = document->owner().stickers().isFaved(document);
		menu->addAction(
			(isFaved
				? tr::lng_faved_stickers_remove(tr::now)
				: tr::lng_faved_stickers_add(tr::now)),
			[=] { ToggleFavedSticker(controller, document, contextId); },
			isFaved ? &st::menuIconUnfave : &st::menuIconFave);
	}
	if (!document->filepath(true).isEmpty()) {
		menu->addAction(
			(Platform::IsMac()
				? tr::lng_context_show_in_finder(tr::now)
				: tr::lng_context_show_in_folder(tr::now)),
			[=] { ShowInFolder(document); },
			&st::menuIconShowInFolder);
	}
	if (document->hasAttachedStickers()) {
		const auto controller = list->controller();
		auto callback = [=] {
			auto &attached = session->api().attachedStickers();
			attached.requestAttachedStickerSets(controller, document);
		};
		menu->addAction(
			tr::lng_context_attached_stickers(tr::now),
			std::move(callback),
			&st::menuIconStickers);
	}
	if (item && !list->hasCopyMediaRestriction(item)) {
		const auto controller = list->controller();
		AddSaveSoundForNotifications(menu, item, document, controller);
	}
	AddSaveDocumentAction(menu, item, document, list);
	AddCopyFilename(
		menu,
		document,
		[=] { return list->showCopyRestrictionForSelected(); });
}

void AddPostLinkAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request) {
		if (!GetEnhancedBool("show_message_context_copy_link")) {
			return;
		}
		const auto item = request.item;
		if (!item
			|| !item->hasDirectLink()
		|| request.pointState == PointState::Outside) {
		return;
	} else if (request.link
		&& !request.link->copyToClipboardContextItemText().isEmpty()) {
		return;
	}
	const auto itemId = item->fullId();
	const auto context = request.view
		? request.view->context()
		: Context::History;
	const auto controller = request.navigation->parentController();
	menu->insertAction(0, base::make_unique_q<Ui::Menu::Action>(
		menu->menu(),
		menu->st().menu,
		Ui::Menu::CreateAction(
			menu->menu(),
			(item->history()->peer->isMegagroup()
				? tr::lng_context_copy_message_link
				: tr::lng_context_copy_post_link)(tr::now),
			[=] { CopyPostLink(controller, itemId, context); }),
		&st::menuIconLink,
		&st::menuIconLink));
}

MessageIdsList ExtractIdsList(const SelectedItems &items) {
	return ranges::views::all(
		items
	) | ranges::views::transform(
		&SelectedItem::msgId
	) | ranges::to_vector;
}

bool AddForwardSelectedAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_forward")) {
			return false;
		}
		if (!request.overSelection || request.selectedItems.empty()) {
			return false;
		}
	if (!ranges::all_of(request.selectedItems, &SelectedItem::canForward)) {
		return false;
	}

	menu->addAction(tr::lng_context_forward_selected(tr::now), [=] {
		const auto weak = base::make_weak(list);
		Window::ShowNewForwardMessagesBox(
			request.navigation,
			ExtractIdsList(request.selectedItems),
			false,
			[=] {
				if (const auto strong = weak.get()) {
					strong->cancelSelection();
				}
			});
	}, &st::menuIconForward);
	menu->addAction(tr::lng_context_forward_selected_no_quote(tr::now), [=] {
		const auto weak = base::make_weak(list);
		Window::ShowNewForwardMessagesBox(
				request.navigation,
				ExtractIdsList(request.selectedItems),
				true,
				[=] {
					if (const auto strong = weak.get()) {
						strong->cancelSelection();
					}
				});
	}, &st::menuIconForward);
	menu->addAction(tr::lng_forward_to_saved_message(tr::now), [=] {
		const auto weak = base::make_weak(list);
		const auto items = ExtractIdsList(request.selectedItems);
		const auto item = request.navigation->session().data().message(items[0]);
		const auto api = &item->history()->peer->session().api();
		const auto session = &item->history()->peer->session();
		const auto self = api->session().user()->asUser();
		auto msgItems = session->data().idsToItems(items);

		auto action = Api::SendAction(item->history()->peer->owner().history(self));
		action.clearDraft = false;
		action.generateLocal = false;

		const auto history = item->history()->peer->owner().history(self);
		auto resolved = history->resolveForwardDraft(Data::ForwardDraft{ .ids = items });

		api->forwardMessages(std::move(resolved), action, [=] {
			Ui::Toast::Show(tr::lng_share_done(tr::now));

			if (const auto strong = weak.get()) {
				strong->cancelSelection();
			}
		});
	}, &st::menuIconFave);
	return true;
}

bool AddForwardMessageAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_forward")) {
			return false;
		}
		const auto item = request.item;
		if (!request.selectedItems.empty()) {
			return false;
	} else if (!item || !item->allowsForward()) {
		return false;
	}
	const auto owner = &item->history()->owner();
	const auto asGroup = (request.pointState != PointState::GroupPart);
	if (asGroup) {
		if (const auto group = owner->groups().find(item)) {
			if (!ranges::all_of(group->items, &HistoryItem::allowsForward)) {
				return false;
			}
		}
	}
	const auto itemId = item->fullId();
	auto fwdSubmenu = std::make_unique<Ui::PopupMenu>(list, st::popupMenuWithIcons);
	fwdSubmenu->addAction(tr::lng_context_forward_msg_old(tr::now), [=] {
		if (const auto item = owner->message(itemId)) {
			const auto weak = base::make_weak(list);
			Window::ShowForwardMessagesBox(
				request.navigation,
				(asGroup
					? owner->itemOrItsGroup(item)
					: MessageIdsList{ 1, itemId }),
				[=] {
					if (const auto strong = weak.get()) {
						strong->cancelSelection();
					}
				});
		}
		}, &st::menuIconForward);
	fwdSubmenu->addAction(tr::lng_context_forward_msg(tr::now), [=] {
		if (const auto item = owner->message(itemId)) {
			const auto weak = base::make_weak(list);
			Window::ShowNewForwardMessagesBox(
				request.navigation,
				(asGroup
					? owner->itemOrItsGroup(item)
					: MessageIdsList{ 1, itemId }), false,
				[=] {
					if (const auto strong = weak.get()) {
						strong->cancelSelection();
					}
				});
		}
	}, &st::menuIconForward);
	fwdSubmenu->addAction(tr::lng_context_forward_msg_no_quote(tr::now), [=] {
		if (const auto item = owner->message(itemId)) {
			const auto weak = base::make_weak(list);
			Window::ShowNewForwardMessagesBox(
					request.navigation,
					(asGroup
					 ? owner->itemOrItsGroup(item)
					 : MessageIdsList{ 1, itemId }),
					true, 
					[=] {
					if (const auto strong = weak.get()) {
						strong->cancelSelection();
					}
				});
		}
	}, &st::menuIconForward);
	if (item->allowsForward()) {
		fwdSubmenu->addAction(tr::lng_forward_to_saved_message(tr::now), [=] {
			if (item->id <= 0) return;
			const auto api = &item->history()->peer->session().api();
			auto action = Api::SendAction(item->history()->peer->owner().history(api->session().user()->asUser()));
			action.clearDraft = false;
			action.generateLocal = false;

			const auto history = item->history()->peer->owner().history(api->session().user()->asUser());
			auto resolved = history->resolveForwardDraft(Data::ForwardDraft{ .ids = MessageIdsList(1, itemId) });

			api->forwardMessages(std::move(resolved), action, [] {
				Ui::Toast::Show(tr::lng_share_done(tr::now));
				});
			}, &st::menuIconFave);
	}
	if (!fwdSubmenu->empty()) {
		menu->addAction(tr::lng_context_forward(tr::now), std::move(fwdSubmenu), &st::menuIconForward);
	}
	return true;
}

void AddMsgsFromUserAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest& request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_show_messages_from")) {
			return;
		}
		const auto item = request.item;
		if (!request.selectedItems.empty() || !item) {
			return;
	}
	const auto peer = item->history()->peer;
	if (peer->isChat() || peer->isMegagroup()) {
		const auto msgSigned = item->Get<HistoryMessageSigned>();
		if (msgSigned) {
			menu->addAction(tr::lng_context_show_messages_from(tr::now), [=] {
				App::searchByHashtag(msgSigned->author, peer, item->from());
			}, &st::menuIconInfo);
		} else {
			menu->addAction(tr::lng_context_show_messages_from(tr::now), [=] {
				App::searchByHashtag(QString(), peer, item->from());
			}, &st::menuIconInfo);
		}
	}
}

Api::SendAction prepareSendAction(
		History *history, Api::SendOptions options) {
	auto result = Api::SendAction(history, options);
	result.replyTo = FullReplyTo();
	if (history->peer->isUser()) {
		result.options.sendAs = nullptr;
	}
	return result;
}

void AddRepeaterAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest& request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_repeater")) {
			return;
		}
		const auto item = request.item;
		if (!request.selectedItems.empty() || !item) {
			return;
	}
	const auto itemId = item->fullId();
	const auto _history = item->history();
	auto repeatSubmenu = std::make_unique<Ui::PopupMenu>(list, st::popupMenuWithIcons);
	if ((item->history()->peer->isMegagroup() || item->history()->peer->isChat() || item->history()->peer->isUser())) {
		if (GetEnhancedBool("show_repeater_option")) {
			if (item->allowsForward()) {
				repeatSubmenu->addAction(tr::lng_context_repeat_msg(tr::now), [=] {
					if (item->id <= 0) return;
					const auto api = &item->history()->peer->session().api();
					auto action = Api::SendAction(item->history()->peer->owner().history(item->history()->peer), Api::SendOptions{ .sendAs = _history->session().sendAsPeers().resolveChosen(_history->peer) });
					action.clearDraft = false;
					if (item->history()->peer->isUser() || item->history()->peer->isChat() || item->history()->peer->isMonoforum()) {
						action.options.sendAs = nullptr;
					}

					if (item->topic()) {
						action.replyTo = FullReplyTo{
											.messageId = item->fullId(),
											.topicRootId = item->topicRootId(),
										};
					}

					if (const auto sublist = item->savedSublist()) {
						action.replyTo.monoforumPeerId = sublist->monoforumPeerId();
					}

					const auto history = item->history()->peer->owner().history(item->history()->peer);
					auto resolved = history->resolveForwardDraft(Data::ForwardDraft{ .ids = MessageIdsList(1, itemId) });

					api->forwardMessages(std::move(resolved), action, [] {
						Ui::Toast::Show(tr::lng_share_done(tr::now));
					});
				}, &st::menuIconDiscussion);
			}
			if (!item->isService() && !item->emptyText() && item->media() == nullptr) {
				repeatSubmenu->addAction(tr::lng_context_repeat_msg_no_fwd(tr::now), [=] {
					if (item->id <= 0) return;
					const auto api = &item->history()->peer->session().api();
					auto message = ApiWrap::MessageToSend(prepareSendAction(_history->peer->owner().history(item->history()->peer), Api::SendOptions{ .sendAs = _history->session().sendAsPeers().resolveChosen(_history->peer) }));
					message.textWithTags = { item->originalText().text,TextUtilities::ConvertEntitiesToTextTags(item->originalText().entities) };
					if (item->history()->peer->isUser() || item->history()->peer->isChat() || item->history()->peer->isMonoforum()) {
						message.action.options.sendAs = nullptr;
					}
					if (item->topic()) {
						message.action.replyTo = FullReplyTo{
													.messageId = item->fullId(),
													.topicRootId = item->topicRootId(),
												};
					}
					if (GetEnhancedBool("repeater_reply_to_orig_msg")) {
						message.action.replyTo.messageId = item->fullId();
					}
					if (const auto sublist = item->savedSublist()) {
						message.action.replyTo.monoforumPeerId = sublist->monoforumPeerId();
					}
					api->sendMessage(std::move(message));
				}, &st::menuIconDiscussion);
			}
			else if (!item->isService() && item->media()->document() != nullptr && item->media()->document()->sticker() != nullptr) {
				if (item->allowsForward()) {
					repeatSubmenu->addAction(tr::lng_context_repeat_msg_no_fwd(tr::now), [=] {
						if (item->id <= 0) return;
						const auto api = &item->history()->peer->session().api();
						auto action = Api::SendAction(item->history()->peer->owner().history(item->history()->peer), Api::SendOptions{ .sendAs = _history->session().sendAsPeers().resolveChosen(_history->peer) });
						action.clearDraft = false;
						if (item->history()->peer->isUser() || item->history()->peer->isChat() || item->history()->peer->isMonoforum()) {
							action.options.sendAs = nullptr;
						}
						if (item->topic()) {
							action.replyTo = FullReplyTo{
												.messageId = item->fullId(),
												.topicRootId = item->topicRootId(),
											};
						}
						if (GetEnhancedBool("repeater_reply_to_orig_msg")) {
							action.replyTo.messageId = item->fullId();
						}
						if (const auto sublist = item->savedSublist()) {
							action.replyTo.monoforumPeerId = sublist->monoforumPeerId();
						}

						const auto history = item->history()->peer->owner().history(item->history()->peer);
						auto resolved = history->resolveForwardDraft(Data::ForwardDraft{ .ids = MessageIdsList(1, itemId), .options = Data::ForwardOptions::NoSenderNames });

						api->forwardMessages(std::move(resolved), action, [] {
							Ui::Toast::Show(tr::lng_share_done(tr::now));
						});
						}, &st::menuIconDiscussion);
				}
				else {
					repeatSubmenu->addAction(tr::lng_context_repeat_msg_no_fwd(tr::now), [=] {
						if (item->id <= 0) return;
						const auto document = item->media()->document();
						const auto history = item->history()->peer->owner().history(item->history()->peer);
						auto message = ApiWrap::MessageToSend(prepareSendAction(history, Api::SendOptions{ .sendAs = _history->session().sendAsPeers().resolveChosen(_history->peer) }));
						if (item->history()->peer->isUser() || item->history()->peer->isChat() || item->history()->peer->isMonoforum()) {
							message.action.options.sendAs = nullptr;
						}
						if (item->topic()) {
							message.action.replyTo = FullReplyTo{
														.messageId = item->fullId(),
														.topicRootId = item->topicRootId(),
													};
						}
						if (const auto sublist = item->savedSublist()) {
							message.action.replyTo.monoforumPeerId = sublist->monoforumPeerId();
						}
						Api::SendExistingDocument(std::move(message), document);
					}, &st::menuIconDiscussion);
				}
			}
			if (GetEnhancedBool("show_repeater_option") && !repeatSubmenu->empty()) {
				menu->addAction(tr::lng_context_repeater(tr::now), std::move(repeatSubmenu), &st::menuIconDiscussion);
			}
		}
	}
}

void AddForwardAction(
		not_null<Ui::PopupMenu*> menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	AddForwardSelectedAction(menu, request, list);
	AddForwardMessageAction(menu, request, list);
}

bool AddSendNowSelectedAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_send_now")) {
			return false;
		}
		if (!request.overSelection || request.selectedItems.empty()) {
			return false;
		}
	if (!ranges::all_of(request.selectedItems, &SelectedItem::canSendNow)) {
		return false;
	}

	const auto session = &request.navigation->session();
	auto histories = ranges::views::all(
		request.selectedItems
	) | ranges::views::transform([&](const SelectedItem &item) {
		return session->data().message(item.msgId);
	}) | ranges::views::filter([](HistoryItem *item) {
		return item != nullptr;
	}) | ranges::views::transform(
		&HistoryItem::history
	);
	if (histories.begin() == histories.end()) {
		return false;
	}
	const auto history = *histories.begin();

	menu->addAction(tr::lng_context_send_now_selected(tr::now), [=] {
		const auto weak = base::make_weak(list);
		const auto callback = [=] {
			request.navigation->showBackFromStack();
		};
		Window::ShowSendNowMessagesBox(
			request.navigation,
			history,
			ExtractIdsList(request.selectedItems),
			callback);
	}, &st::menuIconSend);
	return true;
}

bool AddSendNowMessageAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_send_now")) {
			return false;
		}
		const auto item = request.item;
		if (!request.selectedItems.empty()) {
			return false;
	} else if (!item || !item->allowsSendNow()) {
		return false;
	}
	const auto owner = &item->history()->owner();
	const auto asGroup = (request.pointState != PointState::GroupPart);
	if (asGroup) {
		if (const auto group = owner->groups().find(item)) {
			if (!ranges::all_of(group->items, &HistoryItem::allowsSendNow)) {
				return false;
			}
		}
	}
	const auto itemId = item->fullId();
	menu->addAction(tr::lng_context_send_now_msg(tr::now), [=] {
		if (const auto item = owner->message(itemId)) {
			Window::ShowSendNowMessagesBox(
				request.navigation,
				item->history(),
				(asGroup
					? owner->itemOrItsGroup(item)
					: MessageIdsList{ 1, itemId }));
		}
	}, &st::menuIconSend);
	return true;
}

bool AddRescheduleAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_reschedule")) {
			return false;
		}
		const auto owner = &request.navigation->session().data();

	const auto goodSingle = HasEditMessageAction(request, list)
		&& request.item->allowsReschedule();
	const auto goodMany = [&] {
		if (goodSingle) {
			return false;
		}
		const auto &items = request.selectedItems;
		if (!request.overSelection || items.empty()) {
			return false;
		}
		if (items.size() > kRescheduleLimit) {
			return false;
		}
		return ranges::all_of(items, &SelectedItem::canReschedule);
	}();
	if (!goodSingle && !goodMany) {
		return false;
	}
	auto ids = goodSingle
		? MessageIdsList{ request.item->fullId() }
		: ExtractIdsList(request.selectedItems);
	ranges::sort(ids, [&](const FullMsgId &a, const FullMsgId &b) {
		const auto itemA = owner->message(a);
		const auto itemB = owner->message(b);
		return (itemA && itemB) && (itemA->position() < itemB->position());
	});

	auto text = ((ids.size() == 1)
		? tr::lng_context_reschedule
		: tr::lng_context_reschedule_selected)(tr::now);

	menu->addAction(std::move(text), [=] {
		const auto firstItem = owner->message(ids.front());
		if (!firstItem) {
			return;
		}
		const auto callback = [=](Api::SendOptions options) {
			list->cancelSelection();
			auto groupedIds = std::vector<MessageGroupId>();
			for (const auto &id : ids) {
				const auto item = owner->message(id);
				if (!item || !item->isScheduled()) {
					continue;
				}
				if (const auto groupId = item->groupId()) {
					if (ranges::contains(groupedIds, groupId)) {
						continue;
					}
					groupedIds.push_back(groupId);
				}
				Api::RescheduleMessage(item, options);
				// Increase the scheduled date by 1s to keep the order.
				options.scheduled += 1;
			}
		};

		const auto peer = firstItem->history()->peer;
		const auto sendMenuType = !peer
			? SendMenu::Type::Disabled
			: peer->starsPerMessageChecked()
			? SendMenu::Type::SilentOnly
			: peer->isSelf()
			? SendMenu::Type::Reminder
			: HistoryView::CanScheduleUntilOnline(peer)
			? SendMenu::Type::ScheduledToUser
			: SendMenu::Type::Disabled;

		const auto itemDate = firstItem->date();
		const auto date = (itemDate == Api::kScheduledUntilOnlineTimestamp)
			? HistoryView::DefaultScheduleTime()
			: itemDate + (firstItem->isScheduled() ? 0 : crl::time(600));
		const auto repeatPeriod = firstItem->scheduleRepeatPeriod();

		const auto box = request.navigation->parentController()->show(
			HistoryView::PrepareScheduleBox(
				&request.navigation->session(),
				request.navigation->uiShow(),
				{ .type = sendMenuType, .effectAllowed = false },
				callback,
				{ .scheduleRepeatPeriod = repeatPeriod },
				date));

		owner->itemRemoved(
		) | rpl::on_next([=](not_null<const HistoryItem*> item) {
			if (ranges::contains(ids, item->fullId())) {
				box->closeBox();
			}
		}, box->lifetime());
	}, &st::menuIconReschedule);
	return true;
}

void AddViewJSONAction(
		not_null<Ui::PopupMenu*> menu,
		const ContextMenuRequest& request,
		not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_json")) {
			return;
		}
		const auto item = request.item;
		if (item == nullptr) {
			return;
		}
	if (!request.selectedItems.empty()) {
		return;
	}
	const auto controller = list->controller();
	const auto itemId = item->fullId();
	menu->addAction(tr::lng_context_view_as_json(tr::now), [=] {
		HistoryView::ViewAsJSON(controller, itemId);
	}, &st::menuIcon64gJson);
}

bool AddReplyToMessageAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_reply")) {
			return false;
		}
		const auto context = list->elementContext();
		const auto item = request.quote.item
			? request.quote.item
		: request.item;
	const auto topic = item ? item->topic() : nullptr;
	const auto peer = item ? item->history()->peer.get() : nullptr;
	if (!item
		|| !item->isRegular()
		|| (context != Context::History
			&& context != Context::Replies
			&& context != Context::Monoforum)) {
		return false;
	}
	const auto canSendReply = topic
		? Data::CanSendAnything(topic)
		: Data::CanSendAnything(peer);
	const auto canReply = canSendReply || item->allowsForward();
	if (!canReply) {
		return false;
	}

	const auto todoListTaskId = request.link
		? request.link->property(kTodoListItemIdProperty).toInt()
		: 0;
	const auto &quote = request.quote;
	auto text = (todoListTaskId
		? tr::lng_context_reply_to_task
		: quote.highlight.quote.empty()
		? tr::lng_context_reply_msg
		: tr::lng_context_quote_and_reply)(
			tr::now,
			Ui::Text::FixAmpersandInAction);
	menu->addAction(std::move(text), [=, itemId = item->fullId()] {
		list->replyToMessageRequestNotify({
			.messageId = itemId,
			.quote = quote.highlight.quote,
			.quoteOffset = quote.highlight.quoteOffset,
			.todoItemId = todoListTaskId,
		}, base::IsCtrlPressed());
	}, &st::menuIconReply);
	return true;
}

bool AddTodoListAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request,
			not_null<ListWidget*> list) {
	const auto context = list->elementContext();
	const auto item = request.item;
	if (!item
		|| !Window::PeerMenuShowAddTodoListTasks(item)
		|| (context != Context::History
			&& context != Context::Replies
			&& context != Context::Monoforum
			&& context != Context::Pinned)) {
		return false;
		}
		const auto itemId = item->fullId();
		const auto controller = list->controller();
		auto added = false;
		if (GetEnhancedBool("show_message_context_edit")) {
			menu->addAction(tr::lng_context_edit_msg(tr::now), [=] {
				if (const auto item = controller->session().data().message(itemId)) {
					Window::PeerMenuEditTodoList(controller, item);
				}
			}, &st::menuIconEdit);
			added = true;
		}
		if (GetEnhancedBool("show_message_context_add_task")) {
			menu->addAction(tr::lng_todo_add_title(tr::now), [=] {
				if (const auto item = controller->session().data().message(itemId)) {
					Window::PeerMenuAddTodoListTasks(controller, item);
				}
			}, &st::menuIconAdd);
			added = true;
		}
		return added;
	}

bool AddViewRepliesAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_view_replies")) {
			return false;
		}
		const auto context = list->elementContext();
		const auto item = request.item;
	if (!item
		|| !item->isRegular()
		|| (context != Context::History && context != Context::Pinned)) {
		return false;
	}
	const auto topicRootId = item->history()->isForum()
		? item->topicRootId()
		: 0;
	const auto repliesCount = item->repliesCount();
	const auto withReplies = (repliesCount > 0);
	if (!withReplies || !item->history()->peer->isMegagroup()) {
		if (!topicRootId) {
			return false;
		}
	}
	const auto rootId = topicRootId
		? topicRootId
		: repliesCount
		? item->id
		: item->replyToTop();
	const auto highlightId = topicRootId ? item->id : 0;
	const auto phrase = topicRootId
		? tr::lng_replies_view_topic(tr::now)
		: (repliesCount > 0)
		? tr::lng_replies_view(
			tr::now,
			lt_count,
			repliesCount)
		: tr::lng_replies_view_thread(tr::now);
	const auto controller = list->controller();
	const auto history = item->history();
	menu->addAction(phrase, crl::guard(controller, [=] {
		controller->showRepliesForMessage(
			history,
			rootId,
			highlightId);
	}), &st::menuIconViewReplies);
	return true;
}

bool AddEditMessageAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_edit")) {
			return false;
		}
		if (!HasEditMessageAction(request, list)) {
			return false;
		}
	const auto item = request.item;
	if (!item->allowsEdit(base::unixtime::now())) {
		return false;
	}
	const auto owner = &item->history()->owner();
	const auto itemId = item->fullId();
	menu->addAction(tr::lng_context_edit_msg(tr::now), [=] {
		const auto item = owner->message(itemId);
		if (!item) {
			return;
		}
		list->editMessageRequestNotify(item->fullId());
	}, &st::menuIconEdit);
	return true;
}

void AddFactcheckAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_factcheck")) {
			return;
		}
		const auto item = request.item;
		if (!item || !item->history()->session().factchecks().canEdit(item)) {
			return;
	}
	const auto itemId = item->fullId();
	const auto text = item->factcheckText();
	const auto session = &item->history()->session();
	const auto phrase = text.empty()
		? tr::lng_context_add_factcheck(tr::now)
		: tr::lng_context_edit_factcheck(tr::now);
	menu->addAction(phrase, [=] {
		const auto limit = session->factchecks().lengthLimit();
		const auto controller = request.navigation->parentController();
		controller->show(Box(EditFactcheckBox, text, limit, [=](
				TextWithEntities result) {
			const auto show = controller->uiShow();
			session->factchecks().save(itemId, text, result, show);
		}, FactcheckFieldIniter(controller->uiShow())));
	}, &st::menuIconFactcheck);
}

bool AddPinMessageAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_pin")) {
			return false;
		}
		const auto context = list->elementContext();
		const auto item = request.item;
	if (!item || !item->isRegular()) {
		return false;
	}
	const auto topic = item->topic();
	const auto sublist = item->savedSublist();
	if (context != Context::History && context != Context::Pinned) {
		if ((context != Context::Replies || !topic)
			&& (context != Context::Monoforum
				|| !sublist
				|| !item->history()->amMonoforumAdmin())) {
			return false;
		}
	}
	const auto group = item->history()->owner().groups().find(item);
	const auto pinItem = ((item->canPin() && item->isPinned()) || !group)
		? item
		: group->items.front().get();
	if (!pinItem->canPin()) {
		return false;
	}
	const auto pinItemId = pinItem->fullId();
	const auto isPinned = pinItem->isPinned();
	const auto controller = list->controller();
	menu->addAction(isPinned ? tr::lng_context_unpin_msg(tr::now) : tr::lng_context_pin_msg(tr::now), crl::guard(controller, [=] {
		Window::ToggleMessagePinned(controller, pinItemId, !isPinned);
	}), isPinned ? &st::menuIconUnpin : &st::menuIconPin);
	return true;
}

bool AddGoToMessageAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_go_to_message")) {
			return false;
		}
		const auto context = list->elementContext();
		const auto view = request.view;
	if (!view
		|| !view->data()->isRegular()
		|| context != Context::Pinned
		|| !view->hasOutLayout()) {
		return false;
	}
	const auto itemId = view->data()->fullId();
	const auto controller = list->controller();
	menu->addAction(tr::lng_context_to_msg(tr::now), crl::guard(controller, [=] {
		if (const auto item = controller->session().data().message(itemId)) {
			controller->showMessage(item);
		}
	}), &st::menuIconShowInChat);
	return true;
}

void AddSendNowAction(
		not_null<Ui::PopupMenu*> menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	AddSendNowSelectedAction(menu, request, list);
	AddSendNowMessageAction(menu, request, list);
}

bool AddDeleteSelectedAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_delete")) {
			return false;
		}
		if (!request.overSelection || request.selectedItems.empty()) {
			return false;
		}
	if (!ranges::all_of(request.selectedItems, &SelectedItem::canDelete)) {
		return false;
	}

	menu->addAction(tr::lng_context_delete_selected(tr::now), [=] {
		auto items = ExtractIdsList(request.selectedItems);
		auto box = Box<DeleteMessagesBox>(
			&request.navigation->session(),
			std::move(items));
		box->setDeleteConfirmedCallback(crl::guard(list, [=] {
			list->cancelSelection();
		}));
		request.navigation->parentController()->show(std::move(box));
	}, &st::menuIconDelete);
	return true;
}

bool AddDeleteMessageAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_delete")) {
			return false;
		}
		const auto item = request.item;
		if (!request.selectedItems.empty()) {
			return false;
	} else if (!item || !item->canDelete()) {
		return false;
	}
	const auto owner = &item->history()->owner();
	const auto asGroup = (request.pointState != PointState::GroupPart);
	if (asGroup) {
		if (const auto group = owner->groups().find(item)) {
			if (ranges::any_of(group->items, [](auto item) {
				return item->isLocal() || !item->canDelete();
			})) {
				return false;
			}
		}
	}
	const auto controller = list->controller();
	const auto itemId = item->fullId();
	const auto callback = crl::guard(controller, [=] {
		if (const auto item = owner->message(itemId)) {
			if (asGroup) {
				if (const auto group = owner->groups().find(item)) {
					controller->show(Box<DeleteMessagesBox>(
						&owner->session(),
						owner->itemsToIds(group->items)));
					return;
				}
			}
			if (item->isUploading()) {
				controller->cancelUploadLayer(item);
				return;
			}
			const auto list = HistoryItemsList{ item };
			if (CanCreateModerateMessagesBox(list)) {
				const auto opt = DefaultModerateMessagesBoxOptions();
				controller->show(
					Box(CreateModerateMessagesBox, list, nullptr, opt));
			} else {
				const auto suggestModerateActions = false;
				controller->show(
					Box<DeleteMessagesBox>(item, suggestModerateActions));
			}
		}
	});
	if (item->isUploading()) {
		menu->addAction(
			tr::lng_context_cancel_upload(tr::now),
			callback,
			&st::menuIconCancel);
		return true;
	}
	menu->addAction(Ui::DeleteMessageContextAction(
		menu->menu(),
		callback,
		item->ttlDestroyAt(),
		[=] { delete menu; }));
	return true;
}

void AddDeleteAction(
		not_null<Ui::PopupMenu*> menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	if (!AddDeleteSelectedAction(menu, request, list)) {
		AddDeleteMessageAction(menu, request, list);
	}
}

void AddDownloadFilesAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_save_as")) {
			return;
		}
		if (!request.overSelection
			|| request.selectedItems.empty()
			|| list->hasCopyRestrictionForSelected()) {
		return;
	}
	Menu::AddDownloadFilesAction(
		menu,
		request.navigation->parentController(),
		request.selectedItems,
		list);
}

void AddReportAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_report")) {
			return;
		}
		const auto item = request.item;
		if (!request.selectedItems.empty()) {
			return;
	} else if (!item || !item->suggestReport()) {
		return;
	}
	const auto owner = &item->history()->owner();
	const auto controller = list->controller();
	const auto itemId = item->fullId();
	const auto callback = crl::guard(controller, [=] {
		if (const auto item = owner->message(itemId)) {
			const auto group = owner->groups().find(item);
			const auto ids = group
				? (ranges::views::all(
					group->items
				) | ranges::views::transform([](const auto &i) {
					return i->fullId().msg;
				}) | ranges::to_vector)
				: std::vector<MsgId>{ 1, itemId.msg };
			const auto peer = item->history()->peer;
			ShowReportMessageBox(controller->uiShow(), peer, ids, {});
		}
	});
	menu->addAction(
		tr::lng_context_report_msg(tr::now),
		callback,
		&st::menuIconReport);
}

bool AddClearSelectionAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_select")) {
			return false;
		}
		if (!request.overSelection || request.selectedItems.empty()) {
			return false;
		}
	menu->addAction(tr::lng_context_clear_selection(tr::now), [=] {
		list->cancelSelection();
	}, &st::menuIconSelect);
	return true;
}

bool AddSelectMessageAction(
			not_null<Ui::PopupMenu*> menu,
			const ContextMenuRequest &request,
			not_null<ListWidget*> list) {
		if (!GetEnhancedBool("show_message_context_select")) {
			return false;
		}
		const auto item = request.item;
		if (request.overSelection && !request.selectedItems.empty()) {
			return false;
	} else if (!item
		|| item->isLocal()
		|| item->isService()
		|| list->hasSelectRestriction()) {
		return false;
	}
	const auto owner = &item->history()->owner();
	const auto itemId = item->fullId();
	const auto asGroup = (request.pointState != PointState::GroupPart);
	menu->addAction(tr::lng_context_select_msg(tr::now), [=] {
		if (const auto item = owner->message(itemId)) {
			if (asGroup) {
				list->selectItemAsGroup(item);
			} else {
				list->selectItem(item);
			}
		}
	}, &st::menuIconSelect);
	return true;
}

void AddSelectionAction(
		not_null<Ui::PopupMenu*> menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	if (!AddClearSelectionAction(menu, request, list)) {
		AddSelectMessageAction(menu, request, list);
	}
}

void AddTopMessageActions(
		not_null<Ui::PopupMenu*> menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	AddGoToMessageAction(menu, request, list);
	AddViewRepliesAction(menu, request, list);
	AddEditMessageAction(menu, request, list);
	AddFactcheckAction(menu, request, list);
	AddPinMessageAction(menu, request, list);
}

void AddMessageActions(
		not_null<Ui::PopupMenu*> menu,
		const ContextMenuRequest &request,
		not_null<ListWidget*> list) {
	AddPostLinkAction(menu, request);
	AddMsgsFromUserAction(menu, request, list);
	AddForwardAction(menu, request, list);
	AddRepeaterAction(menu, request, list);
	AddSendNowAction(menu, request, list);
	AddDeleteAction(menu, request, list);
	AddDownloadFilesAction(menu, request, list);
	AddReportAction(menu, request, list);
	AddSelectionAction(menu, request, list);
	AddRescheduleAction(menu, request, list);
	AddViewJSONAction(menu, request, list);
}

void AddCopyLinkAction(
		not_null<Ui::PopupMenu*> menu,
		const ClickHandlerPtr &link) {
	if (!link) {
		return;
	}
	const auto action = link->copyToClipboardContextItemText();
	if (action.isEmpty()) {
		return;
	}
	const auto text = link->copyToClipboardText();
	menu->addAction(
		action,
		[=] { QGuiApplication::clipboard()->setText(text); },
		&st::menuIconCopy);
}

void EditTagBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		const Data::ReactionId &id) {
	const auto owner = &controller->session().data();
	const auto title = owner->reactions().myTagTitle(id);
	box->setTitle(title.isEmpty()
		? tr::lng_context_tag_add_name()
		: tr::lng_context_tag_edit_name());
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		tr::lng_edit_tag_about(),
		st::editTagAbout));
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::editTagField,
		tr::lng_edit_tag_name(),
		title));
	field->setMaxLength(kTagNameLimit * 2);
	box->setFocusCallback([=] {
		field->setFocusFast();
	});

	struct State {
		std::unique_ptr<Ui::Text::CustomEmoji> custom;
		QImage image;
	};
	const auto state = field->lifetime().make_state<State>();

	if (const auto customId = id.custom()) {
		state->custom = owner->customEmojiManager().create(
			customId,
			[=] { field->update(); });
	} else {
		owner->reactions().preloadReactionImageFor(id);
	}
	field->paintRequest() | rpl::on_next([=](QRect clip) {
		auto p = QPainter(field);
		const auto top = st::editTagField.textMargins.top();
		if (const auto custom = state->custom.get()) {
			const auto inactive = !field->window()->isActiveWindow();
			custom->paint(p, {
				.textColor = st::windowFg->c,
				.now = crl::now(),
				.position = QPoint(0, top),
				.paused = inactive || On(PowerSaving::kEmojiChat),
			});
		} else {
			if (state->image.isNull()) {
				state->image = owner->reactions().resolveReactionImageFor(
					id);
			}
			if (!state->image.isNull()) {
				const auto size = st::reactionInlineSize;
				const auto skip = (size - st::reactionInlineImage) / 2;
				p.drawImage(skip, top + skip, state->image);
			}
		}
	}, field->lifetime());

	Ui::AddLengthLimitLabel(field, kTagNameLimit);

	const auto save = [=] {
		const auto text = field->getLastText();
		if (text.size() > kTagNameLimit) {
			field->showError();
			return;
		}
		const auto weak = base::make_weak(box);
		controller->session().data().reactions().renameTag(id, text);
		if (const auto strong = weak.get()) {
			strong->closeBox();
		}
	};

	field->submits(
	) | rpl::on_next(save, field->lifetime());

	box->addButton(tr::lng_settings_save(), save);
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

void ShowWhoReadInfo(
		not_null<Window::SessionController*> controller,
		FullMsgId itemId,
		Ui::WhoReadParticipant who) {
	const auto peer = controller->session().data().peer(itemId.peer);
	const auto participant = peer->owner().peer(PeerId(who.id));
	const auto migrated = participant->migrateFrom();
	const auto origin = who.dateReacted
		? Info::Profile::Origin{
			Info::Profile::GroupReactionOrigin{ peer, itemId.msg },
		}
		: Info::Profile::Origin();
	auto memento = std::make_shared<Info::Memento>(
		std::vector<std::shared_ptr<Info::ContentMemento>>{
		std::make_shared<Info::Profile::Memento>(
			participant,
			migrated ? migrated->id : PeerId(),
			origin),
	});
	controller->showSection(std::move(memento));
}

[[nodiscard]] rpl::producer<not_null<UserData*>> LookupMessageAuthor(
		not_null<HistoryItem*> item) {
	struct Author {
		UserData *user = nullptr;
		std::vector<Fn<void(UserData*)>> callbacks;
	};
	struct Authors {
		base::flat_map<FullMsgId, Author> map;
	};
	static auto Cache = base::flat_map<not_null<Main::Session*>, Authors>();

	const auto channel = item->history()->peer->asChannel();
	const auto session = &channel->session();
	const auto id = item->fullId();
	if (!Cache.contains(session)) {
		Cache.emplace(session);
		session->lifetime().add([session] {
			Cache.remove(session);
		});
	}

	return [channel, id](auto consumer) {
		const auto session = &channel->session();
		auto &map = Cache[session].map;
		auto i = map.find(id);
		if (i == end(map)) {
			i = map.emplace(id).first;
			const auto finishWith = [=](UserData *user) {
				auto &entry = Cache[session].map[id];
				entry.user = user;
				for (const auto &callback : base::take(entry.callbacks)) {
					callback(user);
				}
			};
			session->api().request(MTPchannels_GetMessageAuthor(
				channel->inputChannel(),
				MTP_int(id.msg.bare)
			)).done([=](const MTPUser &result) {
				finishWith(session->data().processUser(result));
			}).fail([=] {
				finishWith(nullptr);
			}).send();
		} else if (const auto user = i->second.user
			; user || i->second.callbacks.empty()) {
			if (user) {
				consumer.put_next(not_null(user));
			}
			return rpl::lifetime();
		}

		auto lifetime = rpl::lifetime();
		const auto done = [=](UserData *result) {
			if (result) {
				consumer.put_next(not_null(result));
			}
		};
		const auto guard = lifetime.make_state<base::has_weak_ptr>();
		i->second.callbacks.push_back(crl::guard(guard, done));
		return lifetime;
	};
}

[[nodiscard]] base::unique_qptr<Ui::Menu::ItemBase> MakeMessageAuthorAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item,
		not_null<Window::SessionController*> controller) {
	const auto parent = menu->menu();
	const auto user = std::make_shared<UserData*>(nullptr);
	const auto action = Ui::Menu::CreateAction(
		parent,
		tr::lng_contacts_loading(tr::now),
		[=] { if (*user) { controller->showPeerInfo(*user); } });
	action->setDisabled(true);
	auto lifetime = LookupMessageAuthor(
		item
	) | rpl::on_next([=](not_null<UserData*> author) {
		action->setText(
			tr::lng_context_sent_by(tr::now, lt_user, author->name()));
		action->setDisabled(false);
		*user = author;
	});
	auto result = base::make_unique_q<Ui::Menu::Action>(
		menu->menu(),
		st::whoSentItem,
		action,
		nullptr,
		nullptr);
	result->lifetime().add(std::move(lifetime));
	return result;
}

} // namespace

ContextMenuRequest::ContextMenuRequest(
	not_null<Window::SessionNavigation*> navigation)
: navigation(navigation) {
}

base::unique_qptr<Ui::PopupMenu> FillContextMenu(
		not_null<ListWidget*> list,
		const ContextMenuRequest &request) {
	const auto link = request.link;
	const auto view = request.view;
	const auto item = request.item;
	const auto itemId = item ? item->fullId() : FullMsgId();
	const auto lnkPhoto = link
		? reinterpret_cast<PhotoData*>(
			link->property(kPhotoLinkMediaProperty).toULongLong())
		: nullptr;
	const auto lnkDocument = link
		? reinterpret_cast<DocumentData*>(
			link->property(kDocumentLinkMediaProperty).toULongLong())
		: nullptr;
	const auto poll = item
		? (item->media() ? item->media()->poll() : nullptr)
		: nullptr;
	const auto hasSelection = !request.selectedItems.empty()
		|| !request.selectedText.empty();
	const auto hasWhoReactedItem = item
		&& Api::WhoReactedExists(item, Api::WhoReactedList::All);

	auto result = base::make_unique_q<Ui::PopupMenu>(
		list,
		st::popupMenuWithIcons);

	AddMessageDetailsAction(result, item, view, list->controller());

	if (hasWhoReactedItem) {
		AddWhoReactedAction(result, list, item, list->controller());
	}

	AddReplyToMessageAction(result, request, list);
	AddTodoListAction(result, request, list);

	if (request.overSelection
		&& !list->hasCopyRestrictionForSelected()
		&& !list->getSelectedText().empty()) {
		const auto text = request.selectedItems.empty()
			? tr::lng_context_copy_selected(tr::now)
			: tr::lng_context_copy_selected_items(tr::now);
		result->addAction(text, [=] {
			if (!list->showCopyRestrictionForSelected()) {
				TextUtilities::SetClipboardText(list->getSelectedText());
			}
		}, &st::menuIconCopy);
	}
	if (request.overSelection
		&& !Ui::SkipTranslate(list->getSelectedText().rich)) {
		const auto owner = &view->history()->owner();
		result->addAction(tr::lng_context_translate_selected(tr::now), [=] {
			if (const auto item = owner->message(itemId)) {
				list->controller()->show(Box(
					Ui::TranslateBox,
					item->history()->peer,
					MsgId(),
					list->getSelectedText().rich,
					list->hasCopyRestrictionForSelected()));
			}
		}, &st::menuIconTranslate);
	}

	AddTopMessageActions(result, request, list);
	if (lnkPhoto && request.selectedItems.empty()) {
		AddPhotoActions(result, lnkPhoto, item, list);
	} else if (lnkDocument) {
		AddDocumentActions(result, lnkDocument, item, list);
	} else if (poll) {
		const auto context = list->elementContext();
		AddPollActions(result, poll, item, context, list->controller());
	} else if (!request.overSelection && view && !hasSelection) {
		const auto owner = &view->history()->owner();
		const auto media = view->media();
		const auto mediaHasTextForCopy = media && media->hasTextForCopy();
		if (const auto document = media ? media->getDocument() : nullptr) {
			AddDocumentActions(result, document, view->data(), list);
		}
		if (!link && (view->hasVisibleText() || mediaHasTextForCopy)) {
			if (!list->hasCopyRestriction(view->data())) {
				const auto asGroup = (request.pointState != PointState::GroupPart);
				result->addAction(tr::lng_context_copy_text(tr::now), [=] {
					if (const auto item = owner->message(itemId)) {
						if (!list->showCopyRestriction(item)) {
							if (asGroup) {
								if (const auto group = owner->groups().find(item)) {
									TextUtilities::SetClipboardText(HistoryGroupText(group));
									return;
								}
							}
							TextUtilities::SetClipboardText(HistoryItemText(item));
						}
					}
				}, &st::menuIconCopy);
			}

			const auto translate = mediaHasTextForCopy
				? (HistoryView::TransribedText(item)
					.append('\n')
					.append(item->originalText()))
				: item->originalText();
			if ((!item->translation() || !item->history()->translatedTo())
				&& !translate.text.isEmpty()
				&& !Ui::SkipTranslate(translate)) {
				result->addAction(tr::lng_context_translate(tr::now), [=] {
					if (const auto item = owner->message(itemId)) {
						list->controller()->show(Box(
							Ui::TranslateBox,
							item->history()->peer,
							mediaHasTextForCopy
								? MsgId()
								: item->fullId().msg,
							translate,
							list->hasCopyRestriction(view->data())));
					}
				}, &st::menuIconTranslate);
			}
		}
	}

	AddCopyLinkAction(result, link);
	AddMessageActions(result, request, list);
	AddRateTranscribeAction(result, item);

	const auto wasAmount = result->actions().size();
	if (const auto textItem = view ? view->textItem() : item) {
		AddEmojiPacksAction(
			result,
			textItem,
			HistoryView::EmojiPacksSource::Message,
			list->controller());
	}
	if (item) {
		const auto added = (result->actions().size() > wasAmount);
		AddSelectRestrictionAction(result, item, !added);
	}
	//if (lnkDocument){
	//	AddStickerSetOwnerActions(result, lnkDocument, item);
	//}

	return result;
}

void AddMessageDetailsAction(
			not_null<Ui::PopupMenu*> menu,
			HistoryItem *item,
			Element *view,
			not_null<Window::SessionController*> controller) {
		if (!GetEnhancedBool("show_message_context_details")) {
			return;
		}
		if (!item) {
			return;
		}
	Ui::Menu::CreateAddActionCallback(menu)(Window::PeerMenuCallback::Args{
		.text = tr::lng_context_details(tr::now),
		.handler = nullptr,
		.icon = &st::menuIconInfo,
		.fillSubmenu = [=](not_null<Ui::PopupMenu*> submenu) {
			FillDetailsSubmenu(submenu, item, view, controller);
		},
	});
}

void CopyPostLink(
		not_null<Window::SessionController*> controller,
		FullMsgId itemId,
		Context context,
		std::optional<TimeId> videoTimestamp) {
	CopyPostLink(controller->uiShow(), itemId, context, videoTimestamp);
}

void CopyPostLink(
		std::shared_ptr<Main::SessionShow> show,
		FullMsgId itemId,
		Context context,
		std::optional<TimeId> videoTimestamp) {
	const auto item = show->session().data().message(itemId);
	if (!item || !item->hasDirectLink()) {
		return;
	}
	const auto inRepliesContext = (context == Context::Replies);
	const auto forceNonPublicLink = !videoTimestamp && base::IsCtrlPressed();
	QGuiApplication::clipboard()->setText(
		item->history()->session().api().exportDirectMessageLink(
			item,
			inRepliesContext,
			forceNonPublicLink,
			videoTimestamp));

	const auto isPublicLink = [&] {
		if (forceNonPublicLink) {
			return false;
		}
		const auto channel = item->history()->peer->asChannel();
		Assert(channel != nullptr);
		if (const auto rootId = item->replyToTop()) {
			const auto root = item->history()->owner().message(
				channel->id,
				rootId);
			const auto sender = root
				? root->discussionPostOriginalSender()
				: nullptr;
			if (sender && sender->hasUsername()) {
				return true;
			}
		}
		return channel->hasUsername();
	}();
	if (isPublicLink && !videoTimestamp) {
		show->showToast({
			.text = tr::lng_channel_public_link_copied(
				tr::now, tr::bold
			).append('\n').append(Platform::IsMac()
				? tr::lng_public_post_private_hint_cmd(tr::now)
				: tr::lng_public_post_private_hint_ctrl(tr::now)),
			.duration = kPublicPostLinkToastDuration,
		});
	} else {
		show->showToast(isPublicLink
			? tr::lng_channel_public_link_copied(tr::now)
			: tr::lng_context_about_private_link(tr::now));
	}
}

void ViewAsJSON(
	not_null<Window::SessionController*> controller,
	FullMsgId itemId) {
	ViewAsJSON(controller->uiShow(), itemId);
}

void ViewAsJSON(
	std::shared_ptr<Main::SessionShow> show,
	FullMsgId itemId) {
	const auto item = show->session().data().message(itemId);
	if (!item) {
		return;
	}
	item->history()->session().api().exportMessageAsBase64(item,
		crl::guard(show, [=](const QString& base64) {
			Core::App().iv().showTLViewer(MTP::details::kCurrentLayer, base64);
		}),
		crl::guard(show, [=] {
			show->showToast(u"error"_q);
		}));
}

void CopyStoryLink(
		std::shared_ptr<Main::SessionShow> show,
		FullStoryId storyId) {
	const auto session = &show->session();
	const auto maybeStory = session->data().stories().lookup(storyId);
	if (!maybeStory) {
		return;
	}
	const auto story = *maybeStory;
	QGuiApplication::clipboard()->setText(
		session->api().exportDirectStoryLink(story));
	show->showToast(tr::lng_channel_public_link_copied(tr::now));
}

void AddPollActions(
		not_null<Ui::PopupMenu*> menu,
		not_null<PollData*> poll,
		not_null<HistoryItem*> item,
		Context context,
		not_null<Window::SessionController*> controller) {
	{
		constexpr auto kRadio = "\xf0\x9f\x94\x98";
		const auto radio = QString::fromUtf8(kRadio);
		auto text = poll->question;
		for (const auto &answer : poll->answers) {
			text.append('\n').append(radio).append(answer.text);
		}
		if (!Ui::SkipTranslate(text)) {
			menu->addAction(tr::lng_context_translate(tr::now), [=] {
				controller->show(Box(
					Ui::TranslateBox,
					item->history()->peer,
					MsgId(),
					std::move(text),
					item->forbidsForward()));
			}, &st::menuIconTranslate);
		}
	}
	if ((context != Context::History)
		&& (context != Context::Replies)
		&& (context != Context::Pinned)
		&& (context != Context::ScheduledTopic)) {
		return;
	}
	if (poll->closed()) {
		return;
	}
	const auto itemId = item->fullId();
	if (poll->voted() && !poll->quiz()) {
		menu->addAction(tr::lng_polls_retract(tr::now), [=] {
			poll->session().api().polls().sendVotes(itemId, {});
		}, &st::menuIconRetractVote);
	}
	if (item->canStopPoll()) {
		menu->addAction(tr::lng_polls_stop(tr::now), [=] {
			controller->show(Ui::MakeConfirmBox({
				.text = tr::lng_polls_stop_warning(),
				.confirmed = [=](Fn<void()> &&close) {
					close();
					if (const auto item = poll->owner().message(itemId)) {
						controller->session().api().polls().close(item);
					}
				},
				.confirmText = tr::lng_polls_stop_sure(),
				.cancelText = tr::lng_cancel(),
			}));
		}, &st::menuIconRemove);
	}
}

void AddSaveSoundForNotifications(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item,
		not_null<DocumentData*> document,
		not_null<Window::SessionController*> controller) {
	if (ItemHasTtl(item)) {
		return;
	}
	const auto &ringtones = document->session().api().ringtones();
	if (document->size > ringtones.maxSize()) {
		return;
	} else if (ranges::contains(ringtones.list(), document->id)) {
		return;
	} else if (int(ringtones.list().size()) >= ringtones.maxSavedCount()) {
		return;
	} else if (document->song()) {
		if (document->duration() > ringtones.maxDuration()) {
			return;
		}
	} else if (document->voice()) {
		if (document->duration() > ringtones.maxDuration()) {
			return;
		}
	} else {
		return;
	}
	const auto show = controller->uiShow();
	menu->addAction(tr::lng_context_save_custom_sound(tr::now), [=] {
		Api::ToggleSavedRingtone(
			document,
			item->fullId(),
			[=] { show->showToast(
				tr::lng_ringtones_toast_added(tr::now)); },
			true);
	}, &st::menuIconSoundAdd);
}

void AddWhenEditedForwardedAuthorActionHelper(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item,
		not_null<Window::SessionController*> controller,
		bool insertSeparator) {
	if (const auto forwarded = item->Get<HistoryMessageForwarded>()) {
		if (!forwarded->story && forwarded->psaType.isEmpty()) {
			if (insertSeparator && !menu->empty()) {
				menu->addSeparator(&st::expandedMenuSeparator);
			}
			menu->addAction(Ui::WhenReadContextAction(
				menu.get(),
				Api::WhenOriginal(item->from(), forwarded->originalDate)));
		}
	} else if (const auto edited = item->Get<HistoryMessageEdited>()) {
		if (!item->hideEditedBadge()) {
			if (insertSeparator && !menu->empty()) {
				menu->addSeparator(&st::expandedMenuSeparator);
			}
			menu->addAction(Ui::WhenReadContextAction(
				menu.get(),
				Api::WhenEdited(item->from(), edited->date)));
		}
	}
	if (item->canLookupMessageAuthor()) {
		if (insertSeparator && !menu->empty()) {
			menu->addSeparator(&st::expandedMenuSeparator);
		}
		menu->addAction(MakeMessageAuthorAction(menu, item, controller));
	}
}

void AddWhoReactedAction(
			not_null<Ui::PopupMenu*> menu,
			not_null<QWidget*> context,
			not_null<HistoryItem*> item,
			not_null<Window::SessionController*> controller) {
		if (!GetEnhancedBool("show_message_context_read_info")) {
			return;
		}
		const auto whoReadIds = std::make_shared<Api::WhoReadList>();
	const auto weak = base::make_weak(menu.get());
	const auto user = item->history()->peer;
	const auto showOrPremium = [=] {
		if (const auto strong = weak.get()) {
			strong->hideMenu();
		}
		const auto type = Ui::ShowOrPremium::ReadTime;
		const auto name = user->shortName();
		auto box = Box(Ui::ShowOrPremiumBox, type, name, [=] {
			const auto api = &controller->session().api();
			api->globalPrivacy().updateHideReadTime({});
		}, [=] {
			Settings::ShowPremium(controller, u"revtime_hidden"_q);
		});
		controller->show(std::move(box));
	};
	const auto itemId = item->fullId();
	const auto participantChosen = [=](Ui::WhoReadParticipant who) {
		if (const auto strong = weak.get()) {
			strong->hideMenu();
		}
		ShowWhoReadInfo(controller, itemId, who);
	};
	const auto showAllChosen = [=, itemId = item->fullId()]{
		// Pressing on an item that has a submenu doesn't hide it :(
		if (const auto strong = weak.get()) {
			strong->hideMenu();
		}
		if (const auto item = controller->session().data().message(itemId)) {
			controller->showSection(
				std::make_shared<Info::Memento>(
					whoReadIds,
					itemId,
					HistoryView::Reactions::DefaultSelectedTab(
						item,
						whoReadIds)));
		}
	};
	if (item->history()->peer->isUser()) {
		AddWhenEditedForwardedAuthorActionHelper(
			menu,
			item,
			controller,
			false);
		menu->addAction(Ui::WhenReadContextAction(
			menu.get(),
			Api::WhoReacted(item, context, st::defaultWhoRead, whoReadIds),
			showOrPremium));
	} else {
		menu->addAction(Ui::WhoReactedContextAction(
			menu.get(),
			Api::WhoReacted(item, context, st::defaultWhoRead, whoReadIds),
			Data::ReactedMenuFactory(&controller->session()),
			participantChosen,
			showAllChosen));
		AddWhenEditedForwardedAuthorActionHelper(
			menu,
			item,
			controller,
			true);
	}
}

void MaybeAddWhenEditedForwardedAction(
			not_null<Ui::PopupMenu*> menu,
			not_null<HistoryItem*> item,
			not_null<Window::SessionController*> controller) {
		if (!GetEnhancedBool("show_message_context_read_info")) {
			return;
		}
		AddWhenEditedForwardedAuthorActionHelper(menu, item, controller, true);
	}

void AddEditTagAction(
		not_null<Ui::PopupMenu*> menu,
		const Data::ReactionId &id,
		not_null<Window::SessionController*> controller) {
	const auto owner = &controller->session().data();
	const auto editLabel = owner->reactions().myTagTitle(id).isEmpty()
		? tr::lng_context_tag_add_name(tr::now)
		: tr::lng_context_tag_edit_name(tr::now);
	menu->addAction(editLabel, [=] {
		controller->show(Box(EditTagBox, controller, id));
	}, &st::menuIconTagRename);
}

void AddTagPackAction(
		not_null<Ui::PopupMenu*> menu,
		const Data::ReactionId &id,
		not_null<Window::SessionController*> controller) {
	if (const auto custom = id.custom()) {
		const auto owner = &controller->session().data();
		if (const auto set = owner->document(custom)->sticker()) {
			if (set->set.id) {
				AddEmojiPacksAction(
					menu,
					{ set->set },
					EmojiPacksSource::Tag,
					controller);
			}
		}
	}
}

void ShowTagMenu(
		not_null<base::unique_qptr<Ui::PopupMenu>*> menu,
		QPoint position,
		not_null<QWidget*> context,
		not_null<HistoryItem*> item,
		const Data::ReactionId &id,
		not_null<Window::SessionController*> controller) {
	using namespace Data;
	const auto itemId = item->fullId();
	const auto owner = &controller->session().data();
	*menu = base::make_unique_q<Ui::PopupMenu>(
		context,
		st::popupMenuExpandedSeparator);
	(*menu)->addAction(tr::lng_context_filter_by_tag(tr::now), [=] {
		HashtagClickHandler(SearchTagToQuery(id)).onClick({
			.button = Qt::LeftButton,
			.other = QVariant::fromValue(ClickHandlerContext{
				.sessionWindow = controller,
			}),
		});
	}, &st::menuIconTagFilter);

	AddEditTagAction(menu->get(), id, controller);

	const auto removeTag = [=] {
		if (const auto item = owner->message(itemId)) {
			const auto &list = item->reactions();
			if (ranges::contains(list, id, &MessageReaction::id)) {
				item->toggleReaction(id, HistoryReactionSource::Quick);
			}
		}
	};
	(*menu)->addAction(base::make_unique_q<Ui::Menu::Action>(
		(*menu)->menu(),
		st::menuWithIconsAttention,
		Ui::Menu::CreateAction(
			(*menu)->menu(),
			tr::lng_context_remove_tag(tr::now),
			removeTag),
		&st::menuIconTagRemoveAttention,
		&st::menuIconTagRemoveAttention));

	AddTagPackAction(menu->get(), id, controller);

	(*menu)->popup(position);
}

void ShowTagInListMenu(
		not_null<base::unique_qptr<Ui::PopupMenu>*> menu,
		QPoint position,
		not_null<QWidget*> context,
		const Data::ReactionId &id,
		not_null<Window::SessionController*> controller) {
	*menu = base::make_unique_q<Ui::PopupMenu>(
		context,
		st::popupMenuExpandedSeparator);

	AddEditTagAction(menu->get(), id, controller);
	AddTagPackAction(menu->get(), id, controller);

	(*menu)->popup(position);
}

void AddCopyFilename(
		not_null<Ui::PopupMenu*> menu,
		not_null<DocumentData*> document,
		Fn<bool()> showCopyRestrictionForSelected) {
	const auto filenameToCopy = [&] {
		if (document->isAudioFile()) {
			return TextForMimeData().append(
				Ui::Text::FormatSongNameFor(document).string());
		} else if (document->sticker()
			|| document->isAnimation()
			|| document->isVideoMessage()
			|| document->isVideoFile()
			|| document->isVoiceMessage()) {
			return TextForMimeData();
		} else {
			return TextForMimeData().append(document->filename());
		}
	}();
	if (!filenameToCopy.empty()) {
		menu->addAction(tr::lng_context_copy_filename(tr::now), [=] {
			if (!showCopyRestrictionForSelected()) {
				TextUtilities::SetClipboardText(filenameToCopy);
			}
		}, &st::menuIconCopy);
	}
}

void ShowWhoReactedMenu(
		not_null<base::unique_qptr<Ui::PopupMenu>*> menu,
		QPoint position,
		not_null<QWidget*> context,
		not_null<HistoryItem*> item,
		const Data::ReactionId &id,
		not_null<Window::SessionController*> controller,
		rpl::lifetime &lifetime) {
	if (item->reactionsAreTags()) {
		ShowTagMenu(menu, position, context, item, id, controller);
		return;
	}

	struct State {
		int addedToBottom = 0;
	};
	const auto itemId = item->fullId();
	const auto participantChosen = [=](Ui::WhoReadParticipant who) {
		ShowWhoReadInfo(controller, itemId, who);
	};
	const auto showAllChosen = [=, itemId = item->fullId()]{
		if (const auto item = controller->session().data().message(itemId)) {
			controller->showSection(std::make_shared<Info::Memento>(
				nullptr,
				itemId,
				HistoryView::Reactions::DefaultSelectedTab(item, id)));
		}
	};
	const auto owner = &controller->session().data();
	const auto reactions = &owner->reactions();
	const auto &list = reactions->list(
		Data::Reactions::Type::Active);
	const auto activeNonQuick = !id.paid()
		&& (id != reactions->favoriteId())
		&& (ranges::contains(list, id, &Data::Reaction::id)
			|| (controller->session().premium() && id.custom()));
	const auto filler = lifetime.make_state<Ui::WhoReactedListMenu>(
		Data::ReactedMenuFactory(&controller->session()),
		participantChosen,
		showAllChosen);
	const auto state = lifetime.make_state<State>();
	Api::WhoReacted(
		item,
		id,
		context,
		st::defaultWhoRead
	) | rpl::filter([=](const Ui::WhoReadContent &content) {
		return content.state != Ui::WhoReadState::Unknown;
	}) | rpl::on_next([=, &lifetime](Ui::WhoReadContent &&content) {
		const auto creating = !*menu;
		const auto refillTop = [=] {
			if (activeNonQuick) {
				(*menu)->addAction(tr::lng_context_set_as_quick(tr::now), [=] {
					reactions->setFavorite(id);
				}, &st::menuIconFave);
				(*menu)->addSeparator();
			}
		};
		const auto appendBottom = [=] {
			state->addedToBottom = 0;
			if (const auto custom = id.custom()) {
				if (const auto set = owner->document(custom)->sticker()) {
					if (set->set.id) {
						state->addedToBottom = 2;
						AddEmojiPacksAction(
							menu->get(),
							{ set->set },
							EmojiPacksSource::Reaction,
							controller);
					}
				}
			}
		};
		if (creating) {
			*menu = base::make_unique_q<Ui::PopupMenu>(
				context,
				st::whoReadMenu);
			(*menu)->lifetime().add(base::take(lifetime));
			refillTop();
		}
		filler->populate(
			menu->get(),
			content,
			refillTop,
			state->addedToBottom,
			appendBottom);
		if (creating) {
			(*menu)->popup(position);
		}
	}, lifetime);
}

std::vector<StickerSetIdentifier> CollectEmojiPacks(
		not_null<HistoryItem*> item,
		EmojiPacksSource source) {
	auto result = std::vector<StickerSetIdentifier>();
	const auto owner = &item->history()->owner();
	const auto push = [&](DocumentId id) {
		if (const auto set = owner->document(id)->sticker()) {
			if (set->set.id
				&& !ranges::contains(
					result,
					set->set.id,
					&StickerSetIdentifier::id)) {
				result.push_back(set->set);
			}
		}
	};
	switch (source) {
	case EmojiPacksSource::Message:
		for (const auto &entity : item->originalText().entities) {
			if (entity.type() == EntityType::CustomEmoji) {
				const auto data = Data::ParseCustomEmojiData(entity.data());
				push(data);
			}
		}
		break;
	case EmojiPacksSource::Reactions:
		for (const auto &reaction : item->reactions()) {
			if (const auto customId = reaction.id.custom()) {
				push(customId);
			}
		}
		break;
	default: Unexpected("Source in CollectEmojiPacks.");
	}
	return result;
}

void AddEmojiPacksAction(
		not_null<Ui::PopupMenu*> menu,
		std::vector<StickerSetIdentifier> packIds,
		EmojiPacksSource source,
		not_null<Window::SessionController*> controller) {
	if (packIds.empty()) {
		return;
	}

	const auto count = int(packIds.size());
	const auto manager = &controller->session().data().customEmojiManager();
	const auto name = (count == 1)
		? TextWithEntities{ manager->lookupSetName(packIds[0].id) }
		: TextWithEntities();
	if (!menu->empty()) {
		menu->addSeparator();
	}
	auto text = [&] {
		switch (source) {
		case EmojiPacksSource::Message:
			return name.text.isEmpty()
				? tr::lng_context_animated_emoji_many(
					tr::now,
					lt_count,
					count,
					tr::rich)
				: tr::lng_context_animated_emoji(
					tr::now,
					lt_name,
					TextWithEntities{ name },
					tr::rich);
		case EmojiPacksSource::Tag:
			return tr::lng_context_animated_tag(
				tr::now,
				lt_name,
				TextWithEntities{ name },
				tr::rich);
		case EmojiPacksSource::Reaction:
			if (!name.text.isEmpty()) {
				return tr::lng_context_animated_reaction(
					tr::now,
					lt_name,
					TextWithEntities{ name },
					tr::rich);
			}
			[[fallthrough]];
		case EmojiPacksSource::Reactions:
			return name.text.isEmpty()
				? tr::lng_context_animated_reactions_many(
					tr::now,
					lt_count,
					count,
					tr::rich)
				: tr::lng_context_animated_reactions(
					tr::now,
					lt_name,
					TextWithEntities{ name },
					tr::rich);
		}
		Unexpected("Source in AddEmojiPacksAction.");
	}();
	auto button = base::make_unique_q<Ui::Menu::MultilineAction>(
		menu->menu(),
		menu->st().menu,
		st::historyHasCustomEmoji,
		st::historyHasCustomEmojiPosition,
		std::move(text));
	const auto weak = base::make_weak(controller);
	button->setActionTriggered([=] {
		const auto strong = weak.get();
		if (!strong) {
			return;
		} else if (packIds.size() > 1) {
			strong->show(Box<StickersBox>(strong->uiShow(), packIds));
			return;
		}
		// Single used emoji pack.
		strong->show(Box<StickerSetBox>(
			strong->uiShow(),
			packIds.front(),
			Data::StickersType::Emoji));
	});
	menu->addAction(std::move(button));
}

void AddEmojiPacksAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item,
		EmojiPacksSource source,
		not_null<Window::SessionController*> controller) {
	AddEmojiPacksAction(
		menu,
		CollectEmojiPacks(item, source),
		source,
		controller);
}

void AddSelectRestrictionAction(
		not_null<Ui::PopupMenu*> menu,
		not_null<HistoryItem*> item,
		bool addIcon) {
	const auto peer = item->history()->peer;
	if ((peer->allowsForwarding() && !item->forbidsForward())
		|| item->isSponsored()) {
		return;
	}
	if (addIcon && !menu->empty()) {
		menu->addSeparator();
	}
	const auto user = peer->asUser();
	auto button = base::make_unique_q<Ui::Menu::MultilineAction>(
		menu->menu(),
		menu->st().menu,
		st::historyHasCustomEmoji,
		((addIcon && !user)
			? st::historySponsoredAboutMenuLabelPosition
			: st::historyHasCustomEmojiPosition),
		(peer->isMegagroup()
			? tr::lng_context_noforwards_info_group(tr::now, tr::rich)
			: (peer->isChannel())
			? tr::lng_context_noforwards_info_channel(tr::now, tr::rich)
			: (user && user->isBot())
			? tr::lng_context_noforwards_info_bot(tr::now, tr::rich)
			: user
			? ((user->flags() & UserDataFlag::NoForwardsMyEnabled)
				? tr::lng_context_noforwards_info_mine(tr::now, tr::rich)
				: tr::lng_context_noforwards_info_his(
					tr::now,
					lt_user,
					tr::bold(user->shortName()),
					tr::rich))
			: tr::lng_context_noforwards_info_channel(tr::now, tr::rich)),
		(addIcon && !user) ? &st::menuIconCopyright : nullptr);
	button->setAttribute(Qt::WA_TransparentForMouseEvents);
	menu->addAction(std::move(button));
}

void AddStickerSetOwnerActions(
	not_null<Ui::PopupMenu*> menu,
	not_null<DocumentData*> document,
	HistoryItem* item) {
	if (document->sticker() && document->sticker()->set) {
		if (!menu->empty()) {
			menu->addSeparator();
		}

		const auto author = [=] {
			auto ownerId = document->sticker()->set.id >> 32;
			if ((document->sticker()->set.id >> 16 & 0xff) == 0x3f) {
				ownerId |= 0x80000000;
			}
			if (document->sticker()->set.id >> 24 & 0xff) {
				ownerId += 0x100000000;
			}
			const auto peer = document->session().data().peerLoaded(static_cast<PeerId>(ownerId));
			if (peer != nullptr) {
				if (const auto window = document->session().tryResolveWindow()) {
					window->showPeerInfo(peer);
				}
			} else {
				QGuiApplication::clipboard()->setText(QString::number(ownerId));
				Ui::Toast::Show(tr::lng_code_copied(tr::now));
			}
		};

		menu->addAction(
			tr::lng_channel_admin_status_creator(tr::now),
			[=] { author(); },
			&st::menuIconProfile);
	}
}

TextWithEntities TransribedText(not_null<HistoryItem*> item) {
	const auto media = item->media();
	const auto document = media ? media->document() : nullptr;
	if (!document || !document->isVoiceMessage()) {
		return {};
	}
	const auto &entry = document->session().api().transcribes().entry(item);
	if (!entry.requestId
		&& entry.shown
		&& !entry.toolong
		&& !entry.failed
		&& !entry.pending
		&& !entry.result.isEmpty()) {
		return { entry.result };
	}
	return {};
}

bool ItemHasTtl(HistoryItem *item) {
	return (item && item->media())
		? (item->media()->ttlSeconds() > 0)
		: false;
}

} // namespace HistoryView
