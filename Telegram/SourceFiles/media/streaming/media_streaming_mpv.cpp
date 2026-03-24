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
#include "data/data_file_origin.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "media/streaming/media_streaming_reader.h"
#include "settings.h"

#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QUuid>
#include <QtCore/QUrl>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace Media::Streaming::Mpv {
namespace {

constexpr auto kPathPrefix = "/mpv/";
constexpr auto kHeadersLimit = 64 * 1024;
constexpr auto kReadChunkSize = 256 * 1024;
constexpr auto kCleanupInterval = 60 * crl::time(1000);
constexpr auto kTokenLifetime = 5 * 60 * crl::time(1000);

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
	std::shared_ptr<Reader> reader;
	QString mime;
	int64 size = 0;
	std::atomic<int> activeRequests = 0;
	std::atomic<crl::time> lastActivity = 0;
	std::mutex fillMutex;
};

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
		.token = path.mid(int(sizeof(kPathPrefix) - 1)),
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

class Server final {
public:
	struct Launch {
		QString token;
		QString url;
	};

	Server()
	: _cleanupTimer([=] { cleanup(); }) {
		QObject::connect(
			&_server,
			&QTcpServer::newConnection,
			[this] { handleNewConnections(); });
	}

	[[nodiscard]] Launch add(
			not_null<DocumentData*> document,
			std::shared_ptr<Reader> reader) {
		if (!ensureListening()) {
			return {};
		}
		auto entry = std::make_shared<Entry>();
		entry->reader = std::move(reader);
		entry->mime = document->mimeString().isEmpty()
			? QStringLiteral("application/octet-stream")
			: document->mimeString();
		entry->size = document->size;
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
			if (i == end(_entries) || i->second->activeRequests.load() > 0) {
				return;
			}
			removed = i->second;
			_entries.erase(i);
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
		}
		const auto guard = std::lock_guard(_entriesMutex);
		if (!_entries.empty()) {
			_cleanupTimer.callOnce(kCleanupInterval);
		}
	}

	void handleNewConnections() {
		while (const auto socket = _server.nextPendingConnection()) {
			const auto descriptor = socket->socketDescriptor();
			socket->deleteLater();
			if (descriptor < 0) {
				continue;
			}
			std::thread([this, descriptor] {
				handleConnection(descriptor);
			}).detach();
		}
	}

	void handleConnection(qintptr descriptor) {
		auto socket = QTcpSocket();
		if (!socket.setSocketDescriptor(descriptor)) {
			return;
		}
		const auto headers = ReadHeaders(socket);
		const auto request = ParseRequest(headers);
		if (!request.valid) {
			(void)SendResponse(socket, "400 Bad Request", {
				{ "Connection", "close" },
				{ "Content-Length", "0" },
			});
			return;
		}
		const auto entry = lookupRetained(request.token);
		if (!entry) {
			(void)SendResponse(socket, "404 Not Found", {
				{ "Connection", "close" },
				{ "Content-Length", "0" },
			});
			return;
		}
		const auto releaseGuard = gsl::finally([&] { release(entry); });
		const auto range = ParseRange(request.rangeHeader, entry->size);
		if (!range.valid) {
			(void)SendResponse(socket, "400 Bad Request", {
				{ "Connection", "close" },
				{ "Content-Length", "0" },
			});
			return;
		} else if (!range.satisfiable) {
			(void)SendResponse(socket, "416 Range Not Satisfiable", {
				{ "Accept-Ranges", "bytes" },
				{ "Connection", "close" },
				{ "Content-Length", "0" },
				{ "Content-Range", QByteArray("bytes */") + QByteArray::number(entry->size) },
			});
			return;
		}
		const auto status = range.range.partial ? "206 Partial Content" : "200 OK";
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
				return;
			}
		} else if (!SendResponse(socket, status, {
			{ "Accept-Ranges", "bytes" },
			{ "Connection", "close" },
			{ "Content-Length", contentLength },
			{ "Content-Type", entry->mime.toUtf8() },
		})) {
			return;
		}
		if (request.method == "HEAD") {
			return;
		}
		const auto lock = std::unique_lock(entry->fillMutex);
		auto offset = range.range.from;
		auto left = range.range.length;
		while (left > 0) {
			const auto size = int(std::min(left, int64(kReadChunkSize)));
			auto buffer = QByteArray(size, Qt::Uninitialized);
			if (!FillBuffer(
					entry->reader.get(),
					offset,
					bytes::span(
						reinterpret_cast<bytes::type*>(buffer.data()),
						size))) {
				return;
			} else if (!WriteAll(socket, buffer.constData(), buffer.size())) {
				return;
			}
			offset += size;
			left -= size;
			entry->lastActivity = crl::now();
		}
	}

	std::mutex _entriesMutex;
		std::map<QString, std::shared_ptr<Entry>> _entries;
	QTcpServer _server;
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
	return item->allowsForward()
		&& media
		&& (media->ttlSeconds() <= 0)
		&& document->size > 0
		&& document->supportsStreaming()
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
		return OpenResult::PlayerNotFound;
	}
	auto loader = document->createStreamingLoader(
		Data::FileOrigin(item->fullId()),
		false);
	if (!loader) {
		return OpenResult::Failed;
	}
	auto reader = std::make_shared<Reader>(
		std::move(loader),
		&document->owner().cacheBigFile());
	reader->startStreaming();
	const auto launch = Server::instance().add(document, reader);
	if (launch.url.isEmpty()) {
		reader->stopStreaming(false);
		return OpenResult::Failed;
	}
	if (!QProcess::startDetached(program, { launch.url })) {
		Server::instance().remove(launch.token);
		return OpenResult::Failed;
	}
	return OpenResult::Success;
}

} // namespace Media::Streaming::Mpv
