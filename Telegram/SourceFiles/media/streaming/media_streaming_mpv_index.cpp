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
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QUuid>
#include <QtNetwork/QLocalSocket>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace Media::Streaming::Mpv {

namespace {

constexpr auto kSeekGrace = std::chrono::milliseconds(200);
constexpr auto kRestoreTimeout = std::chrono::milliseconds(1000);

class IndexReload final : public QObject {
public:
	IndexReload(
		QProcess *process,
		IndexControl control,
		std::int64_t durationMs);

private:
	void poll();
	void connected();
	void receive();
	void handle(const QJsonObject &message);
	void request(QJsonArray command, int id = 0);
	void queryPosition();
	void checkCompletedSeek();
	void updatePause(bool paused);
	void seekTo(double position);
	[[nodiscard]] bool owns(const QString &path) const;
	[[nodiscard]] bool canReload(const QString &path) const;
	void reload();
	void stop(bool abandoned = false);

	IndexControl _control;
	QString _url;
	QString _endpoint;
	QLocalSocket _socket;
	QTimer _timer;
	QByteArray _buffer;
	QString _path;
	QJsonValue _originalIdle;
	double _position = -1.;
	double _target = -1.;
	double _duration = -1.;
	double _expectedDuration = 0.;
	int _connectAttempts = 0;
	int _querySequence = 100;
	int _positionQuery = -1;
	int _completionQuery = -1;
	int _pauseQuery = -1;
	std::uint64_t _revision = 0;
	std::uint64_t _reloadRevision = 0;
	std::uint64_t _seekSerial = 0;
	std::uint64_t _reloadSerial = 0;
	std::uint64_t _completionSerial = 0;
	std::uint64_t _pauseSerial = 0;
	std::chrono::steady_clock::time_point _seekStarted;
	bool _paused = false;
	bool _seekPaused = false;
	bool _seeking = false;
	bool _loaded = false;
	bool _played = false;
	bool _reloading = false;
	bool _onDemandReload = false;
	bool _queryDuringReload = false;
	bool _queryRestarted = false;
	bool _positionOutstanding = false;
	bool _idleKnown = false;
	bool _idleGuarded = false;
	bool _ended = false;
	bool _legacyLoadfile = false;
	bool _stopped = false;

};

IndexReload::IndexReload(
		QProcess *process,
		IndexControl control,
		std::int64_t durationMs)
: QObject(process)
, _control(std::move(control))
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
	const auto state = _control.state();
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
	} else if (state == IndexState::OnDemand
		&& (_loaded || _ended)
		&& _idleKnown
		&& !_reloading
		&& _target >= 0.
		&& std::chrono::steady_clock::now() - _seekStarted >= kSeekGrace
		&& canReload(_path)) {
		if (!_revision) {
			_revision = _control.request(std::int64_t(_target * 1000));
		}
		if (_revision && _control.ready(_revision)) {
			_reloading = true;
			_onDemandReload = true;
			_reloadRevision = _revision;
			_reloadSerial = _seekSerial;
			_position = _target;
			request({ u"get_property"_q, u"path"_q }, 2);
		}
	} else if (state == IndexState::Ready
		&& !_reloading
		&& (_loaded || _ended)
		&& (_seeking || _target >= 0. || unknownDuration)
		&& canReload(_path)) {
		_reloading = true;
		_onDemandReload = false;
		if (_target >= 0.) {
			_position = _target;
			request({ u"get_property"_q, u"path"_q }, 2);
		} else {
			request({ u"get_property"_q, u"time-pos"_q }, 1);
		}
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
	request({ u"get_property"_q, u"idle"_q }, 8);
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

void IndexReload::queryPosition() {
	if (_querySequence == std::numeric_limits<int>::max()) {
		stop(true);
		return;
	}
	_queryDuringReload = _reloading;
	_queryRestarted = false;
	_positionOutstanding = true;
	if (!_reloading && _target < 0.) {
		_seekPaused = _paused;
	}
	_positionQuery = ++_querySequence;
	request({ u"get_property"_q, u"time-pos"_q }, _positionQuery);
}

void IndexReload::updatePause(bool paused) {
	_paused = paused;
	if (!paused || (_target < 0. && !_seeking && !_reloading)) {
		_seekPaused = paused;
	} else if (_querySequence < std::numeric_limits<int>::max()) {
		_pauseSerial = _seekSerial;
		_pauseQuery = ++_querySequence;
		request({ u"get_property"_q, u"eof-reached"_q }, _pauseQuery);
	}
}

void IndexReload::checkCompletedSeek() {
	if (_target >= 0. && _querySequence < std::numeric_limits<int>::max()) {
		_completionSerial = _seekSerial;
		_completionQuery = ++_querySequence;
		request({ u"get_property"_q, u"time-pos"_q }, _completionQuery);
	}
}

void IndexReload::seekTo(double position) {
	if (!std::isfinite(position) || position < 0.
		|| position >= double(std::numeric_limits<std::int64_t>::max() / 1000)
		|| (!_played && position <= 0.)) {
		return;
	} else if (_queryDuringReload && std::abs(position - _position) < 0.1) {
		return;
	} else if (_target >= 0. && std::abs(position - _target) < 0.001) {
		return;
	}
	_target = position;
	_revision = 0;
	_seekStarted = std::chrono::steady_clock::now();
	++_seekSerial;
}

bool IndexReload::owns(const QString &path) const {
	if (path == _url) {
		return true;
	}
	auto parsed = QUrl(path);
	auto query = QUrlQuery(parsed);
	if (!query.hasQueryItem(u"tdesktop_index"_q)) {
		return false;
	}
	query.removeAllQueryItems(u"tdesktop_index"_q);
	parsed.setQuery(query);
	return parsed.toString(QUrl::RemoveQuery) == _url && query.isEmpty();
}

bool IndexReload::canReload(const QString &path) const {
	return owns(path) || (_ended && path.isEmpty());
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
			if (!_path.isEmpty() && !owns(_path)) {
				stop(true);
			}
		} else if (name == u"pause"_q) {
			updatePause(data.toBool());
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
		_ended = false;
	} else if (event == u"end-file"_q && _idleGuarded) {
		const auto reason = message.value(u"reason"_q).toString();
		if (_reloading) {
			if (reason == u"eof"_q || reason == u"error"_q) {
				stop(true);
			}
		} else if ((_target >= 0. || _positionOutstanding)
			&& (reason == u"eof"_q || reason == u"error"_q)) {
			_ended = true;
			_loaded = false;
		} else {
			stop(true);
		}
	} else if (event == u"seek"_q) {
		queryPosition();
	} else if (event == u"playback-restart"_q) {
		_played = true;
		_queryRestarted = true;
		if (_onDemandReload && _reloading) {
			_reloading = false;
		}
		checkCompletedSeek();
	} else if (message.value(u"request_id"_q).toInt() == _positionQuery) {
		_positionOutstanding = false;
		const auto data = message.value(u"data"_q);
		if (data.isDouble()) {
			seekTo(data.toDouble());
			if (_queryRestarted) {
				checkCompletedSeek();
			}
		}
	} else if (message.value(u"request_id"_q).toInt() == _completionQuery) {
		const auto data = message.value(u"data"_q);
		if (_completionSerial == _seekSerial
			&& data.isDouble()
			&& !_seeking
			&& !_reloading
			&& _target >= 0.
			&& std::abs(data.toDouble() - _target) < 0.2) {
			_target = -1.;
			_revision = 0;
			_control.settled();
		}
	} else if (message.value(u"request_id"_q).toInt() == _pauseQuery) {
		const auto data = message.value(u"data"_q);
		if (_pauseSerial == _seekSerial && data.isBool() && !data.toBool()) {
			_seekPaused = _paused;
		}
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
		if (!canReload(message.value(u"data"_q).toString())) {
			stop(true);
			return;
		} else if (_onDemandReload && _reloadSerial != _seekSerial) {
			_reloading = false;
			return;
		}
		reload();
	} else if (message.value(u"request_id"_q).toInt() == 3) {
		const auto failed = message.value(u"error"_q).toString() != u"success"_q;
		if (!_onDemandReload || failed) {
			stop(failed);
		} else if (_reloadSerial == _seekSerial) {
			_target = -1.;
			_revision = 0;
		}
	} else if (message.value(u"request_id"_q).toInt() == 4) {
		const auto version = message.value(u"data"_q).toString();
		const auto marker = version.indexOf(u"0."_q);
		if (marker >= 0) {
			const auto minor = version.mid(marker + 2).section('.', 0, 0).toInt();
			_legacyLoadfile = (minor > 0 && minor < 38);
		}
	} else if (message.value(u"request_id"_q).toInt() == 8) {
		_originalIdle = message.value(u"data"_q);
		_idleKnown = !_originalIdle.isNull() && !_originalIdle.isUndefined();
	} else if (message.value(u"request_id"_q).toInt() == 9) {
		if (message.value(u"error"_q).toString() != u"success"_q) {
			stop(true);
		} else if (_reloadSerial != _seekSerial) {
			_reloading = false;
		} else {
			reload();
		}
	}
}

void IndexReload::reload() {
	if (_onDemandReload && !_idleGuarded) {
		_idleGuarded = true;
		request({ u"set_property"_q, u"idle"_q, u"yes"_q }, 9);
		return;
	}
	_ended = false;
	auto url = QUrl(_url);
	if (_onDemandReload) {
		auto query = QUrlQuery(url);
		query.addQueryItem(u"tdesktop_index"_q, QString::number(_reloadRevision));
		url.setQuery(query);
	}
	auto command = QJsonArray{ u"loadfile"_q, url.toString(), u"replace"_q };
	if (!_legacyLoadfile) {
		command.append(-1);
	}
	command.append(QJsonObject{
		{ u"start"_q, QString::number(std::max(0., _position), 'f', 6) },
		{ u"pause"_q, (_onDemandReload ? _seekPaused : _paused) ? u"yes"_q : u"no"_q },
		{ u"demuxer-lavf-o"_q, PlaybackDemuxerOptions(_onDemandReload) },
	});
	request(std::move(command), 3);
}

void IndexReload::stop(bool abandoned) {
	if (_stopped) {
		return;
	}
	_stopped = true;
	_timer.stop();
	if (abandoned) {
		_control.abandoned();
	}
	if (_idleGuarded && _idleKnown
		&& _socket.state() == QLocalSocket::ConnectedState) {
		connect(&_socket, &QLocalSocket::disconnected, this, &QObject::deleteLater);
		request({ u"set_property"_q, u"idle"_q, _originalIdle });
		_socket.disconnectFromServer();
		QTimer::singleShot(kRestoreTimeout, this, &QObject::deleteLater);
	} else {
		_socket.abort();
		deleteLater();
	}
}

} // namespace

void ManageIndexReload(
		QProcess *process,
		IndexControl control,
		std::int64_t durationMs) {
	new IndexReload(
		process,
		std::move(control),
		durationMs);
}

QString PlaybackDemuxerOptions(bool fastOpen) {
	return fastOpen
		? u"ignore_editlist=1,fflags=+ignidx"_q
		: u"ignore_editlist=1"_q;
}

} // namespace Media::Streaming::Mpv
