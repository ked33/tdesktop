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
#include "data/data_streaming.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "main/main_session.h"
#include "media/streaming/media_streaming_reader.h"
#include "logs.h"
#include "settings.h"

#include <QtCore/QFileInfo>
#include <QtCore/QCoreApplication>
#include <QtCore/QLibrary>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QStringList>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtCore/QUrl>
#include <QtGui/QCloseEvent>
#include <QtEndian>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <atomic>
#include <cstdint>
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
constexpr auto kMp4ProbeInitialSize = 256 * 1024;
constexpr auto kMp4ProbeMaxSize = 4 * 1024 * 1024;
constexpr auto kCompatibilitySeekBootstrapBytes = 4 * 1024 * 1024;
constexpr auto kCompatibilityLateSeekMinOffset = 128 * 1024 * 1024;
constexpr auto kCompatibilitySeekWaitTimeout = 4000;
constexpr auto kCompatibilitySeekWaitStep = 10;
constexpr auto kCleanupInterval = 60 * crl::time(1000);
constexpr auto kTokenLifetime = 5 * 60 * crl::time(1000);
constexpr auto kMpvLoaderPriority = 2;
constexpr auto kLibMpvReadLogStep = int64(32) * 1024 * 1024;

[[nodiscard]] QString ResolveLibMpvLibraryPath(const QString &programPath) {
	const auto dir = QFileInfo(programPath).absolutePath();
	for (const auto &name : {
		QStringLiteral("libmpv-2.dll"),
		QStringLiteral("mpv-2.dll"),
		QStringLiteral("libmpv.dll"),
		QStringLiteral("mpv.dll"),
	}) {
		const auto path = dir + '/' + name;
		if (QFileInfo(path).isFile()) {
			return path;
		}
	}
	return {};
}

[[nodiscard]] bool MpvDebugLogsEnabled() {
	return GetEnhancedBool("mpv_streaming_debug_logs");
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
	return std::clamp(boost, 0, 5);
}

[[nodiscard]] bool MpvStreamingBoostEnabled() {
	return (DownloadBoostLevel() > 0);
}

[[nodiscard]] QStringList LaunchArguments(const QString &url) {
	auto result = QStringList{
		QStringLiteral("--force-window=immediate"),
		QStringLiteral("--demuxer-lavf-o=ignore_editlist=1"),
	};
	if (MpvStreamingBoostEnabled()) {
		result.push_back(QStringLiteral("--cache=yes"));
		result.push_back(QStringLiteral("--demuxer-seekable-cache=yes"));
		result.push_back(QStringLiteral("--demuxer-max-bytes=536870912"));
		result.push_back(QStringLiteral("--demuxer-max-back-bytes=134217728"));
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
		std::shared_ptr<Reader> reader,
		bool preferCompatibilityForLargeFrontMoov)
	: document(document)
	, origin(origin)
	, reader(std::move(reader))
	, preferCompatibilityForLargeFrontMoov(preferCompatibilityForLargeFrontMoov)
	, size(this->reader ? this->reader->size() : 0) {
	}

	not_null<DocumentData*> document;
	Data::FileOrigin origin;
	std::shared_ptr<Reader> reader;
	std::shared_ptr<Reader> seekReader;
	QString mime;
	int64 size = 0;
	std::atomic<int> activeRequests = 0;
	std::atomic<crl::time> lastActivity = 0;
	std::atomic<bool> headerFinalized = false;
	std::atomic<int> mp4Layout = 0;
	std::atomic<std::uint64_t> latestSeekGeneration = 0;
	std::atomic<int64> compatibilityBootstrapBytes = 0;
	std::atomic<bool> compatibilityLateSeekReady = false;
	bool preferCompatibilityForLargeFrontMoov = false;
	std::mutex fillMutex;
	std::mutex seekFillMutex;
};

enum class Mp4Layout {
	Unknown = 0,
	Fragmented = 1,
	Regular = 2,
	LargeFrontMoov = 3,
};

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

enum mpv_error : int {
	MPV_ERROR_SUCCESS = 0,
	MPV_ERROR_LOADING_FAILED = -13,
	MPV_ERROR_UNSUPPORTED = -18,
	MPV_ERROR_GENERIC = -20,
};

enum mpv_event_id : int {
	MPV_EVENT_NONE = 0,
	MPV_EVENT_SHUTDOWN = 1,
	MPV_EVENT_LOG_MESSAGE = 2,
	MPV_EVENT_END_FILE = 7,
	MPV_EVENT_FILE_LOADED = 8,
	MPV_EVENT_SEEK = 20,
	MPV_EVENT_PLAYBACK_RESTART = 21,
	MPV_EVENT_PROPERTY_CHANGE = 22,
};

enum mpv_format : int {
	MPV_FORMAT_NONE = 0,
	MPV_FORMAT_STRING = 1,
};

struct mpv_handle;

struct mpv_event_end_file {
	int reason = 0;
	int error = 0;
	int64_t playlist_entry_id = 0;
	int64_t playlist_insert_id = 0;
	int playlist_insert_num_entries = 0;
};

struct mpv_event_log_message {
	const char *prefix = nullptr;
	const char *level = nullptr;
	const char *text = nullptr;
	int log_level = 0;
};

struct mpv_event_property {
	const char *name = nullptr;
	mpv_format format = MPV_FORMAT_NONE;
	void *data = nullptr;
};

struct mpv_event {
	mpv_event_id event_id = MPV_EVENT_NONE;
	int error = 0;
	uint64_t reply_userdata = 0;
	void *data = nullptr;
};

using mpv_stream_cb_read_fn = int64_t(*)(void *cookie, char *buf, uint64_t nbytes);
using mpv_stream_cb_seek_fn = int64_t(*)(void *cookie, int64_t offset);
using mpv_stream_cb_size_fn = int64_t(*)(void *cookie);
using mpv_stream_cb_close_fn = void(*)(void *cookie);
using mpv_stream_cb_cancel_fn = void(*)(void *cookie);

struct mpv_stream_cb_info {
	void *cookie = nullptr;
	mpv_stream_cb_read_fn read_fn = nullptr;
	mpv_stream_cb_seek_fn seek_fn = nullptr;
	mpv_stream_cb_size_fn size_fn = nullptr;
	mpv_stream_cb_close_fn close_fn = nullptr;
	mpv_stream_cb_cancel_fn cancel_fn = nullptr;
};

using mpv_stream_cb_open_ro_fn = int(*)(void *user_data, char *uri, mpv_stream_cb_info *info);

class LibMpvWindow final : public QWidget {
public:
	explicit LibMpvWindow(std::function<void()> closeRequested)
	: _closeRequested(std::move(closeRequested)) {
		setAttribute(Qt::WA_NativeWindow);
		setAttribute(Qt::WA_QuitOnClose, false);
		setWindowFlag(Qt::Window, true);
		resize(960, 540);
	}

	void markClosingFromBackend() {
		_closingFromBackend = true;
	}

protected:
	void closeEvent(QCloseEvent *e) override {
		if (!_closingFromBackend && _closeRequested) {
			_closeRequested();
			e->ignore();
			return;
		}
		QWidget::closeEvent(e);
	}

private:
	std::function<void()> _closeRequested;
	bool _closingFromBackend = false;
};

class LibMpvApi final {
public:
	using ClientApiVersion = unsigned long(*)();
	using Create = mpv_handle*(*)();
	using Destroy = void(*)(mpv_handle*);
	using Initialize = int(*)(mpv_handle*);
	using SetOptionString = int(*)(mpv_handle*, const char*, const char*);
	using Command = int(*)(mpv_handle*, const char**);
	using WaitEvent = mpv_event*(*)(mpv_handle*, double);
	using TerminateDestroy = void(*)(mpv_handle*);
	using ErrorString = const char*(*)(int);
	using RequestLogMessages = int(*)(mpv_handle*, const char*);
	using ObserveProperty = int(*)(mpv_handle*, uint64_t, const char*, mpv_format);
	using GetPropertyString = char*(*)(mpv_handle*, const char*);
	using Free = void(*)(void*);
	using StreamCbAddRo = int(*)(mpv_handle*, const char*, void*, mpv_stream_cb_open_ro_fn);

	[[nodiscard]] bool load(const QString &programPath) {
		_library.setFileName(ResolveLibMpvLibraryPath(programPath));
		if (_library.fileName().isEmpty()) {
			return false;
		} else if (!_library.load()) {
			return false;
		}
		clientApiVersion = resolve<ClientApiVersion>("mpv_client_api_version");
		create = resolve<Create>("mpv_create");
		destroy = resolve<Destroy>("mpv_destroy");
		initialize = resolve<Initialize>("mpv_initialize");
		setOptionString = resolve<SetOptionString>("mpv_set_option_string");
		command = resolve<Command>("mpv_command");
		waitEvent = resolve<WaitEvent>("mpv_wait_event");
		terminateDestroy = resolve<TerminateDestroy>("mpv_terminate_destroy");
		errorString = resolve<ErrorString>("mpv_error_string");
		requestLogMessages = resolve<RequestLogMessages>("mpv_request_log_messages");
		observeProperty = resolve<ObserveProperty>("mpv_observe_property");
		getPropertyString = resolve<GetPropertyString>("mpv_get_property_string");
		free = resolve<Free>("mpv_free");
		streamCbAddRo = resolve<StreamCbAddRo>("mpv_stream_cb_add_ro");
		return clientApiVersion
			&& create
			&& destroy
			&& initialize
			&& setOptionString
			&& command
			&& waitEvent
			&& terminateDestroy
			&& errorString
			&& requestLogMessages
			&& observeProperty
			&& getPropertyString
			&& free
			&& streamCbAddRo;
	}

	[[nodiscard]] QString describeError(int error) const {
		const auto value = errorString ? errorString(error) : nullptr;
		return value ? QString::fromUtf8(value) : QString::number(error);
	}

private:
	template <typename T>
	[[nodiscard]] T resolve(const char *name) {
		return reinterpret_cast<T>(_library.resolve(name));
	}

	QLibrary _library;

public:
	ClientApiVersion clientApiVersion = nullptr;
	Create create = nullptr;
	Destroy destroy = nullptr;
	Initialize initialize = nullptr;
	SetOptionString setOptionString = nullptr;
	Command command = nullptr;
	WaitEvent waitEvent = nullptr;
	TerminateDestroy terminateDestroy = nullptr;
	ErrorString errorString = nullptr;
	RequestLogMessages requestLogMessages = nullptr;
	ObserveProperty observeProperty = nullptr;
	GetPropertyString getPropertyString = nullptr;
	Free free = nullptr;
	StreamCbAddRo streamCbAddRo = nullptr;
};

class LibMpvStreamSession final : public std::enable_shared_from_this<LibMpvStreamSession> {
public:
	LibMpvStreamSession(
		not_null<DocumentData*> document,
		Data::FileOrigin origin,
		std::shared_ptr<Reader> reader)
	: _document(document)
	, _origin(origin)
	, _reader(std::move(reader))
	, _size(_reader ? _reader->size() : 0) {
		MPV_STREAMING_LOG(("MPV libmpv: Created stream session for document %1 size=%2.")
			.arg(qulonglong(_document->id))
			.arg(_size));
	}

	[[nodiscard]] int64 size() const {
		return _size;
	}

	[[nodiscard]] int64 read(char *buffer, uint64_t nbytes) {
		if (!_reader || !buffer) {
			return -1;
		}
		const auto limit = std::min<int64>(_size - _offset, int64(nbytes));
		if (limit <= 0) {
			return 0;
		}
		auto retriedLoadFailure = false;
		while (true) {
			if (cancelled()) {
				return -1;
			}
			auto lock = std::unique_lock(_mutex);
			const auto offset = _offset;
			lock.unlock();
			if (FillBuffer(
					_reader.get(),
					offset,
					bytes::span(
						reinterpret_cast<bytes::type*>(buffer),
						int(limit)))) {
				lock.lock();
				_offset += limit;
				lock.unlock();
				if (!_headerFinalized.exchange(true)) {
					_reader->headerDone();
				}
				const auto total = _deliveredBytes.fetch_add(limit) + limit;
				if (!_firstReadLogged.exchange(true)) {
					MPV_STREAMING_LOG(("MPV libmpv: First read for document %1 offset=%2 bytes=%3 total=%4.")
						.arg(qulonglong(_document->id))
						.arg(offset)
						.arg(limit)
						.arg(total));
				}
				auto next = _nextReadLogAt.load();
				while (total >= next) {
					if (_nextReadLogAt.compare_exchange_weak(
							next,
							next + kLibMpvReadLogStep)) {
						MPV_STREAMING_LOG(("MPV libmpv: Read progress for document %1 offset=%2 bytes=%3 total=%4.")
							.arg(qulonglong(_document->id))
							.arg(offset)
							.arg(limit)
							.arg(total));
						break;
					}
				}
				return limit;
			}
			const auto error = _reader->streamingError();
			if (!retriedLoadFailure
				&& error
				&& (*error == Error::LoadFailed)
				&& recreateReader(offset)) {
				MPV_STREAMING_LOG(("MPV libmpv: Recreated reader after LoadFailed for document %1 at offset %2.")
					.arg(qulonglong(_document->id))
					.arg(offset));
				retriedLoadFailure = true;
				continue;
			}
			MPV_STREAMING_LOG(("MPV libmpv: Read failed for document %1 offset=%2 bytes=%3 retried=%4.")
				.arg(qulonglong(_document->id))
				.arg(offset)
				.arg(limit)
				.arg(retriedLoadFailure ? 1 : 0));
			return -1;
		}
	}

	[[nodiscard]] int64 seek(int64 offset) {
		if (!_reader) {
			return MPV_ERROR_GENERIC;
		} else if (offset < 0 || offset > _size) {
			return MPV_ERROR_GENERIC;
		}
		resetCancelled();
		auto lock = std::unique_lock(_mutex);
		_offset = offset;
		MPV_STREAMING_LOG(("MPV libmpv: Seek for document %1 -> %2.")
			.arg(qulonglong(_document->id))
			.arg(_offset));
		return _offset;
	}

	[[nodiscard]] int64 streamSize() const {
		return _size;
	}

	void cancel() {
		_cancelled = true;
		MPV_STREAMING_LOG(("MPV libmpv: Cancel requested for document %1.")
			.arg(qulonglong(_document->id)));
	}

	void close() {
		MPV_STREAMING_LOG(("MPV libmpv: Closing stream session for document %1 delivered=%2.")
			.arg(qulonglong(_document->id))
			.arg(_deliveredBytes.load()));
		if (_reader) {
			_reader->stopStreamingAsync();
			_reader->tryRemoveLoaderAsync();
			_reader.reset();
		}
	}

private:
	[[nodiscard]] bool cancelled() const {
		return _cancelled.load();
	}

	void resetCancelled() {
		_cancelled = false;
	}

	[[nodiscard]] bool recreateReader(int64 offset) {
		const auto fresh = CreateDedicatedReaderFromWorker(_document, _origin);
		if (!fresh) {
			return false;
		}
		auto lock = std::unique_lock(_mutex);
		if (_reader) {
			_reader->stopStreamingAsync();
			_reader->tryRemoveLoaderAsync();
		}
		_reader = fresh;
		_headerFinalized = false;
		_offset = offset;
		return true;
	}

	const not_null<DocumentData*> _document;
	const Data::FileOrigin _origin;
	std::shared_ptr<Reader> _reader;
	const int64 _size = 0;
	std::atomic<bool> _cancelled = false;
	std::atomic<bool> _headerFinalized = false;
	std::atomic<bool> _firstReadLogged = false;
	std::atomic<int64> _deliveredBytes = 0;
	std::atomic<int64> _nextReadLogAt = kLibMpvReadLogStep;
	std::mutex _mutex;
	int64 _offset = 0;
};

struct LibMpvStreamCookie {
	std::shared_ptr<LibMpvStreamSession> session;
};

class LibMpvControlledPlayer final : public std::enable_shared_from_this<LibMpvControlledPlayer> {
public:
	LibMpvControlledPlayer(
		not_null<DocumentData*> document,
		std::shared_ptr<LibMpvStreamSession> session,
		QString programPath)
	: _document(document)
	, _session(std::move(session))
	, _programPath(std::move(programPath))
	, _protocol(QStringLiteral("tdmpv") + QUuid::createUuid().toString(QUuid::Id128)) {
	}

	[[nodiscard]] bool start() {
		if (!_api.load(_programPath)) {
			MPV_STREAMING_LOG(("MPV Streaming: Failed to load libmpv from '%1'.")
				.arg(_programPath));
			return false;
		}
		_handle = _api.create ? _api.create() : nullptr;
		if (!_handle) {
			MPV_STREAMING_LOG(("MPV Streaming: mpv_create() failed for '%1'.")
				.arg(_programPath));
			return false;
		}
		const auto self = shared_from_this();
		_window = new LibMpvWindow([self] {
			self->requestClose();
		});
		_window->setWindowTitle(_document->filename().isEmpty()
			? QStringLiteral("mpv")
			: _document->filename());
		_window->createWinId();
		const auto wid = QString::number(quintptr(_window->winId()));
		if (!setOption("wid", wid)
			|| !setOption("force-window", "immediate")
			|| !setOption("title", _window->windowTitle())
			|| !setOption("demuxer-lavf-o", "ignore_editlist=1")
			|| !setOption("osc", "yes")
			|| !setOption("input-cursor", "yes")
			|| !setOption("input-default-bindings", "yes")
			|| !setOption("input-vo-keyboard", "yes")
			|| !setOption("vo", "gpu-next")
			|| !setOption("gpu-context", "d3d11")
			|| !setOption("hwdec", "auto-safe")
			|| !setOption("cache", "yes")
			|| !setOption("stream-buffer-size", "4194304")) {
			return false;
		}
		MPV_STREAMING_LOG(("MPV libmpv: Runtime options for document %1 osc=yes input-cursor=yes input-default-bindings=yes input-vo-keyboard=yes vo=gpu-next gpu-context=d3d11 hwdec=auto-safe cache=yes stream-buffer-size=4194304.")
			.arg(qulonglong(_document->id)));
		if (_api.streamCbAddRo(
				_handle,
				_protocol.toUtf8().constData(),
				this,
				&LibMpvControlledPlayer::openStream) < 0) {
			MPV_STREAMING_LOG(("MPV Streaming: mpv_stream_cb_add_ro() failed for protocol %1.")
				.arg(_protocol));
			return false;
		}
		if (_api.initialize(_handle) < 0) {
			MPV_STREAMING_LOG(("MPV Streaming: mpv_initialize() failed for protocol %1.")
				.arg(_protocol));
			return false;
		}
		_initialized = true;
		_api.requestLogMessages(_handle, MpvDebugLogsEnabled() ? "debug" : "warn");
		observeDebugProperties();
		const auto uri = (_protocol + QStringLiteral("://stream")).toUtf8();
		const char *command[] = { "loadfile", uri.constData(), nullptr };
		if (_api.command(_handle, command) < 0) {
			MPV_STREAMING_LOG(("MPV Streaming: mpv loadfile failed for protocol %1.")
				.arg(_protocol));
			return false;
		}
		MPV_STREAMING_LOG(("MPV libmpv: Started controlled player for document %1 protocol=%2.")
			.arg(qulonglong(_document->id))
			.arg(_protocol));
		_window->show();
		_eventThread = std::thread([self] {
			self->eventLoop();
		});
		return true;
	}

	~LibMpvControlledPlayer() {
		requestClose();
		if (_eventThread.joinable()) {
			_eventThread.join();
		}
		if (_handle) {
			if (_initialized) {
				_api.terminateDestroy(_handle);
			} else {
				_api.destroy(_handle);
			}
			_handle = nullptr;
		}
		if (_window) {
			_window->markClosingFromBackend();
			_window->close();
			_window->deleteLater();
			_window = nullptr;
		}
		if (_session) {
			_session->close();
		}
	}

	static void keepAlive(const std::shared_ptr<LibMpvControlledPlayer> &player) {
		const auto guard = std::lock_guard(_registryMutex);
		_registry.push_back(player);
	}

	static void release(const std::shared_ptr<LibMpvControlledPlayer> &player) {
		const auto guard = std::lock_guard(_registryMutex);
		_registry.erase(
			std::remove(_registry.begin(), _registry.end(), player),
			_registry.end());
	}

private:
	[[nodiscard]] QString propertyValue(const char *name) const {
		if (!_handle || !_api.getPropertyString || !_api.free || !name) {
			return QStringLiteral("<unavailable>");
		}
		auto *raw = _api.getPropertyString(_handle, name);
		if (!raw) {
			return QStringLiteral("<unavailable>");
		}
		const auto result = QString::fromUtf8(raw);
		_api.free(raw);
		return result;
	}

	void logPropertySnapshot(const char *reason) const {
		MPV_STREAMING_LOG(("MPV libmpv state: document=%1 reason=%2 file-format=%3 video-codec=%4 audio-codec=%5 video-format=%6 hwdec-current=%7 seekable=%8 paused-for-cache=%9 cache-buffering-state=%10 cache-speed=%11 width=%12 height=%13.")
			.arg(qulonglong(_document->id))
			.arg(QString::fromLatin1(reason ? reason : "unknown"))
			.arg(propertyValue("file-format"))
			.arg(propertyValue("video-codec"))
			.arg(propertyValue("audio-codec-name"))
			.arg(propertyValue("video-format"))
			.arg(propertyValue("hwdec-current"))
			.arg(propertyValue("seekable"))
			.arg(propertyValue("paused-for-cache"))
			.arg(propertyValue("cache-buffering-state"))
			.arg(propertyValue("cache-speed"))
			.arg(propertyValue("width"))
			.arg(propertyValue("height")));
	}

	void observeProperty(const char *name, uint64_t id) {
		if (!_api.observeProperty || !_handle || !name) {
			return;
		}
		const auto result = _api.observeProperty(
			_handle,
			id,
			name,
			MPV_FORMAT_NONE);
		if (result < 0) {
			MPV_STREAMING_LOG(("MPV libmpv: mpv_observe_property(%1) failed: %2.")
				.arg(QString::fromLatin1(name))
				.arg(_api.describeError(result)));
		}
	}

	void observeDebugProperties() {
		if (!MpvDebugLogsEnabled()) {
			return;
		}
		auto id = uint64_t(1);
		for (const auto *name : {
			"file-format",
			"video-codec",
			"audio-codec-name",
			"video-format",
			"hwdec-current",
			"seekable",
			"paused-for-cache",
			"cache-buffering-state",
			"cache-speed",
			"width",
			"height",
		}) {
			observeProperty(name, id++);
		}
		logPropertySnapshot("observe-registered");
	}

	[[nodiscard]] bool setOption(const char *name, const QString &value) {
		const auto bytes = value.toUtf8();
		const auto result = _api.setOptionString(_handle, name, bytes.constData());
		if (result < 0) {
			MPV_STREAMING_LOG(("MPV Streaming: mpv_set_option_string(%1) failed: %2.")
				.arg(QString::fromLatin1(name))
				.arg(_api.describeError(result)));
			return false;
		}
		return true;
	}

	void requestClose() {
		if (_closing.exchange(true) || !_handle) {
			return;
		}
		MPV_STREAMING_LOG(("MPV libmpv: Requesting shutdown for document %1.")
			.arg(qulonglong(_document->id)));
		const char *command[] = { "quit", nullptr };
		_api.command(_handle, command);
	}

	void eventLoop() {
		while (_handle) {
			const auto event = _api.waitEvent(_handle, -1);
			if (!event) {
				continue;
			}
			if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
				const auto message = static_cast<mpv_event_log_message*>(event->data);
				if (message) {
					const auto text = MpvLogLine(message->text);
					if (!text.isEmpty()) {
						MPV_STREAMING_LOG(("MPV libmpv event: document=%1 prefix=%2 level=%3 text=%4")
							.arg(qulonglong(_document->id))
							.arg(MpvLogString(message->prefix))
							.arg(MpvLogString(message->level))
							.arg(text));
					}
				}
			} else if (event->event_id == MPV_EVENT_FILE_LOADED) {
				MPV_STREAMING_LOG(("MPV Streaming: libmpv file loaded for document %1.")
					.arg(qulonglong(_document->id)));
				logPropertySnapshot("file-loaded");
			} else if (event->event_id == MPV_EVENT_SEEK) {
				MPV_STREAMING_LOG(("MPV Streaming: libmpv seek event for document %1.")
					.arg(qulonglong(_document->id)));
			} else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) {
				MPV_STREAMING_LOG(("MPV Streaming: libmpv playback restart for document %1.")
					.arg(qulonglong(_document->id)));
				logPropertySnapshot("playback-restart");
			} else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
				const auto property = static_cast<mpv_event_property*>(event->data);
				if (property && property->name) {
					MPV_STREAMING_LOG(("MPV libmpv property: document=%1 name=%2 value=%3")
						.arg(qulonglong(_document->id))
						.arg(QString::fromLatin1(property->name))
						.arg(propertyValue(property->name)));
				}
			} else if (event->event_id == MPV_EVENT_END_FILE) {
				const auto end = static_cast<mpv_event_end_file*>(event->data);
				if (end && end->error < 0) {
					MPV_STREAMING_LOG(("MPV Streaming: libmpv end-file error for document %1: %2.")
						.arg(qulonglong(_document->id))
						.arg(_api.describeError(end->error)));
				}
			} else if (event->event_id == MPV_EVENT_SHUTDOWN) {
				break;
			}
		}
		if (_handle) {
			_api.terminateDestroy(_handle);
			_handle = nullptr;
		}
		if (const auto app = QCoreApplication::instance()) {
			const auto self = shared_from_this();
			QTimer::singleShot(0, app, [self] {
				if (self->_window) {
					self->_window->markClosingFromBackend();
					self->_window->close();
					self->_window->deleteLater();
					self->_window = nullptr;
				}
				if (self->_session) {
					self->_session->close();
				}
				LibMpvControlledPlayer::release(self);
			});
		}
	}

	static int openStream(void *userData, char *uri, mpv_stream_cb_info *info) {
		const auto player = static_cast<LibMpvControlledPlayer*>(userData);
		if (!player || !player->_session || !info || !uri) {
			return MPV_ERROR_LOADING_FAILED;
		}
		const auto scheme = player->_protocol.toUtf8() + "://";
		if (!QByteArray(uri).startsWith(scheme)) {
			return MPV_ERROR_LOADING_FAILED;
		}
		MPV_STREAMING_LOG(("MPV libmpv: open_stream for document %1 uri=%2 size=%3.")
			.arg(qulonglong(player->_document->id))
			.arg(QString::fromUtf8(uri))
			.arg(player->_session->streamSize()));
		auto *cookie = new LibMpvStreamCookie{ player->_session };
		info->cookie = cookie;
		info->read_fn = &LibMpvControlledPlayer::readStream;
		info->seek_fn = &LibMpvControlledPlayer::seekStream;
		info->size_fn = &LibMpvControlledPlayer::sizeStream;
		info->close_fn = &LibMpvControlledPlayer::closeStream;
		info->cancel_fn = &LibMpvControlledPlayer::cancelStream;
		return MPV_ERROR_SUCCESS;
	}

	static int64_t readStream(void *cookie, char *buffer, uint64_t nbytes) {
		const auto data = static_cast<LibMpvStreamCookie*>(cookie);
		return data && data->session ? data->session->read(buffer, nbytes) : -1;
	}

	static int64_t seekStream(void *cookie, int64_t offset) {
		const auto data = static_cast<LibMpvStreamCookie*>(cookie);
		return data && data->session
			? data->session->seek(offset)
			: MPV_ERROR_GENERIC;
	}

	static int64_t sizeStream(void *cookie) {
		const auto data = static_cast<LibMpvStreamCookie*>(cookie);
		return data && data->session
			? data->session->streamSize()
			: MPV_ERROR_UNSUPPORTED;
	}

	static void closeStream(void *cookie) {
		auto *data = static_cast<LibMpvStreamCookie*>(cookie);
		if (data && data->session) {
			MPV_STREAMING_LOG(("MPV libmpv: close_stream callback."));
			data->session->close();
		}
		delete data;
	}

	static void cancelStream(void *cookie) {
		const auto data = static_cast<LibMpvStreamCookie*>(cookie);
		if (data && data->session) {
			MPV_STREAMING_LOG(("MPV libmpv: cancel_stream callback."));
			data->session->cancel();
		}
	}

	inline static std::mutex _registryMutex;
	inline static std::vector<std::shared_ptr<LibMpvControlledPlayer>> _registry;

	const not_null<DocumentData*> _document;
	std::shared_ptr<LibMpvStreamSession> _session;
	QString _programPath;
	QString _protocol;
	LibMpvApi _api;
	mpv_handle *_handle = nullptr;
	QPointer<LibMpvWindow> _window;
	std::thread _eventThread;
	std::atomic<bool> _closing = false;
	bool _initialized = false;
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
	reader->setLoaderPriority(kMpvLoaderPriority);
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
			int64 offset) {
		const auto fresh = CreateDedicatedReaderFromWorker(
			entry->document,
			entry->origin);
			if (!fresh) {
				MPV_STREAMING_LOG(("MPV Streaming: Failed to recreate reader for token %1 at offset %2.")
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

[[nodiscard]] bool StartDetachedPlayer(
		const QString &program,
		const QString &target) {
	auto process = QProcess();
	process.setProgram(program);
	process.setArguments(LaunchArguments(target));
	process.setWorkingDirectory(QFileInfo(program).absolutePath());
	process.setProcessEnvironment(LaunchEnvironment());
	return process.startDetached();
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

[[nodiscard]] quint64 ReadMp4AtomSize(
		const QByteArray &data,
		int offset,
		int *headerSize) {
	const auto sizeRaw = qFromBigEndian<quint32>(
		reinterpret_cast<const uchar*>(data.constData() + offset));
	if (sizeRaw != 1) {
		*headerSize = 8;
		return sizeRaw;
	}
	if (offset + 16 > data.size()) {
		*headerSize = 0;
		return 0;
	}
	*headerSize = 16;
	return qFromBigEndian<quint64>(
		reinterpret_cast<const uchar*>(data.constData() + offset + 8));
}

[[nodiscard]] Mp4Layout DetectMp4Layout(const QByteArray &data) {
	constexpr auto kLargeFrontMoovThreshold = quint64(2 * 1024 * 1024);
	if (data.size() < 16) {
		return Mp4Layout::Unknown;
	}
	auto offset = 0;
	auto sawMoov = false;
	while (offset + 8 <= data.size()) {
		auto headerSize = 0;
		const auto atomSize = ReadMp4AtomSize(data, offset, &headerSize);
		if ((headerSize == 0) || (atomSize < quint64(headerSize))) {
			return Mp4Layout::Unknown;
		}
		const auto type = QByteArray::fromRawData(
			data.constData() + offset + 4, 4);
		if (type == QByteArray("moov", 4)) {
			sawMoov = true;
			if (atomSize >= kLargeFrontMoovThreshold) {
				return Mp4Layout::LargeFrontMoov;
			}
			const auto payloadOffset = offset + headerSize;
			const auto available = std::max(data.size() - payloadOffset, 0);
			const auto payloadSize = int(std::min(
				atomSize - quint64(headerSize),
				quint64(available)));
			if (payloadSize > 0) {
				const auto payload = QByteArray::fromRawData(
					data.constData() + payloadOffset,
					payloadSize);
				if (payload.contains("mvex")) {
					return Mp4Layout::Fragmented;
				}
			}
		} else if (type == QByteArray("moof", 4)) {
			return sawMoov ? Mp4Layout::Fragmented : Mp4Layout::Regular;
		} else if (type == QByteArray("mdat", 4)) {
			return Mp4Layout::Regular;
		}
		if (atomSize > quint64(data.size() - offset)) {
			return Mp4Layout::Unknown;
		}
		offset += int(atomSize);
	}
	return Mp4Layout::Unknown;
}

[[nodiscard]] bool UsesCompatibilityLateSeekGate(
		const std::shared_ptr<Entry> &entry) {
	return (entry->mp4Layout.load() == int(Mp4Layout::LargeFrontMoov))
		&& entry->preferCompatibilityForLargeFrontMoov;
}

[[nodiscard]] bool AllowsCompatibilityLateSeekOffset(
		const std::shared_ptr<Entry> &entry,
		int64 offset) {
	return UsesCompatibilityLateSeekGate(entry)
		&& (offset >= kCompatibilityLateSeekMinOffset);
}

void MarkCompatibilityLateSeekReady(
		const std::shared_ptr<Entry> &entry,
		const QString &token,
		const char *reason) {
	if (!UsesCompatibilityLateSeekGate(entry)) {
		return;
	}
	if (!entry->compatibilityLateSeekReady.exchange(true)) {
		MPV_STREAMING_LOG(("MPV Streaming: Compatibility late seek ready for token %1 reason=%2 bootstrapBytes=%3.")
			.arg(token)
			.arg(QString::fromLatin1(reason))
			.arg(entry->compatibilityBootstrapBytes.load()));
	}
}

void NoteCompatibilityBootstrapProgress(
		const std::shared_ptr<Entry> &entry,
		const QString &token,
		int size) {
	if (!UsesCompatibilityLateSeekGate(entry) || (size <= 0)) {
		return;
	}
	const auto served = entry->compatibilityBootstrapBytes.fetch_add(size) + size;
	if (served >= kCompatibilitySeekBootstrapBytes) {
		MarkCompatibilityLateSeekReady(entry, token, "bootstrap-bytes");
	}
}

[[nodiscard]] bool WaitForCompatibilityLateSeekReady(
		const std::shared_ptr<Entry> &entry,
		const QString &token,
		int64 offset) {
	if (!UsesCompatibilityLateSeekGate(entry)
		|| entry->compatibilityLateSeekReady.load()) {
		return true;
	}
	const auto started = crl::now();
	while (!entry->compatibilityLateSeekReady.load()) {
		if ((crl::now() - started) >= kCompatibilitySeekWaitTimeout) {
			MPV_STREAMING_LOG(("MPV Streaming: Compatibility late seek wait timed out for token %1 offset=%2 bootstrapBytes=%3 activeRequests=%4.")
				.arg(token)
				.arg(offset)
				.arg(entry->compatibilityBootstrapBytes.load())
				.arg(entry->activeRequests.load()));
			return false;
		}
		std::this_thread::sleep_for(
			std::chrono::milliseconds(kCompatibilitySeekWaitStep));
	}
	MPV_STREAMING_LOG(("MPV Streaming: Compatibility late seek released for token %1 offset=%2 bootstrapBytes=%3.")
		.arg(token)
		.arg(offset)
		.arg(entry->compatibilityBootstrapBytes.load()));
	return true;
}

[[nodiscard]] bool WaitForMp4LayoutForSeek(
		const std::shared_ptr<Entry> &entry,
		const QString &token,
		int64 offset) {
	if (!entry->preferCompatibilityForLargeFrontMoov
		|| (entry->mp4Layout.load() != 0)) {
		return true;
	}
	const auto started = crl::now();
	while (entry->mp4Layout.load() == 0) {
		if ((crl::now() - started) >= kCompatibilitySeekWaitTimeout) {
			MPV_STREAMING_LOG(("MPV Streaming: MP4 layout wait timed out for token %1 offset=%2 bootstrapBytes=%3 activeRequests=%4.")
				.arg(token)
				.arg(offset)
				.arg(entry->compatibilityBootstrapBytes.load())
				.arg(entry->activeRequests.load()));
			return false;
		}
		std::this_thread::sleep_for(
			std::chrono::milliseconds(kCompatibilitySeekWaitStep));
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
			std::shared_ptr<Reader> reader,
			bool preferCompatibilityForLargeFrontMoov) {
		if (!ensureListening()) {
			return {};
		}
		auto entry = std::make_shared<Entry>(
			document,
			origin,
			std::move(reader),
			preferCompatibilityForLargeFrontMoov);
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
			if (i == end(_entries) || i->second->activeRequests.load() > 0) {
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
			const auto releaseGuard = gsl::finally([&] { release(entry); });
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
			// Probe the MP4 header once and keep the fallback narrow.
			// Ordinary faststart files also place moov before mdat,
			// but only fragmented files or videos with a very large
			// front moov should take the sequential-open fallback.
			if (entry->mp4Layout.load() == 0
				&& range.range.from == 0
				&& range.satisfiable) {
				auto probeActual = int(std::min(
					int64(kMp4ProbeInitialSize), entry->size));
				const auto probeMax = int(std::min(
					int64(kMp4ProbeMaxSize), entry->size));
				auto detected = Mp4Layout::Unknown;
				{
					const auto lock = std::unique_lock(entry->fillMutex);
					while (true) {
						auto probe = QByteArray(probeActual, Qt::Uninitialized);
						if (!FillBuffer(
								entry->reader.get(),
								0,
								bytes::span(
									reinterpret_cast<bytes::type*>(probe.data()),
									probeActual))) {
							detected = Mp4Layout::Regular;
							break;
						}
						detected = DetectMp4Layout(probe);
						if (detected != Mp4Layout::Unknown
							|| probeActual >= probeMax) {
							break;
						}
						probeActual = std::min(probeActual * 2, probeMax);
					}
					if (detected == Mp4Layout::Unknown) {
						detected = Mp4Layout::Regular;
					}
					entry->mp4Layout.store(int(detected));
					MPV_STREAMING_LOG(("MPV Streaming: Detected MP4 layout: %1 for token %2 after probing %3 bytes.")
						.arg(int(detected))
						.arg(request.token)
						.arg(probeActual));
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
			const auto compatibilitySequentialLayout =
				(entry->mp4Layout.load() == int(Mp4Layout::Fragmented));
			const auto compatibilityLateSeekGate =
				UsesCompatibilityLateSeekGate(entry);
			const auto compatibilityFarSeek =
				AllowsCompatibilityLateSeekOffset(entry, range.range.from);
			if (compatibilityLateSeekGate
				&& (range.range.from > 0)
				&& !compatibilityFarSeek) {
				MPV_STREAMING_LOG(("MPV Streaming: Keeping compatibility sequential path for token %1 offset=%2 minOffset=%3.")
					.arg(request.token)
					.arg(range.range.from)
					.arg(kCompatibilityLateSeekMinOffset));
			}
			if (compatibilityFarSeek
				&& (range.range.from > 0)
				&& !WaitForCompatibilityLateSeekReady(
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
			const auto compatibilitySequentialRequest =
				compatibilitySequentialLayout
				|| (compatibilityLateSeekGate
					&& (range.range.from > 0)
					&& !compatibilityFarSeek);
			const auto sequentialLayout =
				compatibilitySequentialRequest
				|| (entry->mp4Layout.load() == int(Mp4Layout::LargeFrontMoov));
			const auto initialSequentialOpen =
				sequentialLayout
				&& (range.range.from == 0);
			if (compatibilitySequentialRequest) {
				if (!SendResponse(socket, "200 OK", {
					{ "Connection", "close" },
					{ "Content-Length", QByteArray::number(entry->size) },
					{ "Content-Type", entry->mime.toUtf8() },
				})) {
					return;
				}
			} else if (initialSequentialOpen) {
				if (!SendResponse(socket, "200 OK", {
					{ "Accept-Ranges", "bytes" },
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
			if (request.method == "HEAD") {
				return;
			}
			auto offset = compatibilitySequentialRequest
				? int64(0)
				: initialSequentialOpen
				? int64(0)
				: range.range.from;
			auto left = compatibilitySequentialRequest
				? entry->size
				: initialSequentialOpen
				? entry->size
				: range.range.length;
			const auto startedFromZero = (offset == 0);
			const auto isolatedSeekRequest =
				sequentialLayout
				&& !compatibilitySequentialRequest
				&& !initialSequentialOpen
				&& (range.range.from > 0);
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
							&& RecoverEntryReader(entry, request.token, offset)) {
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
				entry->lastActivity = crl::now();
				if (startedFromZero && !usingSeekReader) {
					NoteCompatibilityBootstrapProgress(
						entry,
						request.token,
						size);
				}
			}
			if (supersededSeek) {
				MPV_STREAMING_LOG(("MPV Streaming: Superseded seek request token=%1 generation=%2 offset=%3 latest=%4.")
					.arg(request.token)
					.arg(qulonglong(seekGeneration))
					.arg(range.range.from)
					.arg(qulonglong(entry->latestSeekGeneration.load())));
				return;
			}
			if (clientDisconnected && startedFromZero && !usingSeekReader) {
				MarkCompatibilityLateSeekReady(
					entry,
					request.token,
					"initial-stream-ended");
			}
			// Pre-fill cache sequentially after client disconnect.
			// When a fragmented MP4 is opened, the demuxer scans
			// hundreds of fragment headers via HTTP range requests.
			// By continuing to fill the cache sequentially here,
			// those seek connections find data already cached and
			// complete almost instantly instead of each downloading
			// from Telegram independently (~150ms per seek).
			if (clientDisconnected && startedFromZero && left > 0) {
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

[[nodiscard]] OpenResult StartExternalBridgePlayback(
		not_null<DocumentData*> document,
		Data::FileOrigin origin,
		const QString &program,
		std::shared_ptr<Reader> reader,
		bool preferCompatibilityForLargeFrontMoov) {
	const auto launch = Server::instance().add(
		document,
		origin,
		std::move(reader),
		preferCompatibilityForLargeFrontMoov);
	if (launch.url.isEmpty()) {
		MPV_STREAMING_LOG(("MPV Streaming: Failed to create launch URL for document %1.")
			.arg(qulonglong(document->id)));
		return OpenResult::Failed;
	}
	MPV_STREAMING_LOG(("MPV Streaming: Launching '%1' with URL %2.")
		.arg(program)
		.arg(launch.url));
	MPV_STREAMING_LOG(("MPV Streaming: Launch arguments: %1.")
		.arg(LaunchArguments(launch.url).join(QStringLiteral(" "))));
	if (!StartDetachedPlayer(program, launch.url)) {
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

	OpenResult OpenVideoMessageInMpv(HistoryItem *item, DocumentData *document) {
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
		const auto preferCompatibilityForLargeFrontMoov = media
			? !media->hasQualitiesList()
			: true;
		MPV_STREAMING_LOG(("MPV Streaming: Bridge strategy preferCompatibilityForLargeFrontMoov=%1 hasQualities=%2.")
			.arg(preferCompatibilityForLargeFrontMoov ? 1 : 0)
			.arg(media ? media->hasQualitiesList() : 0));
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
			std::move(reader),
			preferCompatibilityForLargeFrontMoov);
	}

	OpenResult OpenVideoMessageInLibMpv(HistoryItem *item, DocumentData *document) {
		const auto media = item ? item->media() : nullptr;
		const auto mediaDocument = media ? media->document() : nullptr;
		MPV_STREAMING_LOG(("MPV libmpv: Open request passedDocument=%1 mediaDocument=%2 same=%3 passedSize=%4 mediaSize=%5 passedSupports=%6 mediaSupports=%7 passedLoader=%8 mediaLoader=%9 hasQualities=%10.")
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
			MPV_STREAMING_LOG(("MPV libmpv: Player not found."));
			return OpenResult::PlayerNotFound;
		}
		const auto libmpvPath = ResolveLibMpvLibraryPath(program);
		if (libmpvPath.isEmpty()) {
			MPV_STREAMING_LOG(("MPV libmpv: libmpv was not found next to '%1'.")
				.arg(program));
			return OpenResult::PlayerNotFound;
		}
		const auto origin = Data::FileOrigin(item->fullId());
		auto reader = CreateDedicatedReader(document, origin);
		if (!reader) {
			MPV_STREAMING_LOG(("MPV libmpv: Failed to create dedicated reader for document %1.")
				.arg(qulonglong(document->id)));
			return OpenResult::Failed;
		}
		auto controlled = std::make_shared<LibMpvControlledPlayer>(
			document,
			std::make_shared<LibMpvStreamSession>(document, origin, reader),
			program);
		if (!controlled->start()) {
			MPV_STREAMING_LOG(("MPV libmpv: Failed to start controlled backend for document %1.")
				.arg(qulonglong(document->id)));
			return OpenResult::Failed;
		}
		LibMpvControlledPlayer::keepAlive(controlled);
		MPV_STREAMING_LOG(("MPV libmpv: Routed document %1 to libmpv controlled backend.")
			.arg(qulonglong(document->id)));
		return OpenResult::Success;
	}

	#undef MPV_STREAMING_LOG

} // namespace Media::Streaming::Mpv
