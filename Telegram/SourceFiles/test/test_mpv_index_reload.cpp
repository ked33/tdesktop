/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_mpv_index.h"

#include "base/basic_types.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QThread>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>

namespace {

namespace Mpv = Media::Streaming::Mpv;

auto Checks = 0;

void Check(bool condition, const char *name) {
	++Checks;
	if (!condition) {
		std::cerr << "FAILED: " << name << '\n';
		std::exit(1);
	}
}

template <typename Predicate>
[[nodiscard]] bool WaitUntil(Predicate condition, int timeout = 2500) {
	auto timer = QElapsedTimer();
	timer.start();
	while (!condition()) {
		if (timer.elapsed() >= timeout) {
			return false;
		}
		QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
		QThread::msleep(1);
	}
	return true;
}

void Drain() {
	auto timer = QElapsedTimer();
	timer.start();
	Check(WaitUntil([&] { return timer.elapsed() >= 350; }), "event loop drains");
}

class Peer final : public QObject {
public:
	Peer();
	~Peer();

	void seek(
		double target,
		std::optional<double> landing,
		bool snapshot = true);
	void property(const QString &name, QJsonValue value);
	void event(const QString &name);
	void log(const QString &text, const QString &prefix = u"cplayer"_q);
	void seekCommand(
		const QString &target,
		const QString &flags = u"absolute+exact"_q);
	void highResolutionSeek(double target);
	void restartLog(double position);
	void releasePositionReply(double position);
	void completeReload();
	[[nodiscard]] bool paused() const;

	std::vector<std::int64_t> requests;
	std::vector<QJsonObject> reloads;
	std::optional<QJsonObject> delayedPositionReply;
	int positionReplies = 0;
	int settled = 0;
	int abandoned = 0;
	bool delayNextPositionReply = false;
	bool indexReady = true;
	bool finishReloads = true;
	bool restartReloads = true;
	double viewStart = -1.;

private:
	void receive();
	void handle(const QJsonObject &message);
	void send(QJsonObject message);

	QLocalServer _server;
	QLocalSocket *_socket = nullptr;
	QByteArray _buffer;
	QJsonObject _properties;
	QJsonObject _reloadOptions;
	std::map<QString, int> _observed;
	std::unique_ptr<QProcess> _process;
	std::uint64_t _revision = 0;
	bool _idleRead = false;
	bool _logsRequested = false;

};

Peer::Peer() : _process(std::make_unique<QProcess>()) {
	const auto url = u"http://127.0.0.1/video.mp4"_q;
	_properties = {
		{ u"path"_q, url },
		{ u"pause"_q, true },
		{ u"duration"_q, 845. },
		{ u"seeking"_q, false },
		{ u"time-pos"_q, 572. },
		{ u"idle-active"_q, false },
		{ u"idle"_q, false },
		{ u"eof-reached"_q, false },
		{ u"mpv-version"_q, u"mpv 0.41.0"_q },
	};
	_process->setArguments({ url });
	auto control = Mpv::IndexControl{
		.state = [] { return Mpv::IndexState::OnDemand; },
		.request = [=](std::int64_t position) {
			requests.push_back(position);
			return ++_revision;
		},
		.ready = [=](std::uint64_t revision) { return indexReady && revision != 0; },
		.seekStart = [=](std::uint64_t) { return viewStart; },
		.settled = [=] { ++settled; },
		.abandoned = [=] { ++abandoned; },
	};
	Mpv::ManageIndexReload(_process.get(), std::move(control), 845000);
	const auto endpoint = _process->arguments().front().mid(
		u"--input-ipc-server="_q.size());
	Check(_server.listen(endpoint), "fake MPV listens on the controller endpoint");
	connect(&_server, &QLocalServer::newConnection, this, [=] {
		_socket = _server.nextPendingConnection();
		connect(_socket, &QLocalSocket::readyRead, this, [=] { receive(); });
	});
	Check(WaitUntil([&] { return _idleRead && _logsRequested; }),
		"controller initializes IPC properties and debug logs");
	Drain();
}

Peer::~Peer() {
	if (_socket) {
		QObject::disconnect(_socket, nullptr, this, nullptr);
	}
	_process.reset();
}

void Peer::send(QJsonObject message) {
	const auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
	Check(_socket->write(bytes) == bytes.size(), "fake MPV writes an IPC message");
}

void Peer::event(const QString &name) {
	send({ { u"event"_q, name } });
}

void Peer::log(const QString &text, const QString &prefix) {
	Check(_logsRequested, "fake MPV sends only subscribed logs");
	send({
		{ u"event"_q, u"log-message"_q },
		{ u"prefix"_q, prefix },
		{ u"level"_q, u"debug"_q },
		{ u"text"_q, text },
	});
}

void Peer::seekCommand(const QString &target, const QString &flags) {
	log(u"Run command: seek, flags=64, args=[target=\"%1\", "
		u"flags=\"%2\", legacy=\"unused\"]\n"_q.arg(target, flags));
}

void Peer::highResolutionSeek(double target) {
	log(u"hr-seek, skipping to %1\n"_q.arg(QString::number(target, 'f', 6)));
}

void Peer::restartLog(double position) {
	const auto suffix = _properties.value(u"pause"_q).toBool()
		? u" (paused)"_q
		: QString();
	log(u"playback restart complete @ %1, audio=ready, video=playing%2\n"_q.arg(
		QString::number(position, 'f', 6), suffix));
}

void Peer::releasePositionReply(double position) {
	Check(delayedPositionReply.has_value(), "a time-pos reply is pending");
	auto reply = std::move(*delayedPositionReply);
	delayedPositionReply.reset();
	reply.insert(u"data"_q, position);
	++positionReplies;
	send(std::move(reply));
}

void Peer::property(const QString &name, QJsonValue value) {
	_properties.insert(name, value);
	const auto i = _observed.find(name);
	if (i != _observed.end()) {
		send({
			{ u"event"_q, u"property-change"_q },
			{ u"name"_q, name },
			{ u"id"_q, i->second },
			{ u"data"_q, value },
		});
	}
}

void Peer::seek(
		double target,
		std::optional<double> landing,
		bool snapshot) {
	event(u"seek"_q);
	property(u"seeking"_q, true);
	if (snapshot) {
		property(u"time-pos"_q, target);
	} else {
		_properties.insert(u"time-pos"_q, target);
	}
	if (landing) {
		event(u"playback-restart"_q);
		property(u"seeking"_q, false);
		property(u"time-pos"_q, *landing);
	}
}

void Peer::receive() {
	_buffer += _socket->readAll();
	while (true) {
		const auto end = _buffer.indexOf('\n');
		if (end < 0) {
			return;
		}
		const auto document = QJsonDocument::fromJson(_buffer.left(end));
		_buffer.remove(0, end + 1);
		Check(document.isObject(), "controller writes a JSON command");
		handle(document.object());
	}
}

void Peer::handle(const QJsonObject &message) {
	const auto command = message.value(u"command"_q).toArray();
	const auto name = command.first().toString();
	auto reply = QJsonObject{
		{ u"request_id"_q, message.value(u"request_id"_q) },
		{ u"error"_q, u"success"_q },
	};
	if (name == u"request_log_messages"_q) {
		Check(command[1].toString() == u"debug"_q,
			"controller subscribes to MPV debug logs");
		_logsRequested = true;
	} else if (name == u"observe_property"_q) {
		const auto propertyName = command[2].toString();
		_observed.emplace(propertyName, command[1].toInt());
		property(propertyName, _properties.value(propertyName));
	} else if (name == u"get_property"_q) {
		const auto propertyName = command[1].toString();
		reply.insert(u"data"_q, _properties.value(propertyName));
		_idleRead = _idleRead || (propertyName == u"idle"_q);
		if (propertyName == u"time-pos"_q && delayNextPositionReply) {
			Check(!delayedPositionReply, "only one time-pos reply is delayed");
			delayNextPositionReply = false;
			delayedPositionReply = std::move(reply);
			return;
		}
		positionReplies += (propertyName == u"time-pos"_q);
	} else if (name == u"set_property"_q) {
		property(command[1].toString(), command[2]);
	} else if (name == u"loadfile"_q) {
		const auto options = command.last().toObject();
		_reloadOptions = options;
		reloads.push_back(options);
		send(reply);
		log(u"Run command: loadfile, flags=64, args=[url=\"%1\", "
			u"flags=\"replace\", index=\"-1\", options=\"\"]\n"_q.arg(
				command[1].toString()));
		send({
			{ u"event"_q, u"end-file"_q },
			{ u"reason"_q, u"stop"_q },
		});
		event(u"start-file"_q);
		property(u"path"_q, command[1]);
		property(u"eof-reached"_q, false);
		property(u"time-pos"_q, QJsonValue());
		if (finishReloads) {
			completeReload();
		}
		return;
	} else {
		Check(false, "controller command is supported by the fake MPV");
	}
	send(reply);
}

void Peer::completeReload() {
	if (_reloadOptions.contains(u"pause"_q)) {
		property(u"pause"_q,
			_reloadOptions.value(u"pause"_q).toString() == u"yes"_q);
	}
	event(u"file-loaded"_q);
	const auto position = _reloadOptions.value(u"start"_q).toString().toDouble();
	highResolutionSeek(position);
	seek(position, restartReloads ? std::make_optional(position) : std::nullopt);
	if (restartReloads) {
		restartLog(position);
	}
}

bool Peer::paused() const {
	return _properties.value(u"pause"_q).toBool();
}

void CheckReload(Peer &peer, double target) {
	Check(WaitUntil([&] { return !peer.reloads.empty(); }), "uncached seek reloads");
	Check(peer.requests == std::vector<std::int64_t>{ std::int64_t(target * 1000) },
		"metadata preparation uses the requested target, not the decoded position");
	const auto actual = peer.reloads.front().value(u"start"_q).toString().toDouble();
	Check(std::abs(actual - target) < 0.000001, "reload starts at the requested target");
	Drain();
	Check(peer.reloads.size() == 1, "the reload's own seek does not reload again");
	Check(peer.abandoned == 0, "the indexed URL retains its controller");
}

void CheckNoReload(Peer &peer, const char *name) {
	Drain();
	Check(peer.requests.empty() && peer.reloads.empty(), name);
	Check(peer.abandoned == 0, "ignored logs retain the controller");
}

void TestBackwardSnapshot() {
	auto peer = Peer();
	peer.seek(114.4, 570.);
	CheckReload(peer, 114.4);
	Check(peer.settled == 1, "only the corrected reload completes the seek");
}

void TestCachedSnapshot() {
	auto peer = Peer();
	peer.seek(574., 574.);
	Check(WaitUntil([&] { return peer.settled == 1; }), "cached seek completes");
	Drain();
	Check(peer.requests.empty() && peer.reloads.empty(), "cached seek avoids reload");
}

void TestLatestSnapshot() {
	auto peer = Peer();
	peer.seek(114.4, 570.);
	peer.seek(158.5, 570.);
	peer.property(u"pause"_q, false);
	CheckReload(peer, 158.5);
	Check(!peer.paused(),
		"resume after dragging is preserved");
}

void TestQueryFallback() {
	auto peer = Peer();
	peer.seek(114.4, std::nullopt, false);
	CheckReload(peer, 114.4);
}

void TestBeginningSnapshot() {
	auto peer = Peer();
	peer.seek(0., 570.);
	CheckReload(peer, 0.);
}

void TestBackwardLateLogs() {
	auto peer = Peer();
	peer.seek(114.400759, 570., false);
	Drain();
	Check(peer.positionReplies > 0,
		"wrong landing queries are answered before seek logs arrive");
	Check(peer.requests.empty(), "wrong landing does not prepare its own index");
	peer.seekCommand(u"114.400759"_q);
	peer.highResolutionSeek(114.400759);
	peer.restartLog(570.);
	CheckReload(peer, 114.400759);
}

void TestCachedLateLogs() {
	auto peer = Peer();
	peer.seek(574., 574., false);
	Drain();
	Check(peer.positionReplies > 0,
		"cached landing queries are answered before seek logs arrive");
	peer.seekCommand(u"574.000000"_q);
	peer.highResolutionSeek(574.);
	peer.restartLog(574.);
	CheckNoReload(peer, "late logs for a cached seek avoid metadata and reload");
	Check(peer.settled > 0, "cached seek completes even with late logs");
}

void TestCachedLateLogsAfterPlaybackAdvances() {
	auto peer = Peer();
	peer.seek(574., 574., false);
	Drain();
	Check(peer.settled > 0, "cached seek finishes before playback advances");
	peer.property(u"pause"_q, false);
	peer.property(u"time-pos"_q, 576.);
	peer.seekCommand(u"574.000000"_q);
	peer.highResolutionSeek(574.);
	peer.restartLog(574.);
	CheckNoReload(peer,
		"late restart logs use their landing instead of later playback progress");
}

void TestOldPositionReplyAfterLoggedCompletion() {
	auto peer = Peer();
	peer.delayNextPositionReply = true;
	peer.seek(574., 574., false);
	Check(WaitUntil([&] { return peer.delayedPositionReply.has_value(); }),
		"the seek position reply waits until after logged completion");
	peer.seekCommand(u"574.000000"_q);
	peer.highResolutionSeek(574.);
	peer.restartLog(574.);
	Check(WaitUntil([&] { return peer.settled == 1; }),
		"the fixed restart landing completes the seek before its old query");
	peer.property(u"pause"_q, false);
	peer.property(u"time-pos"_q, 576.);
	peer.releasePositionReply(576.);
	peer.property(u"time-pos"_q, 576.4);
	CheckNoReload(peer, "an old position reply cannot revive a completed seek");
	Check(peer.settled == 1, "an old position reply cannot finish another seek");
}

void TestCachedKeyframeLanding() {
	auto peer = Peer();
	peer.seek(574., 572., false);
	Drain();
	Check(peer.positionReplies > 0,
		"the preceding keyframe is queried without a target snapshot");
	peer.seekCommand(u"574.000000"_q, u"absolute+keyframes"_q);
	peer.restartLog(572.);
	CheckNoReload(peer,
		"a cached keyframe landing before the target avoids an exact reload");
}

void TestLogsBeforeSeekEvent() {
	auto peer = Peer();
	peer.seekCommand(u"114.400759"_q);
	peer.highResolutionSeek(114.400759);
	peer.seek(114.400759, 570., false);
	peer.restartLog(570.);
	CheckReload(peer, 114.400759);
	Check(peer.settled == 1,
		"the wrong landing stays pending until the corrected reload completes");
}

void TestExecutedLogTarget() {
	auto peer = Peer();
	peer.seekCommand(u"115.400759"_q);
	peer.highResolutionSeek(114.400759);
	peer.seek(115.400759, 570., false);
	peer.restartLog(570.);
	CheckReload(peer, 114.400759);
}

void TestLatestSeekCommand() {
	auto peer = Peer();
	peer.seekCommand(u"305.068690"_q);
	peer.highResolutionSeek(305.068690);
	peer.seek(305.068690, 570., false);
	peer.seekCommand(u"303.061659"_q, u"absolute+keyframes"_q);
	peer.seek(303.061659, 570., false);
	peer.restartLog(570.);
	CheckReload(peer, 303.061659);
}

void TestBackwardKeyframeCommand() {
	auto peer = Peer();
	peer.seekCommand(u"114.400759"_q, u"absolute+keyframes"_q);
	peer.seek(114.400759, 570., false);
	peer.restartLog(570.);
	CheckReload(peer, 114.400759);
}

void TestForwardKeyframeSnapshotWithoutRestart() {
	auto peer = Peer();
	peer.seekCommand(u"700.000000"_q, u"absolute+keyframes"_q);
	peer.seek(700., std::nullopt);
	CheckReload(peer, 700.);
}

void TestUnexecutedSeekCommands() {
	for (const auto &flags : { u"absolute+exact"_q, u"absolute+keyframes"_q }) {
		auto peer = Peer();
		peer.seekCommand(u"114.400759"_q, flags);
		CheckNoReload(peer, "a command without execution cannot trigger reload");
	}
}

void TestPendingCommandBlocksOldTarget() {
	auto peer = Peer();
	peer.seek(114.400759, std::nullopt);
	peer.seekCommand(u"158.500000"_q);
	CheckNoReload(peer, "a pending command blocks reloading an older target");
	peer.highResolutionSeek(158.5);
	peer.seek(158.5, 570., false);
	peer.restartLog(570.);
	CheckReload(peer, 158.5);
}

void TestPositionPropertyReplacesLoggedTarget() {
	const auto logs = {
		u"Set property: chapter=3 -> 1\n"_q,
		u"Set property: time-pos -> 1\n"_q,
	};
	for (const auto &log : logs) {
		auto peer = Peer();
		peer.seekCommand(u"319.000000"_q);
		peer.highResolutionSeek(319.);
		peer.seek(705., std::nullopt);
		peer.log(log);
		CheckReload(peer, 705.);
	}
}

void TestExecutedSubSeekReplacesLoggedTarget() {
	auto peer = Peer();
	peer.seekCommand(u"319.000000"_q);
	peer.highResolutionSeek(319.);
	peer.seek(705., std::nullopt);
	peer.log(u"Run command: sub-seek, flags=64, "
		u"args=[skip=\"1\", flags=\"primary\"]\n"_q);
	peer.log(u"Set property: volume=80 -> 1\n"_q);
	peer.highResolutionSeek(705.);
	CheckReload(peer, 705.);
}

void TestUnexecutedOtherSeekRetainsLoggedTarget() {
	const auto logs = {
		u"Run command: sub-seek, flags=64, "
			u"args=[skip=\"1\", flags=\"primary\"]\n"_q,
		u"Run command: revert-seek, flags=64, args=[flags=\"mark\"]\n"_q,
	};
	for (const auto &log : logs) {
		auto peer = Peer();
		peer.seekCommand(u"319.000000"_q);
		peer.highResolutionSeek(319.);
		peer.seek(705., std::nullopt);
		peer.log(log);
		CheckReload(peer, 319.);
	}
}

void TestUnrelatedPropertyLogsRetainLoggedTarget() {
	auto peer = Peer();
	peer.seekCommand(u"319.000000"_q);
	peer.highResolutionSeek(319.);
	peer.seek(705., std::nullopt);
	peer.log(u"Set property: chapter=3 -> -3\n"_q);
	peer.log(u"Set property: volume=80 -> 1\n"_q);
	peer.log(u"Set property: pause=yes -> 1\n"_q);
	CheckReload(peer, 319.);
}

void TestPlaybackStartClearsUnexecutedCommand() {
	auto peer = Peer();
	peer.event(u"start-file"_q);
	peer.seekCommand(u"114.400759"_q);
	peer.event(u"file-loaded"_q);
	peer.log(u"Starting playback...\n"_q);
	peer.highResolutionSeek(0.);
	peer.restartLog(0.);
	CheckNoReload(peer, "playback start discards a rejected startup command");
}

void TestLargeLogBatch() {
	auto peer = Peer();
	const auto text = QString(8192, QChar(u'x')) + '\n';
	for (auto i = 0; i != 256; ++i) {
		peer.log(text);
	}
	Drain();
	Check(peer.abandoned == 0,
		"a batch over one MiB of small log messages retains the controller");
	Check(peer.requests.empty() && peer.reloads.empty(),
		"unrelated log traffic does not request metadata or reload");
	peer.seekCommand(u"114.400759"_q);
	peer.highResolutionSeek(114.400759);
	peer.seek(114.400759, 570., false);
	peer.restartLog(570.);
	CheckReload(peer, 114.400759);
}

void TestUnknownLogFormat() {
	{
		auto peer = Peer();
		peer.seekCommand(u"114.400759"_q);
		peer.log(u"Run command: seek, flags=64, args=[target=114.400759]\n"_q);
		peer.highResolutionSeek(114.400759);
		peer.restartLog(570.);
		CheckNoReload(peer, "an unknown command cannot revive its predecessor");
	}
	{
		auto peer = Peer();
		peer.seekCommand(u"114.400759"_q);
		peer.log(u"hr-seek to 114.400759\n"_q);
		peer.log(u"playback restart complete at 570.000000\n"_q);
		CheckNoReload(peer, "unknown execution log formats are ignored");
	}
}

void TestNonFiniteLogTarget() {
	{
		auto peer = Peer();
		peer.seekCommand(u"114.400759"_q);
		peer.seekCommand(u"nan"_q);
		peer.highResolutionSeek(114.400759);
		peer.restartLog(570.);
		CheckNoReload(peer, "a NaN command cannot authorize an execution log");
	}
	{
		auto peer = Peer();
		peer.seekCommand(u"114.400759"_q);
		peer.log(u"hr-seek, skipping to nan\n"_q);
		CheckNoReload(peer, "a NaN execution target is ignored");
	}
}

void TestWrongLogPrefix() {
	{
		auto peer = Peer();
		peer.log(u"Run command: seek, flags=64, args=[target=\"114.400759\", "
			u"flags=\"absolute+exact\", legacy=\"unused\"]\n"_q, u"lavf"_q);
		peer.highResolutionSeek(114.400759);
		peer.restartLog(570.);
		CheckNoReload(peer, "a non-cplayer command log is ignored");
	}
	{
		auto peer = Peer();
		peer.seekCommand(u"114.400759"_q);
		peer.log(u"hr-seek, skipping to 114.400759\n"_q, u"lavf"_q);
		peer.log(u"playback restart complete @ 570.000000, "
			u"audio=ready, video=playing\n"_q, u"lavf"_q);
		CheckNoReload(peer, "non-cplayer execution logs are ignored");
	}
}

void TestAutomaticSeekLogs() {
	auto peer = Peer();
	peer.highResolutionSeek(114.400759);
	peer.restartLog(570.);
	CheckNoReload(peer, "automatic seek logs without a user command are ignored");
}

void TestLoadfileClearsCommand() {
	auto peer = Peer();
	peer.seekCommand(u"114.400759"_q);
	peer.log(u"Run command: loadfile, flags=64, args=["
		u"url=\"http://127.0.0.1/video.mp4\", flags=\"replace\", "
		u"index=\"-1\", options=\"\"]\n"_q);
	peer.highResolutionSeek(114.400759);
	peer.restartLog(570.);
	CheckNoReload(peer, "loadfile prevents an old seek command from reviving");
}

void TestNewFileClearsCommand() {
	auto peer = Peer();
	peer.seekCommand(u"114.400759"_q);
	peer.event(u"start-file"_q);
	peer.event(u"file-loaded"_q);
	peer.highResolutionSeek(114.400759);
	peer.restartLog(570.);
	CheckNoReload(peer, "a new file prevents an old seek command from reviving");
}

void TestUnsupportedCommandClearsCandidate() {
	auto peer = Peer();
	peer.seekCommand(u"114.400759"_q);
	peer.seekCommand(u"-5.000000"_q, u"relative+keyframes"_q);
	peer.restartLog(570.);
	CheckNoReload(peer, "an unsupported command replaces the prior candidate");
}

void TestRepeatedPendingTarget() {
	auto peer = Peer();
	peer.indexReady = false;
	peer.seekCommand(u"166.583561"_q);
	peer.highResolutionSeek(166.583561);
	peer.seek(166.583561, 429.6, false);
	peer.restartLog(429.6);
	Check(WaitUntil([&] { return peer.requests.size() == 1; }),
		"the uncached target starts preparing its index");
	peer.seekCommand(u"166.583561"_q);
	peer.highResolutionSeek(166.583561);
	peer.seek(166.583561, 429.6, false);
	peer.restartLog(429.6);
	Drain();
	Check(peer.requests == std::vector<std::int64_t>{ 166583 },
		"repeating the same target preserves its in-flight index");
	peer.indexReady = true;
	CheckReload(peer, 166.583561);
}

void StartIndexedPlayback(Peer &peer) {
	peer.viewStart = 429.6;
	peer.seekCommand(u"433.518664"_q);
	peer.highResolutionSeek(433.518664);
	peer.seek(433.518664, std::nullopt);
	CheckReload(peer, 433.518664);
}

void TestBackwardSeekWaitsInNewView() {
	auto peer = Peer();
	StartIndexedPlayback(peer);
	peer.indexReady = false;
	peer.finishReloads = false;
	peer.seekCommand(u"166.583561"_q);
	peer.highResolutionSeek(166.583561);
	peer.seek(166.583561, std::nullopt);
	Check(WaitUntil([&] { return peer.reloads.size() == 2; }, 150),
		"an impossible backward seek leaves the old view before its wrong restart");
	Check(peer.requests.back() == 166583,
		"the waiting view belongs to the original backward target");
	Check(peer.settled == 1, "waiting for metadata does not complete the new seek");
	peer.indexReady = true;
	peer.completeReload();
	Check(WaitUntil([&] { return peer.settled == 2; }),
		"the backward seek completes only after its new view starts");
	Drain();
	Check(peer.reloads.size() == 2, "the completed waiting view is not reloaded again");
}

void TestNewTargetDuringIndexLoading() {
	auto peer = Peer();
	StartIndexedPlayback(peer);
	peer.indexReady = false;
	peer.finishReloads = false;
	peer.seekCommand(u"166.583561"_q);
	peer.highResolutionSeek(166.583561);
	peer.seek(166.583561, std::nullopt);
	Check(WaitUntil([&] { return peer.reloads.size() == 2; }),
		"the backward view is opening while metadata is pending");
	peer.property(u"pause"_q, false);
	peer.seekCommand(u"194.681993"_q);
	Check(WaitUntil([&] { return peer.reloads.size() == 3; }),
		"a command during owned loading replaces the pending view");
	peer.seekCommand(u"194.681993"_q, u"absolute+keyframes"_q);
	Drain();
	Check(peer.requests == std::vector<std::int64_t>{ 433518, 166583, 194681 },
		"a repeated loading command does not cancel or rebuild its index");
	Check(peer.reloads.size() == 3,
		"a repeated loading command does not replace the same pending view");
	Check(!peer.paused(),
		"resume during loading is preserved by the replacement view");
	peer.indexReady = true;
	peer.completeReload();
	Check(WaitUntil([&] { return peer.settled == 2; }),
		"only the latest loading target completes");
	Check(peer.abandoned == 0, "replacing a pending view keeps the controller");
}

void TestPauseChangesDuringIndexLoading() {
	for (const auto paused : { false, true }) {
		auto peer = Peer();
		peer.property(u"pause"_q, !paused);
		StartIndexedPlayback(peer);
		peer.indexReady = false;
		peer.finishReloads = false;
		peer.seekCommand(u"166.583561"_q);
		peer.highResolutionSeek(166.583561);
		peer.seek(166.583561, std::nullopt);
		Check(WaitUntil([&] { return peer.reloads.size() == 2; }),
			"the backward view waits before applying file options");
		peer.property(u"pause"_q, paused);
		Drain();
		peer.indexReady = true;
		peer.completeReload();
		Check(WaitUntil([&] { return peer.settled == 2; }),
			"the waiting view completes after the pause change");
		Check(peer.paused() == paused,
			"late file options cannot overwrite pause changes during loading");
	}
}

void TestEofPauseBeforeReload() {
	for (const auto paused : { false, true }) {
		auto peer = Peer();
		peer.property(u"pause"_q, paused);
		Drain();
		peer.indexReady = false;
		peer.seekCommand(u"760.000000"_q);
		peer.highResolutionSeek(760.);
		peer.seek(760., std::nullopt);
		peer.property(u"eof-reached"_q, true);
		peer.property(u"pause"_q, true);
		Drain();
		peer.indexReady = true;
		CheckReload(peer, 760.);
		Check(peer.paused() == paused,
			"reload clears automatic EOF pause while preserving user pause");
	}
}

void TestDelayedReloadCompletion() {
	auto peer = Peer();
	peer.restartReloads = false;
	peer.seekCommand(u"760.000000"_q);
	peer.highResolutionSeek(760.);
	peer.seek(760., std::nullopt);
	Check(WaitUntil([&] { return peer.reloads.size() == 1; }),
		"the indexed file opens before its startup seek completes");
	Drain();
	peer.delayNextPositionReply = true;
	peer.event(u"playback-restart"_q);
	peer.property(u"seeking"_q, false);
	peer.restartLog(760.);
	Check(WaitUntil([&] { return peer.delayedPositionReply.has_value(); }),
		"the reload completion awaits a position reply");
	Drain();
	Check(peer.reloads.size() == 1,
		"a delayed completion reply cannot reload the same target twice");
	peer.releasePositionReply(760.);
	Check(WaitUntil([&] { return peer.settled == 1; }),
		"the delayed position reply completes the original reload");
}

void TestNewTargetBeforeReloadRestart() {
	auto peer = Peer();
	peer.restartReloads = false;
	peer.seekCommand(u"349.223368"_q);
	peer.highResolutionSeek(349.223368);
	peer.seek(349.223368, std::nullopt);
	Check(WaitUntil([&] { return peer.reloads.size() == 1; }),
		"the first reload opens without finishing its startup seek");
	Drain();
	peer.indexReady = false;
	peer.seekCommand(u"606.123317"_q);
	peer.highResolutionSeek(606.123317);
	peer.seek(606.123317, std::nullopt);
	Check(WaitUntil([&] { return peer.requests.size() == 2; }),
		"a new target prepares before the old reload restarts playback");
	Check(peer.requests.back() == 606123, "the new reload prepares the latest target");
	peer.indexReady = true;
	peer.restartReloads = true;
	Check(WaitUntil([&] { return peer.reloads.size() == 2; }),
		"the ready target replaces the old unfinished reload");
	Check(WaitUntil([&] { return peer.settled == 1; }),
		"the replacement reload completes at the latest target");
}

void TestRelativeSeekDuringIndexLoading() {
	auto peer = Peer();
	StartIndexedPlayback(peer);
	peer.indexReady = false;
	peer.finishReloads = false;
	peer.seekCommand(u"200.000000"_q);
	peer.highResolutionSeek(200.);
	peer.seek(200., std::nullopt);
	Check(WaitUntil([&] { return peer.reloads.size() == 2; }),
		"a known target is waiting for its index");
	peer.seekCommand(u"-5"_q, u"relative+exact"_q);
	Check(WaitUntil([&] { return peer.reloads.size() == 3; }),
		"relative loading seeks use the pending target as their base");
	peer.seekCommand(u"20"_q, u"relative-percent+keyframes"_q);
	Check(WaitUntil([&] { return peer.reloads.size() == 4; }),
		"relative percent loading seeks retain the known duration");
	peer.seekCommand(u"1"_q, u"exact"_q);
	Check(WaitUntil([&] { return peer.reloads.size() == 5; }),
		"the default relative mode also works while loading");
	peer.seekCommand(u"100"_q, u"unsupported"_q);
	Drain();
	Check(peer.requests == std::vector<std::int64_t>{
		433518, 200000, 195000, 364000, 365000,
	}, "resolved relative targets replace the index without guessing unknown modes");
	Check(peer.reloads.size() == 5, "an unknown loading command leaves the pending view intact");
	peer.indexReady = true;
	peer.completeReload();
	Check(WaitUntil([&] { return peer.settled == 2; }),
		"the latest relative target completes after loading");
}

void TestCachedBackwardInsideView() {
	auto peer = Peer();
	StartIndexedPlayback(peer);
	peer.seekCommand(u"431.000000"_q, u"absolute+keyframes"_q);
	peer.seek(431., 430.);
	peer.restartLog(430.);
	Drain();
	Check(peer.requests.size() == 1 && peer.reloads.size() == 1,
		"a cached backward keyframe inside the view uses native seeking");
}

void TestExecutedRelativeSeekBeforeView() {
	auto peer = Peer();
	StartIndexedPlayback(peer);
	peer.indexReady = false;
	peer.finishReloads = false;
	peer.seekCommand(u"-200"_q, u"relative+exact"_q);
	peer.highResolutionSeek(233.518664);
	peer.seek(233.518664, std::nullopt, false);
	Check(WaitUntil([&] { return peer.reloads.size() == 2; }, 150),
		"an executed relative seek before the view starts waiting immediately");
	Check(peer.requests.back() == 233518,
		"the relative seek uses its executed target instead of the prior position");
	peer.indexReady = true;
	peer.completeReload();
	Check(WaitUntil([&] { return peer.settled == 2; }),
		"the executed relative target completes without a stale fallback");
}

void TestSwitchWhileIndexLoading() {
	auto peer = Peer();
	StartIndexedPlayback(peer);
	peer.indexReady = false;
	peer.finishReloads = false;
	peer.seekCommand(u"166.583561"_q);
	peer.highResolutionSeek(166.583561);
	peer.seek(166.583561, std::nullopt);
	Check(WaitUntil([&] { return peer.reloads.size() == 2; }),
		"the old file has a pending index view");
	peer.property(u"path"_q, u"another-file.mp4"_q);
	Check(WaitUntil([&] { return peer.abandoned == 1; }),
		"switching files cancels the pending index controller");
	peer.indexReady = true;
	Drain();
	Check(peer.reloads.size() == 2, "late metadata cannot reload the old file");
}

void TestStopCommandsDuringIndexLoading() {
	for (const auto &command : { u"stop"_q, u"quit"_q }) {
		auto peer = Peer();
		StartIndexedPlayback(peer);
		peer.indexReady = false;
		peer.finishReloads = false;
		peer.seekCommand(u"166.583561"_q);
		peer.highResolutionSeek(166.583561);
		peer.seek(166.583561, std::nullopt);
		Check(WaitUntil([&] { return peer.reloads.size() == 2; }),
			"the index view is loading before the stop command");
		peer.log(u"Run command: %1, flags=64, args=[]\n"_q.arg(command));
		Check(WaitUntil([&] { return peer.abandoned == 1; }),
			"explicit stop commands cancel the pending index view");
		peer.indexReady = true;
		Drain();
		Check(peer.reloads.size() == 2,
			"late metadata cannot restart explicitly stopped playback");
	}
}

} // namespace

int main(int argc, char *argv[]) {
	auto application = QCoreApplication(argc, argv);
	TestRepeatedPendingTarget();
	TestBackwardSnapshot();
	TestCachedSnapshot();
	TestLatestSnapshot();
	TestQueryFallback();
	TestBeginningSnapshot();
	TestBackwardLateLogs();
	TestCachedLateLogs();
	TestCachedLateLogsAfterPlaybackAdvances();
	TestOldPositionReplyAfterLoggedCompletion();
	TestCachedKeyframeLanding();
	TestLogsBeforeSeekEvent();
	TestExecutedLogTarget();
	TestLatestSeekCommand();
	TestBackwardKeyframeCommand();
	TestForwardKeyframeSnapshotWithoutRestart();
	TestUnexecutedSeekCommands();
	TestPendingCommandBlocksOldTarget();
	TestPositionPropertyReplacesLoggedTarget();
	TestExecutedSubSeekReplacesLoggedTarget();
	TestUnexecutedOtherSeekRetainsLoggedTarget();
	TestUnrelatedPropertyLogsRetainLoggedTarget();
	TestPlaybackStartClearsUnexecutedCommand();
	TestLargeLogBatch();
	TestUnknownLogFormat();
	TestNonFiniteLogTarget();
	TestWrongLogPrefix();
	TestAutomaticSeekLogs();
	TestLoadfileClearsCommand();
	TestNewFileClearsCommand();
	TestUnsupportedCommandClearsCandidate();
	TestBackwardSeekWaitsInNewView();
	TestNewTargetDuringIndexLoading();
	TestPauseChangesDuringIndexLoading();
	TestEofPauseBeforeReload();
	TestDelayedReloadCompletion();
	TestNewTargetBeforeReloadRestart();
	TestRelativeSeekDuringIndexLoading();
	TestCachedBackwardInsideView();
	TestExecutedRelativeSeekBeforeView();
	TestSwitchWhileIndexLoading();
	TestStopCommandsDuringIndexLoading();
	std::cout << "MPV index reload checks passed: " << Checks << '\n';
	return 0;
}
