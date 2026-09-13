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

	std::vector<std::int64_t> requests;
	std::vector<QJsonObject> reloads;
	int settled = 0;
	int abandoned = 0;

private:
	void receive();
	void handle(const QJsonObject &message);
	void send(QJsonObject message);
	void event(const QString &name);

	QLocalServer _server;
	QLocalSocket *_socket = nullptr;
	QByteArray _buffer;
	QJsonObject _properties;
	std::map<QString, int> _observed;
	std::unique_ptr<QProcess> _process;
	std::uint64_t _revision = 0;
	bool _idleRead = false;

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
	Check(WaitUntil([&] { return _idleRead; }), "controller initializes over IPC");
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
	if (name == u"observe_property"_q) {
		const auto propertyName = command[2].toString();
		_observed.emplace(propertyName, command[1].toInt());
		property(propertyName, _properties.value(propertyName));
	} else if (name == u"get_property"_q) {
		const auto propertyName = command[1].toString();
		reply.insert(u"data"_q, _properties.value(propertyName));
		_idleRead = _idleRead || (propertyName == u"idle"_q);
	} else if (name == u"set_property"_q) {
		property(command[1].toString(), command[2]);
	} else if (name == u"loadfile"_q) {
		const auto options = command.last().toObject();
		reloads.push_back(options);
		send(reply);
		send({
			{ u"event"_q, u"end-file"_q },
			{ u"reason"_q, u"stop"_q },
		});
		event(u"start-file"_q);
		property(u"path"_q, command[1]);
		property(u"pause"_q, options.value(u"pause"_q).toString() == u"yes"_q);
		event(u"file-loaded"_q);
		const auto position = options.value(u"start"_q).toString().toDouble();
		seek(position, position);
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

} // namespace

int main(int argc, char *argv[]) {
	auto application = QCoreApplication(argc, argv);
	TestBackwardSnapshot();
	TestCachedSnapshot();
	TestLatestSnapshot();
	TestQueryFallback();
	TestBeginningSnapshot();
	std::cout << "MPV index reload checks passed: " << Checks << '\n';
	return 0;
}
