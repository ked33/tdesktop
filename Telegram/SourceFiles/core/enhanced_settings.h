/*
This file is part of 64Gram Desktop,
the unofficial app based on Telegram Desktop.
For license and copyright information please follow this link:
https://github.com/TDesktop-x64/tdesktop/blob/dev/LEGAL
*/
#pragma once

#include "rpl/producer.h"

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

	[[nodiscard]] int MessageEmojiSize();
	[[nodiscard]] int MessageStickerSize();
	[[nodiscard]] bool SearchIncludePorn();
	[[nodiscard]] rpl::producer<bool> SearchIncludePornChanges();
	void SetSearchIncludePorn(bool enabled);
	[[nodiscard]] int SearchPornConcurrency();
	[[nodiscard]] rpl::producer<int> SearchPornConcurrencyChanges();
	void SetSearchPornConcurrency(int value);

	class Manager : public QObject {
	Q_OBJECT

	public:
		Manager();

		void fill();

		void write(bool force = false);

		void addIdToBlocklist(int64 userId);

		void removeIdFromBlocklist(int64 userId);

	public Q_SLOTS:

		void writeTimeout();

	private:
		void writeDefaultFile();

		void writeCurrentSettings();

		bool readCustomFile();

		void readBlocklist();

		void writing();

		QTimer _jsonWriteTimer;

	};

	void Start();

	void Write();

	void Finish();

} // namespace EnhancedSettings
