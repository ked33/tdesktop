/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/filters/edit_filter_chats_preview.h"

#include "boxes/filters/edit_filter_chats_list.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_user.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "ui/text/text_options.h"
#include "ui/widgets/buttons.h"
#include "ui/painter.h"
#include "styles/style_chat.h"
#include "styles/style_window.h"

namespace {

using Flag = Data::ChatFilter::Flag;

constexpr auto kAllTypes = {
	Flag::NewChats,
	Flag::ExistingChats,
	Flag::Contacts,
	Flag::NonContacts,
	Flag::Groups,
	Flag::Channels,
	Flag::Bots,
	Flag::NoMuted,
	Flag::NoRead,
	Flag::NoArchived,
};

} // namespace

FilterChatsPreview::FilterChatsPreview(
	not_null<QWidget*> parent,
	Flags flags,
	const base::flat_set<not_null<History*>> &peers)
: RpWidget(parent) {
	updateData(flags, peers);
}

void FilterChatsPreview::refresh() {
	resizeToWidth(width());
}

void FilterChatsPreview::updateData(
		Flags flags,
		const base::flat_set<not_null<History*>> &peers) {
	_removeFlag.clear();
	_removePeer.clear();
	const auto makeButton = [&](Fn<void()> handler) {
		auto result = base::make_unique_q<Ui::IconButton>(
			this,
			st::windowFilterSmallRemove);
		result->setClickedCallback(std::move(handler));
		result->show();
		return result;
	};
	for (const auto flag : kAllTypes) {
		if (flags & flag) {
			_removeFlag.push_back({
				flag,
				makeButton([=] { removeFlag(flag); }) });
		}
	}
	for (const auto &history : peers) {
		_removePeer.push_back(PeerButton{
			.history = history,
			.button = makeButton([=] { removePeer(history); })
		});
	}
	refresh();
}

int FilterChatsPreview::resizeGetHeight(int newWidth) {
	const auto right = st::windowFilterSmallRemoveRight;
	const auto add = (st::windowFilterSmallItem.height
		- st::windowFilterSmallRemove.height) / 2;
	auto top = 0;
	const auto moveNextButton = [&](not_null<Ui::IconButton*> button) {
		button->moveToRight(right, top + add, newWidth);
		top += st::windowFilterSmallItem.height;
	};
	for (const auto &[flag, button] : _removeFlag) {
		moveNextButton(button.get());
	}
	for (const auto &[history, userpic, name, button] : _removePeer) {
		moveNextButton(button.get());
	}
	return top;
}

void FilterChatsPreview::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	auto top = 0;
	const auto &st = st::windowFilterSmallItem;
	const auto iconLeft = st.photoPosition.x();
	const auto iconTop = st.photoPosition.y();
	const auto nameLeft = st.namePosition.x();
	p.setFont(st::windowFilterSmallItem.nameStyle.font);
	const auto nameTop = st.namePosition.y();
	const auto chatNameLeft = st::windowFilterChatNamePosition.x();
	const auto chatNameTop = st::windowFilterChatNamePosition.y();
	const auto chatDescLeft = st::windowFilterChatDescPosition.x();
	const auto chatDescTop = st::windowFilterChatDescPosition.y();
	for (const auto &[flag, button] : _removeFlag) {
		PaintFilterChatsTypeIcon(
			p,
			flag,
			iconLeft,
			top + iconTop,
			width(),
			st.photoSize);

		p.setPen(st::contactsNameFg);
		p.drawTextLeft(
			nameLeft,
			top + nameTop,
			width(),
			FilterChatsTypeName(flag));
		top += st.height;
	}
	auto statuses = QStringList();
	for (auto &[history, userpic, name, button] : _removePeer) {
		const auto peer = history->peer;
		const auto savedMessages = peer->isSelf();
		const auto repliesMessages = peer->isRepliesChat();
		const auto verifyCodes = peer->isVerifyCodes();
		if (savedMessages || repliesMessages || verifyCodes) {
			if (savedMessages) {
				Ui::EmptyUserpic::PaintSavedMessages(
					p,
					iconLeft,
					top + iconTop,
					width(),
					st.photoSize);
			} else if (repliesMessages) {
				Ui::EmptyUserpic::PaintRepliesMessages(
					p,
					iconLeft,
					top + iconTop,
					width(),
					st.photoSize);
			} else {
				history->peer->paintUserpicLeft(
					p,
					userpic,
					iconLeft,
					top + iconTop,
					width(),
					st.photoSize);
			}
			p.setPen(st::contactsNameFg);
			p.drawTextLeft(
				nameLeft,
				top + nameTop,
				width(),
				(savedMessages
					? tr::lng_saved_messages(tr::now)
					: repliesMessages
					? tr::lng_replies_messages(tr::now)
					: tr::lng_verification_codes(tr::now)));
		} else {
			history->peer->paintUserpicLeft(
				p,
				userpic,
				iconLeft,
				top + iconTop,
				width(),
				st.photoSize);
			if (const auto user = history->peer->asUser()) {
				if (user->isInaccessible()) {
					statuses << tr::lng_chat_status_unaccessible(tr::now);
				} else {
					if (user->isSupport()) {
						statuses << tr::lng_status_support(tr::now);
					}
					if (user->isBot()) {
						statuses << tr::lng_status_bot(tr::now);
					} else if (user->flags() & UserDataFlag::MutualContact) {
						statuses << tr::lng_filters_preview_mutual(tr::now);
					} else if (user->isContact()) {
						statuses << tr::lng_filters_preview_contact(tr::now);
					} else {
						statuses << tr::lng_filters_preview_non_contact(
							tr::now);
					}
				}
			} else if (const auto chat = history->peer->asChat()) {
				statuses << tr::lng_filters_preview_legacy_group(tr::now);
				if (!chat->amIn()) {
					statuses << tr::lng_filters_preview_not_member(tr::now);
				} else if (chat->amCreator()) {
					statuses << tr::lng_filters_preview_owner(tr::now);
				} else if (chat->hasAdminRights()) {
					statuses << tr::lng_admin_badge(tr::now);
				}
				if (chat->count > 0) {
					statuses << tr::lng_chat_status_members(
						tr::now,
						lt_count_decimal,
						chat->count);
				}
			} else if (const auto channel = history->peer->asChannel()) {
				statuses << (channel->isCommunity()
					? tr::lng_community_status(tr::now)
					: channel->isMonoforum()
					? tr::lng_filters_preview_monoforum(tr::now)
					: channel->isForum()
					? tr::lng_filters_preview_forum(tr::now)
					: channel->isMegagroup()
					? tr::lng_filters_preview_supergroup(tr::now)
					: tr::lng_channel_status(tr::now));
				if (!channel->amIn()) {
					statuses << (channel->isBroadcast()
						? tr::lng_filters_preview_not_subscribed(tr::now)
						: tr::lng_filters_preview_not_member(tr::now));
				} else if (channel->amCreator()) {
					statuses << tr::lng_filters_preview_owner(tr::now);
				} else if (channel->hasAdminRights()) {
					statuses << tr::lng_admin_badge(tr::now);
				}
				if (channel->membersCountKnown()) {
					statuses << (channel->isBroadcast()
						? tr::lng_chat_status_subscribers
						: tr::lng_chat_status_members)(
							tr::now,
							lt_count_decimal,
							channel->membersCount());
				}
			}
			p.setPen(st::contactsNameFg);
			if (name.isEmpty()) {
				name.setText(
					st::msgNameStyle,
					history->peer->name(),
					Ui::NameTextOptions());
			}
			if (statuses.isEmpty()) {
				name.drawLeftElided(
					p,
					nameLeft,
					top + nameTop,
					button->x() - nameLeft,
					width());
			} else {
				name.drawLeftElided(
					p,
					chatNameLeft,
					top + chatNameTop,
					button->x() - chatNameLeft,
					width());
				p.setPen(st::windowSubTextFg);
				p.setFont(st::windowFilterChatDescStyle.font);
				p.drawTextLeft(
					chatDescLeft,
					top + chatDescTop,
					width(),
					statuses.join(u", "_q));
				statuses.clear();
			}
		}
		top += st.height;
	}
}

void FilterChatsPreview::removeFlag(Flag flag) {
	const auto i = ranges::find(_removeFlag, flag, &FlagButton::flag);
	Assert(i != end(_removeFlag));
	_removeFlag.erase(i);
	refresh();
	_flagRemoved.fire_copy(flag);
}

void FilterChatsPreview::removePeer(not_null<History*> history) {
	const auto i = ranges::find(_removePeer, history, &PeerButton::history);
	Assert(i != end(_removePeer));
	_removePeer.erase(i);
	refresh();
	_peerRemoved.fire_copy(history);
}

rpl::producer<Flag> FilterChatsPreview::flagRemoved() const {
	return _flagRemoved.events();
}

rpl::producer<not_null<History*>> FilterChatsPreview::peerRemoved() const {
	return _peerRemoved.events();
}
