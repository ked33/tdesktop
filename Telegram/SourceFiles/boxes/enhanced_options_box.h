#pragma once
/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#pragma once

#include "boxes/abstract_box.h"
#include "base/unique_qptr.h"
#include "media/streaming/media_streaming_boost.h"

#include <array>

namespace Ui {
	class Checkbox;

	class RadiobuttonGroup;

	class Radiobutton;

	class FlatLabel;

	class InputField;
	class ScrollArea;
	class VerticalLayout;
} // namespace Ui

class NetBoostBox : public Ui::BoxContent {
public:
	NetBoostBox(QWidget *parent);

	static QString BoostLabel(int boost);

protected:
	void prepare() override;

private:
	void save();

	object_ptr<Ui::FlatLabel> _description = {nullptr};
	std::shared_ptr<Ui::RadiobuttonGroup> _boostGroup;

};

class DownloadBoostBox : public Ui::BoxContent {
public:
	DownloadBoostBox(QWidget *parent);

	static QString BoostLabel(int boost);

protected:
	void prepare() override;

private:
	void save();

	object_ptr<Ui::FlatLabel> _description = {nullptr};
	std::shared_ptr<Ui::RadiobuttonGroup> _boostGroup;

};

class DownloadBoostProfilesBox : public Ui::BoxContent {
public:
	DownloadBoostProfilesBox(QWidget *parent);

protected:
	void prepare() override;
	void resizeEvent(QResizeEvent *e) override;

private:
	static constexpr auto kNumericFieldCount = 22;

	void loadProfile(int profile);
	bool saveCurrentProfile();
	bool save();
	void reset();

	Media::Streaming::BoostProfiles _profiles;
	std::shared_ptr<Ui::RadiobuttonGroup> _profileGroup;
	base::unique_qptr<Ui::ScrollArea> _scroll;
	Ui::VerticalLayout *_content = nullptr;
	std::array<Ui::InputField*, kNumericFieldCount> _fields = {};
	Ui::Checkbox *_seekCancel = nullptr;
	Ui::Checkbox *_tailPrefetch = nullptr;
	int _radioHeight = 0;
	int _editingProfile = 0;
	bool _revertingProfile = false;

};

class AlwaysDeleteBox : public Ui::BoxContent {
public:
	AlwaysDeleteBox(QWidget *parent);

	static QString DeleteLabel(int option);

protected:
	void prepare() override;

private:
	void save();

	object_ptr<Ui::FlatLabel> _description = {nullptr};
	std::shared_ptr<Ui::RadiobuttonGroup> _optionGroup;

};

class MessageMediaSizeBox : public Ui::BoxContent {
public:
	MessageMediaSizeBox(QWidget *parent);

	[[nodiscard]] static QString SizeLabel();

protected:
	void prepare() override;
	void setInnerFocus() override;
	void resizeEvent(QResizeEvent *e) override;

private:
	void reset();
	void save();

	object_ptr<Ui::InputField> _emojiSize = { nullptr };
	object_ptr<Ui::InputField> _stickerSize = { nullptr };

};

class RadioController : public Ui::BoxContent {
public:
	RadioController(QWidget *parent);

protected:
	void prepare() override;

	void setInnerFocus() override;

	void resizeEvent(QResizeEvent *e) override;

private:
	void save();

	object_ptr<Ui::InputField> _url = {nullptr};

};

class MpvPathBox : public Ui::BoxContent {
public:
	MpvPathBox(QWidget *parent);

protected:
	void prepare() override;

	void setInnerFocus() override;

	void resizeEvent(QResizeEvent *e) override;

private:
	void save();

	object_ptr<Ui::InputField> _path = { nullptr };

};

class FloodPremiumWaitBox : public Ui::BoxContent {
public:
	FloodPremiumWaitBox(QWidget *parent);

	static QString DelayLabel(const QString &value);

protected:
	void prepare() override;

	void setInnerFocus() override;

	void resizeEvent(QResizeEvent *e) override;

private:
	void save();

	object_ptr<Ui::InputField> _delay = { nullptr };

};

class QuickCopyTargetsBox : public Ui::BoxContent {
public:
	QuickCopyTargetsBox(QWidget *parent);

	static QString TargetsLabel(const QString &value);

protected:
	void prepare() override;

	void setInnerFocus() override;

	void resizeEvent(QResizeEvent *e) override;

private:
	void save();

	object_ptr<Ui::InputField> _targets = { nullptr };

};

class CustomChatShortcutsBox : public Ui::BoxContent {
public:
	CustomChatShortcutsBox(QWidget *parent);

	static QString ShortcutsLabel(const QString &value);

protected:
	void prepare() override;

	void setInnerFocus() override;

	void resizeEvent(QResizeEvent *e) override;

private:
	void save();

	object_ptr<Ui::InputField> _shortcuts = { nullptr };

};

class NoForwardsBadgeColorBox : public Ui::BoxContent {
public:
	NoForwardsBadgeColorBox(QWidget *parent);

	static QString ColorLabel(const QString &value);

protected:
	void prepare() override;

	void setInnerFocus() override;

	void resizeEvent(QResizeEvent *e) override;

private:
	void save();

	object_ptr<Ui::InputField> _color = { nullptr };

};

class CodeBlockBgColorBox : public Ui::BoxContent {
public:
	CodeBlockBgColorBox(QWidget *parent);

	static QString ColorLabel(const QString &value);

protected:
	void prepare() override;

	void setInnerFocus() override;

	void resizeEvent(QResizeEvent *e) override;

private:
	void save();

	object_ptr<Ui::InputField> _color = { nullptr };

};

class SearchMessageHighlightBgColorBox : public Ui::BoxContent {
public:
	SearchMessageHighlightBgColorBox(QWidget *parent);

	static QString ColorLabel(const QString &value);

protected:
	void prepare() override;

	void setInnerFocus() override;

	void resizeEvent(QResizeEvent *e) override;

private:
	void save();

	object_ptr<Ui::InputField> _color = { nullptr };

};

class ChatSwitchShortcutBox : public Ui::BoxContent {
public:
	ChatSwitchShortcutBox(QWidget *parent);
	ChatSwitchShortcutBox(
		QWidget*,
		QString key,
		rpl::producer<QString> title,
		rpl::producer<QString> placeholder,
		Fn<QString()> invalidToast);

	static QString ShortcutLabel(const QString &value);

protected:
	void prepare() override;

	void setInnerFocus() override;

	void resizeEvent(QResizeEvent *e) override;

private:
	void save();

	object_ptr<Ui::InputField> _shortcut = { nullptr };
	QString _key;
	rpl::producer<QString> _title;
	Fn<QString()> _invalidToast;

};

class BitrateController : public Ui::BoxContent {
public:
	BitrateController(QWidget *parent);

	static QString BitrateLabel(int boost);

protected:
	void prepare() override;

private:
	void save();

	object_ptr<Ui::FlatLabel> _description = {nullptr};
	std::shared_ptr<Ui::RadiobuttonGroup> _bitrateGroup;

};

class PreviewBrightnessBox : public Ui::BoxContent {
public:
	PreviewBrightnessBox(QWidget *parent);

	static QString BrightnessLabel(int percent);

protected:
	void prepare() override;

private:
	void save();

	object_ptr<Ui::FlatLabel> _description = { nullptr };
	std::shared_ptr<Ui::RadiobuttonGroup> _brightnessGroup;

};
