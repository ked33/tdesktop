/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_mpv.h"

#include "media/streaming/media_streaming_mpv_special.h"

#include "base/bytes.h"
#include "base/platform/base_platform_info.h"
#include "base/timer.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_session.h"
#include "data/data_streaming.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_session.h"
#include "media/streaming/media_streaming_boost.h"
#include "media/streaming/media_streaming_mp4_header.h"
#include "media/streaming/media_streaming_reader.h"
#include "logs.h"
#include "settings.h"

#include <QtCore/QFileInfo>
#include <QtCore/QCoreApplication>
#include <QtCore/QLibrary>
#include <QtCore/QMetaObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QStringList>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtCore/QUrl>
#include <QtGui/QCloseEvent>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace Media::Streaming::Mpv {
namespace {

using Mp4Layout = Mp4::Layout;

constexpr auto kPathPrefix = "/mpv/";
constexpr auto kPathPrefixLength = 5;
constexpr auto kHeadersLimit = 64 * 1024;
constexpr auto kReadChunkSize = 256 * 1024;
constexpr auto kInitialReadChunkSize = 64 * 1024;
constexpr auto kMp4LayoutWaitTimeout = 4000;
constexpr auto kMp4LayoutWaitStep = 10;
constexpr auto kCleanupInterval = 60 * crl::time(1000);
constexpr auto kTokenLifetime = 5 * 60 * crl::time(1000);
constexpr auto kPlayerStartTimeout = 5000;
constexpr auto kMpvLoaderPriority = 2;
constexpr auto kSmartSeekStartupGrace = crl::time(2000);
constexpr auto kSmartSeekDuplicateInterval = crl::time(750);
constexpr auto kSmartSeekPressureMaximum = crl::time(1500);
constexpr auto kSmartSeekMinimumPlaybackBytes = int64(2) * 1024 * 1024;
constexpr auto kSmartSeekMinimumJump = int64(4) * 1024 * 1024;
constexpr auto kSmartSeekMaximumJump = int64(32) * 1024 * 1024;
constexpr auto kSmartSeekMinimumRange = int64(1024) * 1024;
constexpr auto kSmartSeekTailGuard = int64(4) * 1024 * 1024;
constexpr auto kSmartSeekPrefetchFallback = int64(4) * 1024 * 1024;

[[nodiscard]] bool MpvDebugLogsEnabled() {
	return GetEnhancedBool("mpv_streaming_debug_logs");
}

[[nodiscard]] bool LooksLikeMp4Stream(not_null<DocumentData*> document) {
	const auto mime = document->mimeString().toLower();
	if (mime == QStringLiteral("video/mp4")
		|| mime == QStringLiteral("video/quicktime")
		|| mime == QStringLiteral("video/x-m4v")) {
		return true;
	}
	const auto name = document->filename().toLower();
	return name.endsWith(QStringLiteral(".mp4"))
		|| name.endsWith(QStringLiteral(".mov"))
		|| name.endsWith(QStringLiteral(".m4v"));
}

[[nodiscard]] int64 TailPrefetchBytesForDocument(
		not_null<DocumentData*> document) {
	if (!LooksLikeMp4Stream(document)) {
		return 0;
	}
	const auto &profile = BoostProfileFor(std::clamp(
		GetEnhancedInt("net_download_speed_boost"), 0, 6));
	if (profile.mpvTailPrefetchParts <= 0) {
		return 0;
	}
	constexpr auto kPart = int64(128 * 1024);
	return profile.mpvTailPrefetchParts * kPart;
}

[[nodiscard]] QString MpvLogString(const char *value) {
	return value ? QString::fromUtf8(value) : QString();
}

[[nodiscard]] QString MpvLogLine(const char *value) {
	auto result = MpvLogString(value);
	while (result.endsWith('\n') || result.endsWith('\r')) {
		result.chop(1);
	}
	return result;
}

[[nodiscard]] int DownloadBoostLevel() {
	const auto boost = GetEnhancedInt("net_download_speed_boost");
	return std::clamp(boost, 0, 6);
}

[[nodiscard]] bool MpvStreamingBoostEnabled() {
	return (DownloadBoostLevel() > 0);
}

[[nodiscard]] int SmartPlaybackRateForDocument(
		not_null<DocumentData*> document) {
	return AveragePlaybackBytesPerSecond(
		document->size,
		document->duration());
}

[[nodiscard]] QStringList LaunchArguments(
		not_null<DocumentData*> document,
		const QString &url) {
	auto lavfOptions = u"ignore_editlist=1"_q;
	if (LooksLikeMp4Stream(document)) {
		// IGNIDX stops the MOV demuxer at the first mdat during open,
		// retaining the moov sample tables needed for indexed seeking.
		// The HTTP server also extends that mdat for a regular front
		// moov, so seeking to later samples does not scan intervening
		// root atoms left unparsed by IGNIDX.
		lavfOptions += u",fflags=+ignidx"_q;
	}
	auto result = QStringList{
		u"--force-window=immediate"_q,
		u"--demuxer-lavf-o=%1"_q.arg(lavfOptions),
	};
	const auto &profile = BoostProfileFor(DownloadBoostLevel());
	if (MpvStreamingBoostEnabled() && profile.mpvCacheMaxMb > 0) {
		result.push_back(QStringLiteral("--cache=yes"));
		result.push_back(QStringLiteral("--demuxer-seekable-cache=yes"));
		result.push_back(QStringLiteral("--demuxer-max-bytes=%1")
			.arg(int64(profile.mpvCacheMaxMb) * 1024 * 1024));
		result.push_back(QStringLiteral("--demuxer-max-back-bytes=%1")
			.arg(int64(profile.mpvCacheBackMb) * 1024 * 1024));
	}
	result.push_back(url);
	return result;
}

#define MPV_STREAMING_LOG(expr) \
	do { \
		if (MpvDebugLogsEnabled()) { \
			LOG(expr); \
		} \
	} while (false)

struct ParsedRequest {
	QByteArray method;
	QString token;
	QByteArray rangeHeader;
	bool valid = false;
};

struct ResponseRange {
	int64 from = 0;
	int64 till = 0;
	int64 length = 0;
	bool partial = false;
};

struct RangeResult {
	bool valid = false;
	bool satisfiable = false;
	ResponseRange range;
};

struct Entry {
	Entry(
		not_null<DocumentData*> document,
		Data::FileOrigin origin,
		std::shared_ptr<Reader> reader)
	: document(document)
	, origin(origin)
	, reader(std::move(reader))
	, size(this->reader ? this->reader->size() : 0)
	, smartPlaybackRate(SmartPlaybackRateForDocument(document))
	, smartOpenedAt(crl::now()) {
		smartActiveReader = this->reader;
		if (this->reader
			&& smartPlaybackRate > 0
			&& this->reader->smartStreamingEnabled()) {
			this->reader->setSmartStreamingPlaybackRate(smartPlaybackRate);
			MPV_STREAMING_LOG(("MPV Streaming: Smart reader playback=%1 "
				"size=%2 duration=%3.")
				.arg(smartPlaybackRate)
				.arg(qlonglong(document->size))
				.arg(qlonglong(document->duration())));
		}
	}

	not_null<DocumentData*> document;
	Data::FileOrigin origin;
	std::shared_ptr<Reader> reader;
	std::shared_ptr<Reader> seekReader;
	QString mime;
	int64 size = 0;
	std::atomic<int> activeRequests = 0;
	std::atomic<crl::time> lastActivity = 0;
	std::atomic<bool> removeWhenIdle = false;
	std::atomic<bool> headerFinalized = false;
	std::atomic<int> mp4Layout = 0;
	Mp4::HeaderPatch mp4HeaderPatch;
	std::atomic<std::uint64_t> latestSeekGeneration = 0;
	int smartPlaybackRate = 0;
	crl::time smartOpenedAt = 0;
	crl::time smartLastSeekAt = 0;
	int64 smartPlaybackTill = 0;
	int64 smartPlaybackBytes = 0;
	int64 smartLastSeekOffset = -1;
	std::uint64_t smartPlaybackGeneration = 0;
	std::weak_ptr<Reader> smartActiveReader;
	std::map<Reader*, int> smartPressureReaders;
	std::mutex smartStateMutex;
	std::mutex fillMutex;
	std::mutex seekFillMutex;
};

struct SmartRangeDecision {
	bool trackPlayback = false;
	bool seek = false;
	std::uint64_t generation = 0;
	int64 previousTill = 0;
	int64 jump = 0;
};

[[nodiscard]] SmartRangeDecision ClassifySmartRange(
		const std::shared_ptr<Entry> &entry,
		not_null<Reader*> reader,
		int64 offset,
		int64 length,
		bool startedFromZero,
		bool directRange) {
	if (!reader->smartStreamingEnabled() || entry->smartPlaybackRate <= 0) {
		return {};
	}
	const auto now = crl::now();
	const auto guard = std::lock_guard(entry->smartStateMutex);
	auto result = SmartRangeDecision{
		.generation = entry->smartPlaybackGeneration,
		.previousTill = entry->smartPlaybackTill,
	};
	if (startedFromZero) {
		result.trackPlayback = true;
		return result;
	}
	const auto jump = (offset >= entry->smartPlaybackTill)
		? (offset - entry->smartPlaybackTill)
		: (entry->smartPlaybackTill - offset);
	result.jump = jump;
	const auto continuationLimit = std::max<int64>(
		2 * kReadChunkSize,
		std::min<int64>(entry->smartPlaybackRate, kSmartSeekMinimumJump));
	if (entry->smartPlaybackBytes > 0 && jump <= continuationLimit) {
		result.trackPlayback = true;
		return result;
	}
	const auto seekJump = std::clamp<int64>(
		int64(entry->smartPlaybackRate) * 2,
		kSmartSeekMinimumJump,
		kSmartSeekMaximumJump);
	if (jump < seekJump) {
		result.trackPlayback = true;
		return result;
	}
	const auto duplicateJump = (entry->smartLastSeekOffset >= 0)
		? ((offset >= entry->smartLastSeekOffset)
			? (offset - entry->smartLastSeekOffset)
			: (entry->smartLastSeekOffset - offset))
		: seekJump;
	const auto usableDirectRange = directRange
		&& offset > 0
		&& length >= kSmartSeekMinimumRange
		&& offset <= entry->size
			- std::min(entry->size, kSmartSeekTailGuard)
		&& now >= entry->smartOpenedAt + kSmartSeekStartupGrace;
	if (entry->smartPlaybackBytes < kSmartSeekMinimumPlaybackBytes) {
		result.trackPlayback = usableDirectRange;
		return result;
	}
	if (!usableDirectRange
		|| (now < entry->smartLastSeekAt + kSmartSeekDuplicateInterval
			&& duplicateJump <= continuationLimit)) {
		return result;
	}
	result.trackPlayback = true;
	result.seek = true;
	result.generation = ++entry->smartPlaybackGeneration;
	entry->smartLastSeekAt = now;
	entry->smartLastSeekOffset = offset;
	return result;
}

void NoteSmartPlaybackProgress(
		const std::shared_ptr<Entry> &entry,
		const SmartRangeDecision &decision,
		int64 till,
		int size) {
	if (!decision.trackPlayback || size <= 0) {
		return;
	}
	const auto guard = std::lock_guard(entry->smartStateMutex);
	if (decision.generation != entry->smartPlaybackGeneration) {
		return;
	}
	entry->smartPlaybackTill = till;
	entry->smartPlaybackBytes = std::min(
		entry->smartPlaybackBytes + size,
		kSmartSeekMinimumPlaybackBytes);
}

void ActivateSmartReader(
		const std::shared_ptr<Entry> &entry,
		const std::shared_ptr<Reader> &reader,
		std::uint64_t generation) {
	if (!reader
		|| entry->smartPlaybackRate <= 0
		|| !reader->smartStreamingEnabled()) {
		return;
	}
	const auto guard = std::lock_guard(entry->smartStateMutex);
	if (generation != entry->smartPlaybackGeneration) {
		return;
	}
	const auto previous = entry->smartActiveReader.lock();
	if (previous == reader) {
		return;
	}
	if (previous) {
		previous->setSmartStreamingPlaybackRate(0);
	}
	reader->setSmartStreamingPlaybackRate(entry->smartPlaybackRate);
	entry->smartActiveReader = reader;
}

[[nodiscard]] bool BeginSmartSeekPressure(
		const std::shared_ptr<Entry> &entry,
		const std::shared_ptr<Reader> &reader,
		std::uint64_t generation) {
	const auto guard = std::lock_guard(entry->smartStateMutex);
	if (generation != entry->smartPlaybackGeneration) {
		return false;
	}
	++entry->smartPressureReaders[reader.get()];
	reader->setSmartStreamingBufferPressure(true);
	return true;
}

void EndSmartSeekPressure(
		const std::shared_ptr<Entry> &entry,
		const std::shared_ptr<Reader> &reader) {
	const auto guard = std::lock_guard(entry->smartStateMutex);
	const auto i = entry->smartPressureReaders.find(reader.get());
	if (i == end(entry->smartPressureReaders)) {
		return;
	} else if (--i->second == 0) {
		reader->setSmartStreamingBufferPressure(false);
		entry->smartPressureReaders.erase(i);
	}
}

void PrefetchSmartSeekIfCurrent(
		const std::shared_ptr<Entry> &entry,
		const std::shared_ptr<Reader> &reader,
		const SmartRangeDecision &decision,
		int64 offset) {
	const auto guard = std::lock_guard(entry->smartStateMutex);
	if (decision.generation == entry->smartPlaybackGeneration) {
		reader->prefetch({
			.generation = decision.generation,
			.offset = offset,
			.amount = kSmartSeekPrefetchFallback,
			.fallbackUrgentOffset = offset,
		});
	}
}

[[nodiscard]] std::shared_ptr<Reader> CreateDedicatedReader(
	not_null<DocumentData*> document,
	Data::FileOrigin origin);
[[nodiscard]] std::shared_ptr<Reader> CreateDedicatedReaderFromWorker(
	not_null<DocumentData*> document,
	Data::FileOrigin origin);
[[nodiscard]] bool FillBuffer(
	not_null<Reader*> reader,
	int64 offset,
	bytes::span buffer);

[[nodiscard]] QString StreamingErrorDebugString(std::optional<Error> error) {
	if (!error) {
		return QStringLiteral("none");
	}
	switch (*error) {
	case Error::OpenFailed: return QStringLiteral("OpenFailed");
	case Error::LoadFailed: return QStringLiteral("LoadFailed");
	case Error::InvalidData: return QStringLiteral("InvalidData");
	case Error::NotStreamable: return QStringLiteral("NotStreamable");
	}
	return QStringLiteral("Unknown(%1)").arg(int(*error));
}

[[nodiscard]] std::shared_ptr<Reader> CreateDedicatedReader(
		not_null<DocumentData*> document,
		Data::FileOrigin origin) {
	auto loader = document->createStreamingLoader(origin, false);
	if (!loader) {
		return nullptr;
	}
	auto reader = std::make_shared<Reader>(
		std::move(loader),
		nullptr);
	reader->setLoaderPriority(kMpvLoaderPriority);
	if (const auto bytes = TailPrefetchBytesForDocument(document)) {
		reader->requestTailPrefetch(bytes);
	}
	reader->startStreaming();
	return reader;
}

[[nodiscard]] std::shared_ptr<Reader> CreateDedicatedReaderFromWorker(
		not_null<DocumentData*> document,
		Data::FileOrigin origin) {
	auto result = std::shared_ptr<Reader>();
	auto semaphore = crl::semaphore();
	crl::on_main(&document->session(), [=, &result, &semaphore] {
		result = CreateDedicatedReader(document, origin);
		semaphore.release();
	});
	semaphore.acquire();
	return result;
}

	[[nodiscard]] bool RecoverEntryReader(
			const std::shared_ptr<Entry> &entry,
			const QString &token,
			int64 offset,
			const SmartRangeDecision &smartRange) {
		const auto fresh = CreateDedicatedReaderFromWorker(
			entry->document,
			entry->origin);
			if (!fresh) {
				MPV_STREAMING_LOG(("MPV Streaming: Failed to recreate reader for token %1 at offset %2.")
					.arg(token)
					.arg(offset));
				return false;
			}
		if (smartRange.trackPlayback) {
			ActivateSmartReader(
				entry,
				fresh,
				smartRange.generation);
		}
		const auto previous = std::move(entry->reader);
		entry->reader = fresh;
		entry->headerFinalized = false;
		if (previous) {
			previous->stopStreamingAsync();
			previous->tryRemoveLoaderAsync();
		}
		MPV_STREAMING_LOG(("MPV Streaming: Recreated reader for token %1 after LoadFailed at offset %2.")
			.arg(token)
			.arg(offset));
		return true;
	}

[[nodiscard]] QString ResolveProgram() {
	auto configured = GetEnhancedString("mpv_path").trimmed();
	if (configured.startsWith('"') && configured.endsWith('"') && configured.size() > 1) {
		configured = configured.mid(1, configured.size() - 2);
	}
	if (!configured.isEmpty()) {
		if (configured.contains('/') || configured.contains('\\')) {
			const auto info = QFileInfo(configured);
			return info.isFile() ? info.absoluteFilePath() : QString();
		}
		return QStandardPaths::findExecutable(configured);
	}
	auto result = QStandardPaths::findExecutable(QStringLiteral("mpv"));
	if (result.isEmpty()) {
		result = QStandardPaths::findExecutable(QStringLiteral("mpv.exe"));
	}
	return result;
}

[[nodiscard]] QProcessEnvironment LaunchEnvironment() {
	auto result = QProcessEnvironment::systemEnvironment();
	for (const auto &name : {
		QStringLiteral("HTTP_PROXY"),
		QStringLiteral("http_proxy"),
		QStringLiteral("HTTPS_PROXY"),
		QStringLiteral("https_proxy"),
		QStringLiteral("ALL_PROXY"),
		QStringLiteral("all_proxy"),
	}) {
		result.remove(name);
	}
	const auto noProxy = QStringLiteral("127.0.0.1,localhost");
	result.insert(QStringLiteral("NO_PROXY"), noProxy);
	result.insert(QStringLiteral("no_proxy"), noProxy);
	return result;
}

[[nodiscard]] QByteArray ReadHeaders(QTcpSocket &socket) {
	auto result = QByteArray();
	while (!result.contains("\r\n\r\n")) {
		if (result.size() > kHeadersLimit) {
			return {};
		} else if (!socket.bytesAvailable() && !socket.waitForReadyRead(30000)) {
			return {};
		}
		const auto bytes = socket.readAll();
		if (bytes.isEmpty() && socket.state() != QAbstractSocket::ConnectedState) {
			return {};
		}
		result += bytes;
	}
	return result;
}

[[nodiscard]] ParsedRequest ParseRequest(const QByteArray &headers) {
	const auto lines = headers.split('\n');
	if (lines.empty()) {
		return {};
	}
	const auto firstLine = lines.front().trimmed();
	const auto parts = firstLine.split(' ');
	if (parts.size() < 2) {
		return {};
	}
	const auto method = parts[0].trimmed().toUpper();
	if (method != "GET" && method != "HEAD") {
		return {};
	}
	const auto path = QUrl::fromEncoded(parts[1].trimmed()).path();
	if (!path.startsWith(QLatin1String(kPathPrefix))) {
		return {};
	}
	auto result = ParsedRequest{
		.method = method,
		.token = path.mid(kPathPrefixLength),
		.valid = true,
	};
	for (auto i = 1; i != lines.size(); ++i) {
		const auto line = lines[i].trimmed();
		if (line.isEmpty()) {
			break;
		}
		const auto colon = line.indexOf(':');
		if (colon <= 0) {
			continue;
		}
		const auto name = line.mid(0, colon).trimmed().toLower();
		if (name == "range") {
			result.rangeHeader = line.mid(colon + 1).trimmed();
		}
	}
	return result;
}

[[nodiscard]] RangeResult ParseRange(const QByteArray &header, int64 size) {
	if (size <= 0) {
		return {};
	} else if (header.isEmpty()) {
		return {
			.valid = true,
			.satisfiable = true,
			.range = {
				.from = 0,
				.till = size - 1,
				.length = size,
				.partial = false,
			},
		};
	}
	const auto trimmed = header.trimmed();
	if (!trimmed.startsWith("bytes=") || trimmed.contains(',')) {
		return {};
	}
	const auto value = trimmed.mid(6);
	const auto dash = value.indexOf('-');
	if (dash < 0) {
		return {};
	}
	const auto fromPart = value.mid(0, dash).trimmed();
	const auto tillPart = value.mid(dash + 1).trimmed();
	auto from = int64(0);
	auto till = size - 1;
	auto ok = false;
	if (fromPart.isEmpty()) {
		const auto suffixLength = tillPart.toLongLong(&ok);
		if (!ok || suffixLength <= 0) {
			return {};
		}
		from = (suffixLength >= size) ? 0 : (size - suffixLength);
	} else {
		from = fromPart.toLongLong(&ok);
		if (!ok || from < 0) {
			return {};
		} else if (from >= size) {
			return {
				.valid = true,
				.satisfiable = false,
			};
		}
		if (!tillPart.isEmpty()) {
			till = tillPart.toLongLong(&ok);
			if (!ok || till < from) {
				return {};
			}
		}
	}
	till = std::min(till, size - 1);
	return {
		.valid = true,
		.satisfiable = true,
		.range = {
			.from = from,
			.till = till,
			.length = till - from + 1,
			.partial = true,
		},
	};
}

[[nodiscard]] bool WriteAll(QTcpSocket &socket, const QByteArray &data) {
	auto written = int64(0);
	while (written < data.size()) {
		const auto amount = socket.write(data.constData() + written, data.size() - written);
		if (amount < 0) {
			return false;
		} else if (!amount && !socket.waitForBytesWritten(30000)) {
			return false;
		}
		written += amount;
	}
	return socket.waitForBytesWritten(30000) || !socket.bytesToWrite();
}

[[nodiscard]] bool WriteAll(
		QTcpSocket &socket,
		const char *data,
		int size) {
	auto written = 0;
	while (written < size) {
		const auto amount = socket.write(data + written, size - written);
		if (amount < 0) {
			return false;
		} else if (!amount && !socket.waitForBytesWritten(30000)) {
			return false;
		}
		written += amount;
	}
	return socket.waitForBytesWritten(30000) || !socket.bytesToWrite();
}

[[nodiscard]] bool SendResponse(
		QTcpSocket &socket,
		QByteArray status,
		std::initializer_list<std::pair<QByteArray, QByteArray>> headers) {
	auto data = QByteArray("HTTP/1.1 ");
	data += status;
	data += "\r\n";
	for (const auto &[name, value] : headers) {
		data += name;
		data += ": ";
		data += value;
		data += "\r\n";
	}
	data += "\r\n";
	return WriteAll(socket, data);
}

[[nodiscard]] bool FillBuffer(
		not_null<Reader*> reader,
		int64 offset,
		bytes::span buffer) {
	auto semaphore = crl::semaphore();
	while (true) {
		const auto state = reader->fill(offset, buffer, &semaphore);
		if (state == Reader::FillState::Success) {
			return true;
		} else if (state == Reader::FillState::Failed) {
			return false;
		}
		semaphore.acquire();
		if (reader->streamingError()) {
			return false;
		}
	}
}

[[nodiscard]] bool WaitForMp4LayoutForSeek(
		const std::shared_ptr<Entry> &entry,
		const QString &token,
		int64 offset) {
	if (entry->mp4Layout.load() != 0) {
		return true;
	}
	const auto started = crl::now();
	while (entry->mp4Layout.load() == 0) {
		if ((crl::now() - started) >= kMp4LayoutWaitTimeout) {
			MPV_STREAMING_LOG(("MPV Streaming: MP4 layout wait timed out for token %1 offset=%2 activeRequests=%3.")
				.arg(token)
				.arg(offset)
				.arg(entry->activeRequests.load()));
			return false;
		}
		std::this_thread::sleep_for(
			std::chrono::milliseconds(kMp4LayoutWaitStep));
	}
	return true;
}

class DescriptorServer final : public QTcpServer {
public:
	explicit DescriptorServer(std::function<void(qintptr)> accepted)
	: _accepted(std::move(accepted)) {
	}

protected:
	void incomingConnection(qintptr descriptor) override {
		if (_accepted) {
			_accepted(descriptor);
		} else {
			QTcpServer::incomingConnection(descriptor);
		}
	}

private:
	std::function<void(qintptr)> _accepted;
};

class Server final {
public:
	struct Launch {
		QString token;
		QString url;
	};

	Server()
	: _server([this](qintptr descriptor) { handleDescriptor(descriptor); })
	, _cleanupTimer([=] { cleanup(); }) {
	}

	[[nodiscard]] Launch add(
			not_null<DocumentData*> document,
			Data::FileOrigin origin,
			std::shared_ptr<Reader> reader) {
		if (!ensureListening()) {
			return {};
		}
		auto entry = std::make_shared<Entry>(
			document,
			origin,
			std::move(reader));
		entry->mime = document->mimeString().isEmpty()
			? QStringLiteral("application/octet-stream")
			: document->mimeString();
		entry->lastActivity = crl::now();

		auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
		{
			const auto guard = std::lock_guard(_entriesMutex);
			_entries.emplace(token, entry);
		}
		scheduleCleanup();
		return {
			.token = token,
			.url = QString("http://127.0.0.1:%1%2%3")
				.arg(_server.serverPort())
				.arg(QString::fromLatin1(kPathPrefix))
				.arg(token),
		};
	}

	void remove(const QString &token) {
		auto removed = std::shared_ptr<Entry>();
		{
			const auto guard = std::lock_guard(_entriesMutex);
			const auto i = _entries.find(token);
			if (i == end(_entries)) {
				return;
			} else if (i->second->activeRequests.load() > 0) {
				i->second->removeWhenIdle = true;
				return;
			}
			removed = i->second;
			_entries.erase(i);
		}
		removed->reader->stopStreaming(false);
		if (removed->seekReader) {
			removed->seekReader->stopStreaming(false);
		}
	}

	static Server &instance() {
		static auto result = Server();
		return result;
	}

private:
	[[nodiscard]] bool ensureListening() {
		return _server.isListening()
			|| _server.listen(QHostAddress::LocalHost, 0);
	}

	[[nodiscard]] std::shared_ptr<Entry> lookupRetained(const QString &token) {
		const auto guard = std::lock_guard(_entriesMutex);
		const auto i = _entries.find(token);
		if (i == end(_entries)) {
			return nullptr;
		} else if (i->second->removeWhenIdle.load()) {
			return nullptr;
		}
		i->second->activeRequests.fetch_add(1);
		i->second->lastActivity = crl::now();
		return i->second;
	}

	void release(const QString &token, const std::shared_ptr<Entry> &entry) {
		entry->lastActivity = crl::now();
		const auto active = entry->activeRequests.fetch_sub(1) - 1;
		if (!active && entry->removeWhenIdle.load()) {
			crl::on_main([token] {
				Server::instance().remove(token);
			});
		}
	}

	void scheduleCleanup() {
		_cleanupTimer.callOnce(kCleanupInterval);
	}

	void cleanup() {
		const auto now = crl::now();
		auto removed = std::vector<std::shared_ptr<Entry>>();
		{
			const auto guard = std::lock_guard(_entriesMutex);
			for (auto i = begin(_entries); i != end(_entries);) {
				const auto entry = i->second;
				if (!entry->activeRequests.load()
					&& (now - entry->lastActivity.load()) >= kTokenLifetime) {
					removed.push_back(entry);
					i = _entries.erase(i);
				} else {
					++i;
				}
			}
		}
		for (const auto &entry : removed) {
			entry->reader->stopStreaming(false);
			if (entry->seekReader) {
				entry->seekReader->stopStreaming(false);
			}
		}
		const auto guard = std::lock_guard(_entriesMutex);
		if (!_entries.empty()) {
			_cleanupTimer.callOnce(kCleanupInterval);
		}
	}

	void handleDescriptor(qintptr descriptor) {
		if (descriptor < 0) {
			return;
		}
		std::thread([this, descriptor] {
			handleConnection(descriptor);
		}).detach();
	}

		void handleConnection(qintptr descriptor) {
			auto socket = QTcpSocket();
				if (!socket.setSocketDescriptor(descriptor)) {
					MPV_STREAMING_LOG(("MPV Streaming: Failed to adopt socket descriptor %1.")
						.arg(qulonglong(descriptor)));
					return;
				}
			const auto headers = ReadHeaders(socket);
			const auto request = ParseRequest(headers);
				if (!request.valid) {
					MPV_STREAMING_LOG(("MPV Streaming: Invalid request, headers size %1.")
						.arg(headers.size()));
				(void)SendResponse(socket, "400 Bad Request", {
					{ "Connection", "close" },
					{ "Content-Length", "0" },
				});
				return;
			}
				MPV_STREAMING_LOG(("MPV Streaming: Request %1 token=%2 range='%3'.")
					.arg(QString::fromLatin1(request.method))
					.arg(request.token)
					.arg(QString::fromLatin1(request.rangeHeader)));
			const auto entry = lookupRetained(request.token);
				if (!entry) {
					MPV_STREAMING_LOG(("MPV Streaming: Token not found: %1.").arg(request.token));
				(void)SendResponse(socket, "404 Not Found", {
					{ "Connection", "close" },
					{ "Content-Length", "0" },
				});
				return;
			}
			const auto releaseGuard = gsl::finally([&] {
				release(request.token, entry);
			});
			const auto range = ParseRange(request.rangeHeader, entry->size);
				if (!range.valid) {
					MPV_STREAMING_LOG(("MPV Streaming: Invalid range '%1' for size %2.")
						.arg(QString::fromLatin1(request.rangeHeader))
						.arg(entry->size));
				(void)SendResponse(socket, "400 Bad Request", {
					{ "Connection", "close" },
					{ "Content-Length", "0" },
				});
				return;
				} else if (!range.satisfiable) {
					MPV_STREAMING_LOG(("MPV Streaming: Unsatisfiable range '%1' for size %2.")
						.arg(QString::fromLatin1(request.rangeHeader))
						.arg(entry->size));
				(void)SendResponse(socket, "416 Range Not Satisfiable", {
					{ "Accept-Ranges", "bytes" },
					{ "Connection", "close" },
					{ "Content-Length", "0" },
					{ "Content-Range", QByteArray("bytes */") + QByteArray::number(entry->size) },
				});
				return;
			}
			// Fragmented MP4 keeps the sequential-open fallback.
			// A large front moov only requires an isolated reader
			// for range requests, so probing and playback reads do
			// not compete for the primary reader's download window.
			if (entry->mp4Layout.load() == 0
				&& range.range.from == 0) {
				const auto lock = std::unique_lock(entry->fillMutex);
				if (entry->mp4Layout.load() == 0) {
					const auto header = Mp4::ProbeForStreaming(
						entry->size,
						[&](std::int64_t offset, std::span<char> buffer) {
							return FillBuffer(
								entry->reader.get(),
								offset,
								bytes::span(
									reinterpret_cast<bytes::type*>(buffer.data()),
									buffer.size()));
						});
					entry->mp4HeaderPatch = header.patch;
					entry->mp4Layout.store(int(header.layout));
					MPV_STREAMING_LOG(("MPV Streaming: Detected MP4 layout: %1 "
						"for token %2, header patch offset=%3 size=%4.")
						.arg(int(header.layout))
						.arg(request.token)
						.arg(qlonglong(header.patch.offset))
						.arg(header.patch.size));
				}
			}
			if ((range.range.from > 0)
				&& !WaitForMp4LayoutForSeek(
					entry,
					request.token,
					range.range.from)) {
				(void)SendResponse(socket, "503 Service Unavailable", {
					{ "Connection", "close" },
					{ "Content-Length", "0" },
					{ "Retry-After", "1" },
				});
				return;
			}
			const auto layout = Mp4Layout(entry->mp4Layout.load());
			const auto headerPatch = entry->mp4HeaderPatch;
			const auto compatibilitySequentialRequest =
				(layout == Mp4Layout::Fragmented);
			const auto isolatedSeekRequest =
				(layout == Mp4Layout::LargeFrontMoov)
				&& (range.range.from > 0);
			if (compatibilitySequentialRequest) {
				if (!SendResponse(socket, "200 OK", {
					{ "Connection", "close" },
					{ "Content-Length", QByteArray::number(entry->size) },
					{ "Content-Type", entry->mime.toUtf8() },
				})) {
					return;
				}
			} else if (range.range.partial) {
				const auto contentRange = QByteArray("bytes ")
					+ QByteArray::number(range.range.from)
					+ '-'
					+ QByteArray::number(range.range.till)
					+ '/'
					+ QByteArray::number(entry->size);
				if (!SendResponse(socket, "206 Partial Content", {
					{ "Accept-Ranges", "bytes" },
					{ "Connection", "close" },
					{ "Content-Length", QByteArray::number(range.range.length) },
					{ "Content-Type", entry->mime.toUtf8() },
					{ "Content-Range", contentRange },
				})) {
					return;
				}
			} else if (!SendResponse(socket, "200 OK", {
				{ "Accept-Ranges", "bytes" },
				{ "Connection", "close" },
				{ "Content-Length", QByteArray::number(range.range.length) },
				{ "Content-Type", entry->mime.toUtf8() },
			})) {
				return;
			}
			auto offset = compatibilitySequentialRequest
				? int64(0)
				: range.range.from;
			auto left = compatibilitySequentialRequest
				? entry->size
				: range.range.length;
			MPV_STREAMING_LOG(("MPV Streaming: Response status=%1 "
				"token=%2 offset=%3 length=%4 layout=%5.")
				.arg(!compatibilitySequentialRequest && range.range.partial
					? 206
					: 200)
				.arg(request.token)
				.arg(offset)
				.arg(left)
				.arg(int(layout)));
			if (request.method == "HEAD") {
				return;
			}
			const auto startedFromZero = (offset == 0);
			auto activeReader = entry->reader;
			auto *fillMutex = &entry->fillMutex;
			if (isolatedSeekRequest) {
				const auto lock = std::unique_lock(entry->seekFillMutex);
				if (!entry->seekReader) {
					entry->seekReader = CreateDedicatedReaderFromWorker(
						entry->document,
						entry->origin);
				}
				if (entry->seekReader) {
					activeReader = entry->seekReader;
					fillMutex = &entry->seekFillMutex;
					MPV_STREAMING_LOG(("MPV Streaming: Using isolated seek reader for token %1 at offset %2.")
						.arg(request.token)
						.arg(range.range.from));
				} else {
					MPV_STREAMING_LOG(("MPV Streaming: Failed to create isolated seek reader for token %1 at offset %2, using primary reader.")
						.arg(request.token)
						.arg(range.range.from));
				}
			}
			const auto usingSeekReader = (activeReader != entry->reader);
			const auto smartRange = ClassifySmartRange(
				entry,
				activeReader.get(),
				offset,
				left,
				startedFromZero,
				!compatibilitySequentialRequest
					&& (range.range.from > 0));
			if (smartRange.trackPlayback) {
				ActivateSmartReader(
					entry,
					activeReader,
					smartRange.generation);
			}
			auto smartPressureActive = smartRange.seek
				&& BeginSmartSeekPressure(
					entry,
					activeReader,
					smartRange.generation);
			auto smartPrefetchPending = smartPressureActive;
			auto smartSeekServed = int64(0);
			const auto smartPressureStarted = crl::now();
			if (smartPressureActive) {
				activeReader->notifySmartStreamingSeek();
				MPV_STREAMING_LOG(("MPV Streaming: Smart seek "
					"target=%1 previous=%2 jump=%3 length=%4 "
					"playback=%5 layout=%6 isolated=%7.")
					.arg(qlonglong(offset))
					.arg(qlonglong(smartRange.previousTill))
					.arg(qlonglong(smartRange.jump))
					.arg(qlonglong(left))
					.arg(entry->smartPlaybackRate)
					.arg(entry->mp4Layout.load())
					.arg(usingSeekReader ? 1 : 0));
			}
			const auto smartPressureGuard = gsl::finally([&] {
				if (smartPressureActive) {
					EndSmartSeekPressure(entry, activeReader);
				}
			});
			const auto seekGenerationManaged =
				(entry->mp4Layout.load() == int(Mp4Layout::LargeFrontMoov))
				&& isolatedSeekRequest;
			const auto seekGeneration = seekGenerationManaged
				? (entry->latestSeekGeneration.fetch_add(1) + 1)
				: std::uint64_t(0);
			const auto seekSuperseded = [&] {
				return seekGenerationManaged
					&& (entry->latestSeekGeneration.load() != seekGeneration);
			};
			auto retriedLoadFailure = false;
			auto clientDisconnected = false;
			auto supersededSeek = false;
			while (left > 0) {
				if (seekSuperseded()) {
					supersededSeek = true;
					break;
				}
				// Check if client disconnected before acquiring the lock.
				socket.waitForReadyRead(0);
				if (socket.state() != QAbstractSocket::ConnectedState) {
					clientDisconnected = true;
					break;
				}
				const auto chunkSize = (offset == range.range.from)
					? kInitialReadChunkSize
					: kReadChunkSize;
				const auto size = int(std::min(left, int64(chunkSize)));
				auto buffer = QByteArray(size, Qt::Uninitialized);
				{
					const auto lock = std::unique_lock(*fillMutex);
					if (seekSuperseded()) {
						supersededSeek = true;
						break;
					}
					// Re-check after acquiring the lock.
					socket.waitForReadyRead(0);
					if (socket.state() != QAbstractSocket::ConnectedState) {
						clientDisconnected = true;
						break;
					}
					if (smartPrefetchPending) {
						PrefetchSmartSeekIfCurrent(
							entry,
							activeReader,
							smartRange,
							range.range.from);
						smartPrefetchPending = false;
					}
					if (!FillBuffer(
							activeReader.get(),
							offset,
							bytes::span(
								reinterpret_cast<bytes::type*>(buffer.data()),
								size))) {
						const auto error = activeReader->streamingError();
						if (!usingSeekReader
							&& !retriedLoadFailure
							&& error
							&& (*error == Error::LoadFailed)
							&& RecoverEntryReader(
								entry,
								request.token,
								offset,
								smartRange)) {
							retriedLoadFailure = true;
							continue;
						}
						MPV_STREAMING_LOG(("MPV Streaming: FillBuffer failed at offset %1, size %2, error=%3.")
							.arg(offset)
							.arg(size)
							.arg(StreamingErrorDebugString(error)));
						return;
					}
					if (startedFromZero
						&& !usingSeekReader
						&& !entry->headerFinalized.exchange(true)) {
						entry->reader->headerDone();
					}
				} // fillMutex released here
				if (seekSuperseded()) {
					supersededSeek = true;
					break;
				}
				headerPatch.apply(
					offset,
					std::span(buffer.data(), std::size_t(buffer.size())));
				if (!WriteAll(socket, buffer.constData(), buffer.size())) {
					const auto error = socket.error();
					if (error != QAbstractSocket::RemoteHostClosedError) {
						MPV_STREAMING_LOG(("MPV Streaming: WriteAll failed at offset %1, size %2, error=%3, detail='%4'.")
							.arg(offset)
							.arg(size)
							.arg(int(error))
							.arg(socket.errorString()));
					}
					clientDisconnected = true;
					break;
				}
				retriedLoadFailure = false;
				offset += size;
				left -= size;
				NoteSmartPlaybackProgress(
					entry,
					smartRange,
					offset,
					size);
				if (smartPressureActive) {
					smartSeekServed += size;
					if (smartSeekServed >= kSmartSeekMinimumPlaybackBytes
						|| crl::now() >= smartPressureStarted
							+ kSmartSeekPressureMaximum) {
						EndSmartSeekPressure(entry, activeReader);
						smartPressureActive = false;
					}
				}
				entry->lastActivity = crl::now();
			}
			if (supersededSeek) {
				MPV_STREAMING_LOG(("MPV Streaming: Superseded seek request token=%1 generation=%2 offset=%3 latest=%4.")
					.arg(request.token)
					.arg(qulonglong(seekGeneration))
					.arg(range.range.from)
					.arg(qulonglong(entry->latestSeekGeneration.load())));
				return;
			}
			// Pre-fill cache sequentially after client disconnect.
			// When a fragmented MP4 is opened, the demuxer scans
			// hundreds of fragment headers via HTTP range requests.
			// By continuing to fill the cache sequentially here,
			// those seek connections find data already cached and
			// complete almost instantly instead of each downloading
			// from Telegram independently (~150ms per seek).
			if (clientDisconnected
				&& startedFromZero
				&& layout != Mp4Layout::LargeFrontMoov
				&& left > 0) {
				while (left > 0 && entry->activeRequests.load() > 1) {
					const auto size = int(std::min(left, int64(kReadChunkSize)));
					auto buffer = QByteArray(size, Qt::Uninitialized);
					{
						const auto lock = std::unique_lock(entry->fillMutex);
						if (entry->activeRequests.load() <= 1) {
							break;
						}
						const auto fillStart = crl::now();
						if (!FillBuffer(
								entry->reader.get(),
								offset,
								bytes::span(
									reinterpret_cast<bytes::type*>(buffer.data()),
									size))) {
							break;
						}
						// Stop if FillBuffer was slow (cache miss).
						// A slow fill means the Reader had to download
						// from Telegram at this offset, indicating our
						// sequential position diverged from the loader.
						// Continuing would thrash the Reader's position
						// between our offset and seek connections' offsets.
						if (crl::now() - fillStart > 50) {
							break;
						}
						if (!entry->headerFinalized.exchange(true)) {
							entry->reader->headerDone();
						}
					}
					offset += size;
					left -= size;
					entry->lastActivity = crl::now();
					// Yield to let seek connections acquire the lock.
					std::this_thread::sleep_for(
						std::chrono::milliseconds(1));
				}
			}
		}

	std::mutex _entriesMutex;
	std::map<QString, std::shared_ptr<Entry>> _entries;
	DescriptorServer _server;
	base::Timer _cleanupTimer;
};

[[nodiscard]] bool StartManagedPlayer(
		const QString &program,
		const QStringList &arguments,
		const QString &token) {
	auto process = std::make_unique<QProcess>();
	const auto raw = process.get();
	raw->setProgram(program);
	raw->setArguments(arguments);
	raw->setWorkingDirectory(QFileInfo(program).absolutePath());
	raw->setProcessEnvironment(LaunchEnvironment());
	raw->setStandardOutputFile(QProcess::nullDevice());
	raw->setStandardErrorFile(QProcess::nullDevice());
	QObject::connect(
		raw,
		QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
		raw,
		[token, raw](int exitCode, QProcess::ExitStatus exitStatus) {
			MPV_STREAMING_LOG(("MPV Streaming: Player exited token=%1 code=%2 status=%3.")
				.arg(token)
				.arg(exitCode)
				.arg(int(exitStatus)));
			Server::instance().remove(token);
			raw->deleteLater();
		});
	raw->start();
	if (!raw->waitForStarted(kPlayerStartTimeout)) {
		return false;
	}
	process.release();
	return true;
}

[[nodiscard]] OpenResult StartExternalBridgePlayback(
		not_null<DocumentData*> document,
		Data::FileOrigin origin,
		const QString &program,
		std::shared_ptr<Reader> reader) {
	const auto launch = Server::instance().add(
		document,
		origin,
		std::move(reader));
	if (launch.url.isEmpty()) {
		MPV_STREAMING_LOG(("MPV Streaming: Failed to create launch URL for document %1.")
			.arg(qulonglong(document->id)));
		return OpenResult::Failed;
	}
	MPV_STREAMING_LOG(("MPV Streaming: Launching '%1' with URL %2.")
		.arg(program)
		.arg(launch.url));
	const auto arguments = LaunchArguments(document, launch.url);
	MPV_STREAMING_LOG(("MPV Streaming: Launch arguments: %1.")
		.arg(arguments.join(u" "_q)));
	if (!StartManagedPlayer(program, arguments, launch.token)) {
		MPV_STREAMING_LOG(("MPV Streaming: Failed to start player '%1'.")
			.arg(program));
		Server::instance().remove(launch.token);
		return OpenResult::Failed;
	}
	return OpenResult::Success;
}

} // namespace

	bool CanOpenVideoMessageInMpv(HistoryItem *item, DocumentData *document) {
	#ifndef Q_OS_WIN
		return false;
	#else
		if (!item || !document) {
			return false;
		}
		const auto media = item->media();
		return media
			&& (media->ttlSeconds() <= 0)
			&& document->size > 0
			&& document->useStreamingLoader()
			&& (document->isVideoFile() || document->isVideoMessage());
	#endif
	}

OpenResult OpenVideoMessageInMpv(
		HistoryItem *item,
		DocumentData *document) {
	const auto media = item ? item->media() : nullptr;
	const auto mediaDocument = media ? media->document() : nullptr;
	MPV_STREAMING_LOG(("MPV Streaming: Open request passedDocument=%1 mediaDocument=%2 same=%3 passedSize=%4 mediaSize=%5 passedSupports=%6 mediaSupports=%7 passedLoader=%8 mediaLoader=%9 hasQualities=%10.")
		.arg(qulonglong(document ? document->id : 0))
		.arg(qulonglong(mediaDocument ? mediaDocument->id : 0))
		.arg((document == mediaDocument) ? 1 : 0)
		.arg(document ? document->size : 0)
		.arg(mediaDocument ? mediaDocument->size : 0)
		.arg(document ? document->supportsStreaming() : 0)
		.arg(mediaDocument ? mediaDocument->supportsStreaming() : 0)
		.arg(document ? document->useStreamingLoader() : 0)
		.arg(mediaDocument ? mediaDocument->useStreamingLoader() : 0)
		.arg(media ? media->hasQualitiesList() : 0));
	if (!CanOpenVideoMessageInMpv(item, document)) {
		return OpenResult::Unsupported;
	}
	const auto program = ResolveProgram();
	if (program.isEmpty()) {
		MPV_STREAMING_LOG(("MPV Streaming: Player not found."));
		return OpenResult::PlayerNotFound;
	}
	const auto origin = Data::FileOrigin(item->fullId());
	auto reader = CreateDedicatedReader(document, origin);
	if (!reader) {
		MPV_STREAMING_LOG(("MPV Streaming: Failed to create dedicated reader for document %1.")
			.arg(qulonglong(document->id)));
		return OpenResult::Failed;
	}
	return StartExternalBridgePlayback(
		document,
		origin,
		program,
		std::move(reader));
}

OpenResult OpenVideoMessageInMpvSpecial(
		HistoryItem *item,
		DocumentData *document) {
	return MpvSpecial::OpenVideoMessageInMpvSpecial(item, document);
}

#undef MPV_STREAMING_LOG

} // namespace Media::Streaming::Mpv
