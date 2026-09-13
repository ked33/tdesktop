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

	std::vector<std::int64_t> requests;
	std::vector<QJsonObject> reloads;
	std::optional<QJsonObject> delayedPositionReply;
	int positionReplies = 0;
	int settled = 0;
	int abandoned = 0;
	bool delayNextPositionReply = false;

private:
	void receive();
	void handle(const QJsonObject &message);
	void send(QJsonObject message);

	QLocalServer _server;
	QLocalSocket *_socket = nullptr;
	QByteArray _buffer;
	QJsonObject _properties;
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
		.ready = [](std::uint64_t revision) { return revision != 0; },
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
		property(u"pause"_q, options.value(u"pause"_q).toString() == u"yes"_q);
		event(u"file-loaded"_q);
		const auto position = options.value(u"start"_q).toString().toDouble();
		highResolutionSeek(position);
		seek(position, position);
		restartLog(position);
		return;
	} else {
		Check(false, "controller command is supported by the fake MPV");
	}
	send(reply);
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
	Check(peer.settled == 0, "a wrong decoded position does not complete the seek");
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
	Check(peer.reloads.front().value(u"pause"_q).toString() == u"no"_q,
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
	Check(peer.settled == 0,
		"a query after execution logs cannot settle at the wrong landing");
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

} // namespace

int main(int argc, char *argv[]) {
	auto application = QCoreApplication(argc, argv);
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
	std::cout << "MPV index reload checks passed: " << Checks << '\n';
	return 0;
}
