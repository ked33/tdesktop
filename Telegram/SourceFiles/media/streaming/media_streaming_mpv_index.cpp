/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_mpv_index.h"

#include <QtCore/QDir>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtNetwork/QLocalSocket>

#include <algorithm>
#include <cmath>

namespace Media::Streaming::Mpv {

namespace {

class IndexReload final : public QObject {
public:
	IndexReload(
		QProcess *process,
		std::function<IndexState()> state,
		std::int64_t durationMs,
		std::function<void()> abandoned);

private:
	void poll();
	void connected();
	void receive();
	void handle(const QJsonObject &message);
	void request(QJsonArray command, int id = 0);
	void reload();
	void stop(bool abandoned = false);

	std::function<IndexState()> _state;
	std::function<void()> _abandoned;
	QString _url;
	QString _endpoint;
	QLocalSocket _socket;
	QTimer _timer;
	QByteArray _buffer;
	QString _path;
	double _position = -1.;
	double _duration = -1.;
	double _expectedDuration = 0.;
	int _connectAttempts = 0;
	bool _paused = false;
	bool _seeking = false;
	bool _loaded = false;
	bool _played = false;
	bool _reloading = false;
	bool _legacyLoadfile = false;
	bool _stopped = false;

};

IndexReload::IndexReload(
		QProcess *process,
		std::function<IndexState()> state,
		std::int64_t durationMs,
		std::function<void()> abandoned)
: QObject(process)
, _state(std::move(state))
, _abandoned(std::move(abandoned))
, _url(process->arguments().back())
, _expectedDuration(durationMs / 1000.) {
	const auto name = u"tdesktop-mpv-"_q
		+ QUuid::createUuid().toString(QUuid::WithoutBraces);
#ifdef Q_OS_WIN
	_endpoint = u"\\\\.\\pipe\\"_q + name;
#else // Q_OS_WIN
	_endpoint = QDir::tempPath() + '/' + name;
#endif // !Q_OS_WIN
	auto arguments = process->arguments();
	arguments.insert(
		arguments.size() - 1,
		u"--input-ipc-server="_q + _endpoint);
	process->setArguments(arguments);
	connect(&_socket, &QLocalSocket::connected, this, [=] {
		connected();
	});
	connect(&_socket, &QLocalSocket::readyRead, this, [=] {
		receive();
	});
	connect(&_timer, &QTimer::timeout, this, [=] {
		poll();
	});
	_timer.start(100);
}

void IndexReload::poll() {
	const auto state = _state();
	const auto unknownDuration = _played
		&& (_expectedDuration <= 0. || _duration + 1. < _expectedDuration);
	if (state == IndexState::Unavailable) {
		stop();
	} else if (_socket.state() == QLocalSocket::UnconnectedState) {
		if (++_connectAttempts > 100) {
			stop(true);
			return;
		}
		_socket.connectToServer(_endpoint);
	} else if (state == IndexState::Ready
		&& !_reloading
		&& _loaded
		&& (_seeking || unknownDuration)
		&& _path == _url) {
		_reloading = true;
		request({ u"get_property"_q, u"time-pos"_q }, 1);
	}
}

void IndexReload::connected() {
	const auto properties = {
		u"path"_q,
		u"pause"_q,
		u"duration"_q,
		u"seeking"_q,
		u"idle-active"_q,
	};
	auto id = 0;
	for (const auto &name : properties) {
		request({ u"observe_property"_q, ++id, name });
	}
	request({ u"get_property"_q, u"mpv-version"_q }, 4);
}

void IndexReload::request(QJsonArray command, int id) {
	const auto data = QJsonDocument(QJsonObject{
		{ u"command"_q, command },
		{ u"request_id"_q, id },
	}).toJson(QJsonDocument::Compact) + '\n';
	if (_socket.write(data) != data.size()) {
		stop(true);
	}
}

void IndexReload::receive() {
	_buffer += _socket.readAll();
	if (_buffer.size() > 1024 * 1024) {
		stop(true);
		return;
	}
	while (true) {
		const auto end = _buffer.indexOf('\n');
		if (end < 0) {
			return;
		}
		const auto line = _buffer.left(end);
		_buffer.remove(0, end + 1);
		handle(QJsonDocument::fromJson(line).object());
	}
}

void IndexReload::handle(const QJsonObject &message) {
	if (_stopped) {
		return;
	}
	const auto event = message.value(u"event"_q).toString();
	if (event == u"property-change"_q) {
		const auto name = message.value(u"name"_q).toString();
		const auto data = message.value(u"data"_q);
		if (name == u"path"_q) {
			_path = data.toString();
			if (!_path.isEmpty() && _path != _url) {
				stop(true);
			}
		} else if (name == u"pause"_q) {
			_paused = data.toBool();
		} else if (name == u"duration"_q) {
			_duration = data.toDouble(-1.);
		} else if (name == u"seeking"_q) {
			_seeking = data.toBool();
			_played = _played || (_loaded && !_seeking);
		} else if (name == u"idle-active"_q && data.isBool()) {
			_loaded = !data.toBool();
			_played = _played || (_loaded && !_seeking && _duration > 0.);
		}
	} else if (event == u"file-loaded"_q) {
		_loaded = true;
	} else if (event == u"playback-restart"_q) {
		_played = true;
	} else if (message.value(u"request_id"_q).toInt() == 1) {
		const auto data = message.value(u"data"_q);
		if (!data.isDouble() || !std::isfinite(data.toDouble())) {
			_reloading = false;
			return;
		}
		_position = data.toDouble();
		if (!_played && _position <= 0.) {
			_reloading = false;
			return;
		}
		request({ u"get_property"_q, u"path"_q }, 2);
	} else if (message.value(u"request_id"_q).toInt() == 2) {
		if (message.value(u"data"_q).toString() != _url) {
			stop(true);
			return;
		}
		reload();
	} else if (message.value(u"request_id"_q).toInt() == 3) {
		stop(message.value(u"error"_q).toString() != u"success"_q);
	} else if (message.value(u"request_id"_q).toInt() == 4) {
		const auto version = message.value(u"data"_q).toString();
		const auto marker = version.indexOf(u"0."_q);
		if (marker >= 0) {
			const auto minor = version.mid(marker + 2).section('.', 0, 0).toInt();
			_legacyLoadfile = (minor > 0 && minor < 38);
		}
	}
}

void IndexReload::reload() {
	auto command = QJsonArray{ u"loadfile"_q, _url, u"replace"_q };
	if (!_legacyLoadfile) {
		command.append(-1);
	}
	command.append(QJsonObject{
		{ u"start"_q, QString::number(std::max(0., _position), 'f', 6) },
		{ u"pause"_q, _paused ? u"yes"_q : u"no"_q },
		{ u"demuxer-lavf-o"_q, PlaybackDemuxerOptions(false) },
	});
	request(std::move(command), 3);
}

void IndexReload::stop(bool abandoned) {
	if (_stopped) {
		return;
	}
	_stopped = true;
	_timer.stop();
	_socket.abort();
	if (abandoned) {
		_abandoned();
	}
	deleteLater();
}

} // namespace

void ManageIndexReload(
		QProcess *process,
		std::function<IndexState()> state,
		std::int64_t durationMs,
		std::function<void()> abandoned) {
	new IndexReload(
		process,
		std::move(state),
		durationMs,
		std::move(abandoned));
}

QString PlaybackDemuxerOptions(bool fastOpen) {
	return fastOpen
		? u"ignore_editlist=1,fflags=+ignidx"_q
		: u"ignore_editlist=1"_q;
}

} // namespace Media::Streaming::Mpv
