/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_mpv.h"

#include "base/bytes.h"
#include "base/platform/base_platform_info.h"
#include "base/timer.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_session.h"
#include "data/data_streaming.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_session.h"
#include "media/streaming/media_streaming_reader.h"
#include "logs.h"
#include "settings.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QStringList>
#include <QtCore/QStandardPaths>
#include <QtCore/QUuid>
#include <QtCore/QUrl>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QLocalSocket>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace Media::Streaming::Mpv {
namespace {

constexpr auto kPathPrefix = "/mpv/";
constexpr auto kPathPrefixLength = 5;
constexpr auto kHeadersLimit = 64 * 1024;
constexpr auto kReadChunkSize = 256 * 1024;
constexpr auto kInitialReadChunkSize = 64 * 1024;
constexpr auto kBoostedReadChunkSize = 64 * 1024;
constexpr auto kCleanupInterval = 60 * crl::time(1000);
constexpr auto kTokenLifetime = 5 * 60 * crl::time(1000);
constexpr auto kMpvLoaderPriority = 2;
constexpr auto kBoostedMpvLoaderPriority = 8;
constexpr auto kSeekStormRequestThreshold = 32;
constexpr auto kSeekStormOffsetThreshold = 24 * 1024 * 1024;
constexpr auto kSlowFillThreshold = crl::time(300);
constexpr auto kSlowRequestThreshold = crl::time(1000);
constexpr auto kIpcTimeout = 500;

[[nodiscard]] bool MpvDebugLogsEnabled() {
	return GetEnhancedBool("mpv_streaming_debug_logs");
}

[[nodiscard]] int DownloadBoostLevel() {
	const auto boost = GetEnhancedInt("net_download_speed_boost");
	return std::clamp(boost, 0, 5);
}

[[nodiscard]] bool MpvStreamingBoostEnabled() {
	return (DownloadBoostLevel() > 0);
}

[[nodiscard]] QString TempBasePath() {
	if (const auto base = QStandardPaths::writableLocation(
			QStandardPaths::TempLocation); !base.isEmpty()) {
		return base;
	}
	return QDir::currentPath();
}

[[nodiscard]] QString MpvLogFilePath(const QString &token) {
	return QDir(TempBasePath()).filePath(
		QStringLiteral("tdesktop-mpv-%1.log").arg(token));
}

[[nodiscard]] QString MpvSessionHomePath(const QString &token) {
	return QDir(TempBasePath()).filePath(
		QStringLiteral("tdesktop-mpv-home-%1").arg(token));
}

[[nodiscard]] QString MpvIpcServerName(const QString &token) {
#ifdef Q_OS_WIN
	return QStringLiteral("\\\\.\\pipe\\tdesktop-mpv-%1-ipc").arg(token);
#else
	return QStringLiteral("tdesktop-mpv-%1-ipc").arg(token);
#endif
}

[[nodiscard]] QString FallbackExtension(not_null<DocumentData*> document) {
	const auto suffix = QFileInfo(document->filename()).suffix();
	if (!suffix.isEmpty()) {
		return QStringLiteral(".%1").arg(suffix);
	}
	const auto mime = document->mimeString();
	if (mime.contains(QStringLiteral("webm"), Qt::CaseInsensitive)) {
		return QStringLiteral(".webm");
	} else if (mime.contains(QStringLiteral("quicktime"), Qt::CaseInsensitive)) {
		return QStringLiteral(".mov");
	}
	return QStringLiteral(".mp4");
}

[[nodiscard]] QString MpvFallbackFilePath(
		not_null<DocumentData*> document,
		const QString &token) {
	return QDir(TempBasePath()).filePath(
		QStringLiteral("tdesktop-mpv-fallback-%1%2").arg(
			token,
			FallbackExtension(document)));
}

[[nodiscard]] bool PersistLoadedBytes(
		not_null<DocumentData*> document,
		const QString &path) {
	const auto media = document->activeMediaView();
	if (!media) {
		return false;
	}
	const auto bytes = media->bytes();
	if (bytes.isEmpty()) {
		return false;
	}
	QFile::remove(path);
	auto file = QFile(path);
	if (!file.open(QIODevice::WriteOnly)
		|| (file.write(bytes) != bytes.size())) {
		return false;
	}
	return true;
}

[[nodiscard]] QString TryResolveLocalLaunchPath(
		not_null<DocumentData*> document,
		const QString &token) {
	if (const auto path = document->filepath(true); !path.isEmpty()) {
		return path;
	}
	const auto path = MpvFallbackFilePath(document, token);
	return PersistLoadedBytes(document, path) ? path : QString();
}

[[nodiscard]] bool SendMpvLoadfileCommand(
		const QString &ipcName,
		const QString &path) {
	auto socket = QLocalSocket();
	socket.connectToServer(ipcName);
	if (!socket.waitForConnected(kIpcTimeout)) {
		return false;
	}
	const auto payload = QJsonDocument(QJsonObject{
		{ QStringLiteral("command"), QJsonArray{
			QStringLiteral("loadfile"),
			QDir::toNativeSeparators(path),
			QStringLiteral("replace"),
		} },
	}).toJson(QJsonDocument::Compact) + '\n';
	if (socket.write(payload) != payload.size()) {
		return false;
	}
	return socket.waitForBytesWritten(kIpcTimeout);
}

[[nodiscard]] QStringList LaunchArguments(
		const QString &source,
		const QString &token,
		const QString &ipcName) {
	auto result = QStringList{
		QStringLiteral("--no-config"),
		QStringLiteral("--player-operation-mode=cplayer"),
		QStringLiteral("--load-scripts=no"),
		QStringLiteral("--load-auto-profiles=no"),
		QStringLiteral("--load-select=no"),
		QStringLiteral("--load-console=no"),
		QStringLiteral("--ytdl=no"),
		QStringLiteral("--idle=no"),
		QStringLiteral("--force-window=immediate"),
	};
	if (!ipcName.isEmpty()) {
		result.push_back(QStringLiteral("--input-ipc-server=%1").arg(ipcName));
	}
	if (MpvDebugLogsEnabled()) {
		result.push_back(QStringLiteral("--log-file=%1").arg(
			MpvLogFilePath(token)));
	}
	if (MpvStreamingBoostEnabled()) {
		result.push_back(QStringLiteral("--cache=yes"));
		result.push_back(QStringLiteral("--demuxer=lavf"));
		result.push_back(QStringLiteral("--demuxer-seekable-cache=yes"));
		result.push_back(QStringLiteral("--demuxer-max-bytes=536870912"));
		result.push_back(QStringLiteral("--demuxer-max-back-bytes=134217728"));
		result.push_back(QStringLiteral("--demuxer-lavf-probe-info=nostreams"));
		result.push_back(QStringLiteral(
			"--demuxer-lavf-o=ignore_editlist=1,interleaved_read=0"));
	}
	result.push_back(source);
	return result;
}

[[nodiscard]] int LoaderPriorityForMpv() {
	return MpvStreamingBoostEnabled()
		? kBoostedMpvLoaderPriority
		: kMpvLoaderPriority;
}

[[nodiscard]] int ChunkSizeForMpvRequest(bool initial) {
	if (initial) {
		return kInitialReadChunkSize;
	}
	return MpvStreamingBoostEnabled()
		? kBoostedReadChunkSize
		: kReadChunkSize;
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

struct FillBufferResult {
	bool success = false;
	int waits = 0;
	crl::time waitDuration = 0;
	crl::time duration = 0;
	std::optional<Error> error;
};

struct Entry {
	Entry(
		not_null<DocumentData*> document,
		Data::FileOrigin origin,
		std::shared_ptr<Reader> reader,
		QString token,
		QString ipcName)
	: document(document)
	, origin(origin)
	, reader(std::move(reader))
	, token(std::move(token))
	, ipcName(std::move(ipcName))
	, size(this->reader ? this->reader->size() : 0) {
	}

	not_null<DocumentData*> document;
	Data::FileOrigin origin;
	std::shared_ptr<Reader> reader;
	QString token;
	QString ipcName;
	QString mime;
	QString fallbackPath;
	int64 size = 0;
	std::atomic<int> activeRequests = 0;
	std::atomic<crl::time> lastActivity = 0;
	std::atomic<bool> headerFinalized = false;
	std::atomic<int> requestsSeen = 0;
	std::atomic<int64> bytesSent = 0;
	std::atomic<bool> fallbackStarted = false;
	std::mutex fillMutex;
	std::mutex fallbackMutex;
	std::shared_ptr<rpl::lifetime> fallbackWatch;
};

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
	reader->setLoaderPriority(LoaderPriorityForMpv());
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

void StartSeekStormFallback(const std::shared_ptr<Entry> &entry) {
	if (entry->fallbackStarted.exchange(true)) {
		return;
	}
	const auto weak = std::weak_ptr<Entry>(entry);
	crl::on_main(&entry->document->session(), [weak] {
		const auto entry = weak.lock();
		if (!entry) {
			return;
		}
		if (const auto direct = TryResolveLocalLaunchPath(
				entry->document,
				entry->token); !direct.isEmpty()) {
			const auto switched = SendMpvLoadfileCommand(entry->ipcName, direct);
			MPV_STREAMING_LOG(
				("MPV Streaming: Immediate local fallback token=%1 path=%2 switched=%3.")
					.arg(entry->token)
					.arg(direct)
					.arg(switched ? 1 : 0));
			return;
		}
		const auto path = MpvFallbackFilePath(entry->document, entry->token);
		{
			const auto guard = std::lock_guard(entry->fallbackMutex);
			entry->fallbackPath = path;
			entry->fallbackWatch = std::make_shared<rpl::lifetime>();
		}
		QFile::remove(path);
		entry->document->session().data().documentLoadProgress(
		) | rpl::filter([document = entry->document](not_null<DocumentData*> current) {
			return (current == document);
		}) | rpl::on_next([weak](not_null<DocumentData*> current) {
			const auto entry = weak.lock();
			if (!entry) {
				return;
			}
			auto path = QString();
			auto watch = std::shared_ptr<rpl::lifetime>();
			{
				const auto guard = std::lock_guard(entry->fallbackMutex);
				path = entry->fallbackPath;
				watch = entry->fallbackWatch;
			}
			if (path.isEmpty()) {
				return;
			}
			if ((current->filepath(true) != path)
				&& (current->loadingFilePath() != path)) {
				return;
			}
			if (!current->loading() && QFileInfo(path).isFile()) {
				const auto switched = SendMpvLoadfileCommand(entry->ipcName, path);
				MPV_STREAMING_LOG(
					("MPV Streaming: Completed local fallback token=%1 path=%2 switched=%3.")
						.arg(entry->token)
						.arg(path)
						.arg(switched ? 1 : 0));
				if (watch) {
					watch->destroy();
				}
				const auto guard = std::lock_guard(entry->fallbackMutex);
				entry->fallbackWatch.reset();
				entry->fallbackPath.clear();
			}
		}, *entry->fallbackWatch);
		entry->document->save(
			entry->origin,
			path,
			LoadFromCloudOrLocal,
			true);
		MPV_STREAMING_LOG(
			("MPV Streaming: Started local fallback download token=%1 path=%2.")
				.arg(entry->token)
				.arg(path));
	});
}

[[nodiscard]] bool RecoverEntryReader(
		const std::shared_ptr<Entry> &entry,
		const QString &token,
		int64 offset) {
	const auto fresh = CreateDedicatedReaderFromWorker(
		entry->document,
		entry->origin);
	if (!fresh) {
		MPV_STREAMING_LOG(
			("MPV Streaming: Failed to recreate reader for token %1 at offset %2.")
				.arg(token)
				.arg(offset));
		return false;
	}
	const auto previous = std::move(entry->reader);
	entry->reader = fresh;
	entry->headerFinalized = false;
	if (previous) {
		previous->stopStreamingAsync();
		previous->tryRemoveLoaderAsync();
	}
	MPV_STREAMING_LOG(
		("MPV Streaming: Recreated reader for token %1 after LoadFailed at offset %2.")
			.arg(token)
			.arg(offset));
	return true;
}

[[nodiscard]] QString ResolveProgram() {
	auto configured = GetEnhancedString("mpv_path").trimmed();
	if (configured.startsWith('"')
		&& configured.endsWith('"')
		&& (configured.size() > 1)) {
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

[[nodiscard]] QProcessEnvironment LaunchEnvironment(const QString &token) {
	auto result = QProcessEnvironment::systemEnvironment();
	for (const auto &name : {
		QStringLiteral("HTTP_PROXY"),
		QStringLiteral("http_proxy"),
		QStringLiteral("HTTPS_PROXY"),
		QStringLiteral("https_proxy"),
		QStringLiteral("ALL_PROXY"),
		QStringLiteral("all_proxy"),
		QStringLiteral("MPV_HOME"),
	}) {
		result.remove(name);
	}
	const auto noProxy = QStringLiteral("127.0.0.1,localhost");
	result.insert(QStringLiteral("NO_PROXY"), noProxy);
	result.insert(QStringLiteral("no_proxy"), noProxy);
	const auto mpvHome = MpvSessionHomePath(token);
	QDir().mkpath(mpvHome);
	result.insert(QStringLiteral("MPV_HOME"), mpvHome);
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
		if (!ok || (from < 0)) {
			return {};
		} else if (from >= size) {
			return {
				.valid = true,
				.satisfiable = false,
			};
		}
		if (!tillPart.isEmpty()) {
			till = tillPart.toLongLong(&ok);
			if (!ok || (till < from)) {
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
		const auto amount = socket.write(
			data.constData() + written,
			data.size() - written);
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

[[nodiscard]] FillBufferResult FillBuffer(
		not_null<Reader*> reader,
		int64 offset,
		bytes::span buffer) {
	auto semaphore = crl::semaphore();
	auto result = FillBufferResult();
	const auto started = crl::now();
	while (true) {
		const auto state = reader->fill(offset, buffer, &semaphore);
		if (state == Reader::FillState::Success) {
			result.success = true;
			result.duration = (crl::now() - started);
			result.error = reader->streamingError();
			return result;
		} else if (state == Reader::FillState::Failed) {
			result.duration = (crl::now() - started);
			result.error = reader->streamingError();
			return result;
		}
		const auto waitStarted = crl::now();
		semaphore.acquire();
		result.waitDuration += (crl::now() - waitStarted);
		++result.waits;
		if (reader->streamingError()) {
			result.duration = (crl::now() - started);
			result.error = reader->streamingError();
			return result;
		}
	}
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
			const QString &token,
			const QString &ipcName,
			not_null<DocumentData*> document,
			Data::FileOrigin origin,
			std::shared_ptr<Reader> reader) {
		if (!ensureListening() || token.isEmpty()) {
			return {};
		}
		auto entry = std::make_shared<Entry>(
			document,
			origin,
			std::move(reader),
			token,
			ipcName);
		entry->mime = document->mimeString().isEmpty()
			? QStringLiteral("application/octet-stream")
			: document->mimeString();
		entry->lastActivity = crl::now();
		{
			const auto guard = std::lock_guard(_entriesMutex);
			if (!_entries.emplace(token, entry).second) {
				return {};
			}
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
			if ((i == end(_entries)) || (i->second->activeRequests.load() > 0)) {
				return;
			}
			removed = i->second;
			_entries.erase(i);
		}
		{
			const auto guard = std::lock_guard(removed->fallbackMutex);
			if (removed->fallbackWatch) {
				removed->fallbackWatch->destroy();
			}
		}
		removed->reader->stopStreaming(false);
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
		}
		i->second->activeRequests.fetch_add(1);
		i->second->lastActivity = crl::now();
		return i->second;
	}

	void release(const std::shared_ptr<Entry> &entry) {
		entry->lastActivity = crl::now();
		entry->activeRequests.fetch_sub(1);
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
				const auto &entry = i->second;
				if (!entry->activeRequests.load()
					&& ((now - entry->lastActivity.load()) >= kTokenLifetime)) {
					removed.push_back(entry);
					i = _entries.erase(i);
				} else {
					++i;
				}
			}
		}
		for (const auto &entry : removed) {
			{
				const auto guard = std::lock_guard(entry->fallbackMutex);
				if (entry->fallbackWatch) {
					entry->fallbackWatch->destroy();
				}
			}
			entry->reader->stopStreaming(false);
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
			MPV_STREAMING_LOG(
				("MPV Streaming: Failed to adopt socket descriptor %1.")
					.arg(qulonglong(descriptor)));
			return;
		}
		const auto headers = ReadHeaders(socket);
		const auto request = ParseRequest(headers);
		if (!request.valid) {
			MPV_STREAMING_LOG(
				("MPV Streaming: Invalid request, headers size %1.")
					.arg(headers.size()));
			(void)SendResponse(socket, "400 Bad Request", {
				{ "Connection", "close" },
				{ "Content-Length", "0" },
			});
			return;
		}
		MPV_STREAMING_LOG(
			("MPV Streaming: Request %1 token=%2 range='%3'.")
				.arg(QString::fromLatin1(request.method))
				.arg(request.token)
				.arg(QString::fromLatin1(request.rangeHeader)));
		const auto entry = lookupRetained(request.token);
		if (!entry) {
			MPV_STREAMING_LOG(
				("MPV Streaming: Token not found: %1.").arg(request.token));
			(void)SendResponse(socket, "404 Not Found", {
				{ "Connection", "close" },
				{ "Content-Length", "0" },
			});
			return;
		}
		const auto releaseGuard = gsl::finally([&] { release(entry); });
		const auto range = ParseRange(request.rangeHeader, entry->size);
		if (!range.valid) {
			MPV_STREAMING_LOG(
				("MPV Streaming: Invalid range '%1' for size %2.")
					.arg(QString::fromLatin1(request.rangeHeader))
					.arg(entry->size));
			(void)SendResponse(socket, "400 Bad Request", {
				{ "Connection", "close" },
				{ "Content-Length", "0" },
			});
			return;
		} else if (!range.satisfiable) {
			MPV_STREAMING_LOG(
				("MPV Streaming: Unsatisfiable range '%1' for size %2.")
					.arg(QString::fromLatin1(request.rangeHeader))
					.arg(entry->size));
			(void)SendResponse(socket, "416 Range Not Satisfiable", {
				{ "Accept-Ranges", "bytes" },
				{ "Connection", "close" },
				{ "Content-Length", "0" },
				{ "Content-Range",
					QByteArray("bytes */") + QByteArray::number(entry->size) },
			});
			return;
		}

		const auto requestIndex = entry->requestsSeen.fetch_add(1) + 1;
		const auto requestStarted = crl::now();
		auto requestBytesSent = int64(0);
		auto requestFillWaits = 0;
		auto requestFillWaitMs = crl::time(0);
		auto requestFillMs = crl::time(0);
		auto requestWriteMs = crl::time(0);
		auto outcome = QStringLiteral("completed");
		const auto logSummary = gsl::finally([&] {
			entry->bytesSent.fetch_add(requestBytesSent);
			const auto requestDuration = (crl::now() - requestStarted);
			if (MpvDebugLogsEnabled()
				&& ((requestDuration >= kSlowRequestThreshold)
					|| requestFillWaits
					|| (outcome != QStringLiteral("completed")))) {
				MPV_STREAMING_LOG(
					("MPV Streaming: Request summary token=%1 id=%2 from=%3 length=%4 sent=%5 waits=%6 waitMs=%7 fillMs=%8 writeMs=%9 totalMs=%10 outcome=%11.")
						.arg(request.token)
						.arg(requestIndex)
						.arg(range.range.from)
						.arg(range.range.length)
						.arg(requestBytesSent)
						.arg(requestFillWaits)
						.arg(int(requestFillWaitMs))
						.arg(int(requestFillMs))
						.arg(int(requestWriteMs))
						.arg(int(requestDuration))
						.arg(outcome));
			}
		});

		if ((request.method == "GET")
			&& range.range.partial
			&& (requestIndex >= kSeekStormRequestThreshold)
			&& (range.range.from >= kSeekStormOffsetThreshold)) {
			StartSeekStormFallback(entry);
		}

		const auto status = range.range.partial
			? "206 Partial Content"
			: "200 OK";
		const auto contentLength = QByteArray::number(range.range.length);
		if (range.range.partial) {
			const auto contentRange = QByteArray("bytes ")
				+ QByteArray::number(range.range.from)
				+ '-'
				+ QByteArray::number(range.range.till)
				+ '/'
				+ QByteArray::number(entry->size);
			if (!SendResponse(socket, status, {
				{ "Accept-Ranges", "bytes" },
				{ "Connection", "close" },
				{ "Content-Length", contentLength },
				{ "Content-Type", entry->mime.toUtf8() },
				{ "Content-Range", contentRange },
			})) {
				outcome = QStringLiteral("header_failed");
				return;
			}
		} else if (!SendResponse(socket, status, {
			{ "Accept-Ranges", "bytes" },
			{ "Connection", "close" },
			{ "Content-Length", contentLength },
			{ "Content-Type", entry->mime.toUtf8() },
		})) {
			outcome = QStringLiteral("header_failed");
			return;
		}
		if (request.method == "HEAD") {
			return;
		}

		auto offset = range.range.from;
		auto left = range.range.length;
		const auto startedFromZero = (offset == 0);
		auto retriedLoadFailure = false;
		while (left > 0) {
			const auto chunkSize = ChunkSizeForMpvRequest(
				offset == range.range.from);
			const auto size = int(std::min(left, int64(chunkSize)));
			auto buffer = QByteArray(size, Qt::Uninitialized);
			{
				const auto lock = std::unique_lock(entry->fillMutex);
				const auto fill = FillBuffer(
					entry->reader.get(),
					offset,
					bytes::span(
						reinterpret_cast<bytes::type*>(buffer.data()),
						size));
				requestFillWaits += fill.waits;
				requestFillWaitMs += fill.waitDuration;
				requestFillMs += fill.duration;
				if (fill.waits || (fill.duration >= kSlowFillThreshold)) {
					MPV_STREAMING_LOG(
						("MPV Streaming: Fill summary token=%1 offset=%2 size=%3 waits=%4 waitMs=%5 totalMs=%6 error=%7.")
							.arg(request.token)
							.arg(offset)
							.arg(size)
							.arg(fill.waits)
							.arg(int(fill.waitDuration))
							.arg(int(fill.duration))
							.arg(StreamingErrorDebugString(fill.error)));
				}
				if (!fill.success) {
					const auto error = fill.error;
					if (!retriedLoadFailure
						&& error
						&& (*error == Error::LoadFailed)
						&& RecoverEntryReader(entry, request.token, offset)) {
						retriedLoadFailure = true;
						continue;
					}
					outcome = QStringLiteral("fill_failed");
					MPV_STREAMING_LOG(
						("MPV Streaming: FillBuffer failed at offset %1, size %2, error=%3.")
							.arg(offset)
							.arg(size)
							.arg(StreamingErrorDebugString(error)));
					return;
				}
				if (startedFromZero
					&& !entry->headerFinalized.exchange(true)) {
					entry->reader->headerDone();
				}
				retriedLoadFailure = false;
			}

			const auto writeStarted = crl::now();
			if (!WriteAll(socket, buffer.constData(), buffer.size())) {
				const auto error = socket.error();
				outcome = QStringLiteral("write_failed");
				if (error != QAbstractSocket::RemoteHostClosedError) {
					MPV_STREAMING_LOG(
						("MPV Streaming: WriteAll failed at offset %1, size %2, error=%3, detail='%4'.")
							.arg(offset)
							.arg(size)
							.arg(int(error))
							.arg(socket.errorString()));
				}
				return;
			}
			requestWriteMs += (crl::now() - writeStarted);
			requestBytesSent += buffer.size();
			offset += size;
			left -= size;
			entry->lastActivity = crl::now();
		}
	}

	std::mutex _entriesMutex;
	std::map<QString, std::shared_ptr<Entry>> _entries;
	DescriptorServer _server;
	base::Timer _cleanupTimer;
};

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
		&& (document->size > 0)
		&& document->useStreamingLoader()
		&& (document->isVideoFile() || document->isVideoMessage());
#endif
}

OpenResult OpenVideoMessageInMpv(HistoryItem *item, DocumentData *document) {
	if (!CanOpenVideoMessageInMpv(item, document)) {
		return OpenResult::Unsupported;
	}
	const auto program = ResolveProgram();
	if (program.isEmpty()) {
		MPV_STREAMING_LOG(("MPV Streaming: Player not found."));
		return OpenResult::PlayerNotFound;
	}

	const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
	if (const auto directPath = TryResolveLocalLaunchPath(
			document,
			token); !directPath.isEmpty()) {
		MPV_STREAMING_LOG(
			("MPV Streaming: Launching '%1' with local path %2.")
				.arg(program)
				.arg(directPath));
		if (MpvDebugLogsEnabled()) {
			MPV_STREAMING_LOG(
				("MPV Streaming: MPV log file: %1.")
					.arg(MpvLogFilePath(token)));
		}
		const auto arguments = LaunchArguments(
			directPath,
			token,
			QString());
		MPV_STREAMING_LOG(
			("MPV Streaming: Launch arguments: %1.")
				.arg(arguments.join(QStringLiteral(" "))));
		auto process = QProcess();
		process.setProgram(program);
		process.setArguments(arguments);
		process.setWorkingDirectory(QFileInfo(program).absolutePath());
		process.setProcessEnvironment(LaunchEnvironment(token));
		if (!process.startDetached()) {
			MPV_STREAMING_LOG(
				("MPV Streaming: Failed to start player '%1'.").arg(program));
			return OpenResult::Failed;
		}
		return OpenResult::Success;
	}

	const auto origin = Data::FileOrigin(item->fullId());
	const auto reader = CreateDedicatedReader(document, origin);
	if (!reader) {
		MPV_STREAMING_LOG(
			("MPV Streaming: Failed to create dedicated reader for document %1.")
				.arg(qulonglong(document->id)));
		return OpenResult::Failed;
	}
	const auto ipcName = MpvIpcServerName(token);
	const auto launch = Server::instance().add(
		token,
		ipcName,
		document,
		origin,
		reader);
	if (launch.url.isEmpty()) {
		MPV_STREAMING_LOG(
			("MPV Streaming: Failed to create launch URL for document %1.")
				.arg(qulonglong(document->id)));
		reader->stopStreaming(false);
		return OpenResult::Failed;
	}
	MPV_STREAMING_LOG(
		("MPV Streaming: Launching '%1' with URL %2.")
			.arg(program)
			.arg(launch.url));
	if (MpvDebugLogsEnabled()) {
		MPV_STREAMING_LOG(
			("MPV Streaming: MPV log file: %1.")
				.arg(MpvLogFilePath(launch.token)));
	}
	const auto arguments = LaunchArguments(
		launch.url,
		launch.token,
		ipcName);
	MPV_STREAMING_LOG(
		("MPV Streaming: Launch arguments: %1.")
			.arg(arguments.join(QStringLiteral(" "))));
	auto process = QProcess();
	process.setProgram(program);
	process.setArguments(arguments);
	process.setWorkingDirectory(QFileInfo(program).absolutePath());
	process.setProcessEnvironment(LaunchEnvironment(token));
	if (!process.startDetached()) {
		MPV_STREAMING_LOG(
			("MPV Streaming: Failed to start player '%1'.").arg(program));
		Server::instance().remove(launch.token);
		return OpenResult::Failed;
	}
	return OpenResult::Success;
}

#undef MPV_STREAMING_LOG

} // namespace Media::Streaming::Mpv
