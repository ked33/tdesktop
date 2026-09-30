/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#pragma once

#include "data/data_peer_id.h"
#include "rpl/producer.h"

#include <QtCore/QByteArray>
#include <QtCore/QTimer>

namespace EnhancedSettings {

	inline constexpr auto kMessageEmojiSizeMinimum = 50;
	inline constexpr auto kMessageEmojiSizeDefault = 112;
	inline constexpr auto kMessageEmojiSizeMaximum = 112;
	inline constexpr auto kMessageStickerSizeMinimum = 50;
	inline constexpr auto kMessageStickerSizeDefault = 256;
	inline constexpr auto kMessageStickerSizeMaximum = 256;
	inline constexpr auto kSearchPornConcurrencyMinimum = 1;
	inline constexpr auto kSearchPornConcurrencyDefault = 3;
	inline constexpr auto kSearchPornConcurrencyMaximum = 32;
	inline constexpr auto kSearchPornRequestIntervalMinimum = 0;
	inline constexpr auto kSearchPornRequestIntervalDefault = 500;
	inline constexpr auto kSearchPornRequestIntervalMaximum = 5000;
	inline constexpr auto kTrayIdleMemoryMinutesMinimum = 0;
	inline constexpr auto kTrayIdleMemoryMinutesDefault = 60;
	inline constexpr auto kTrayIdleMemoryMinutesMaximum = 1440;
	inline constexpr auto kHashtagAutocompleteLimitMinimum = 1;
	inline constexpr auto kHashtagAutocompleteLimitDefault = 1000;
	inline constexpr auto kHashtagAutocompleteLimitMaximum = 5000;

	enum class DownloadLimitToastMode {
		Fixed = 0,
		Countdown = 1,
	};

	[[nodiscard]] int MessageEmojiSize();
	[[nodiscard]] int MessageStickerSize();
	[[nodiscard]] DownloadLimitToastMode DownloadLimitToastsMode();
	[[nodiscard]] auto DownloadLimitToastModeChanges()
		-> rpl::producer<DownloadLimitToastMode>;
	void SetDownloadLimitToastsMode(DownloadLimitToastMode mode);
	[[nodiscard]] bool DownloadLimitToastsEnabled();
	[[nodiscard]] rpl::producer<bool> DownloadLimitToastsChanges();
	void SetDownloadLimitToastsEnabled(bool enabled);
	[[nodiscard]] bool SearchIncludePorn();
	[[nodiscard]] rpl::producer<bool> SearchIncludePornChanges();
	void SetSearchIncludePorn(bool enabled);
	[[nodiscard]] bool SearchDialogFilterEnabled();
	[[nodiscard]] QString SearchDialogFilterIds();
	[[nodiscard]] rpl::producer<> SearchDialogFilterChanges();
	[[nodiscard]] bool SearchDialogFilterContains(PeerId peer);
	void SetSearchDialogFilterEnabled(bool enabled);
	void SetSearchDialogFilterIds(const QString &value);
	[[nodiscard]] bool MultipleChatWindows();
	[[nodiscard]] int SearchPornConcurrency();
	[[nodiscard]] rpl::producer<int> SearchPornConcurrencyChanges();
	void SetSearchPornConcurrency(int value);
	[[nodiscard]] int SearchPornRequestInterval();
	[[nodiscard]] rpl::producer<int> SearchPornRequestIntervalChanges();
	void SetSearchPornRequestInterval(int value);
	[[nodiscard]] int TrayIdleMemoryMinutes();
	[[nodiscard]] rpl::producer<int> TrayIdleMemoryMinutesChanges();
	void SetTrayIdleMemoryMinutes(int value);
	[[nodiscard]] int HashtagAutocompleteLimit();
	[[nodiscard]] rpl::producer<int> HashtagAutocompleteLimitChanges();
	void SetHashtagAutocompleteLimit(int value);

	class Manager : public QObject {
	Q_OBJECT

	public:
		Manager();

		void fill();

		void write(bool force = false);

		[[nodiscard]] bool writeNow();

		void addIdToBlocklist(int64 userId);

		void removeIdFromBlocklist(int64 userId);

	public Q_SLOTS:

		void writeTimeout();

	private:
		void writeDefaultFile();

		bool writeCurrentSettings();

		bool readCustomFile();

		void readBlocklist();

		void writing();

		QTimer _jsonWriteTimer;

	};

	void Start();

	void Write();

	[[nodiscard]] bool WriteNow();

	void Finish();

	[[nodiscard]] QByteArray ExportDocument();

	[[nodiscard]] bool CanImportDocument(const QByteArray &content);

	[[nodiscard]] bool ImportDocument(const QByteArray &content);

} // namespace EnhancedSettings
