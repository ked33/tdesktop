#pragma once
#include "base/flat_map.h"
#include "base/flat_set.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <cmath>
#include <limits>
#include <optional>
#include <rpl/variable.h>
#include <type_traits>
#include <vector>

namespace Storage::SettingsBackup {

template <typename T, typename Enable = void> struct Codec;

template <typename T>
struct Codec<T, std::enable_if_t<std::is_arithmetic_v<T>>> {
	static QJsonValue Encode(T value) {
		if constexpr (std::is_same_v<T, bool>) {
			return value;
		} else if constexpr (sizeof(T) >= 8 && std::is_integral_v<T>) {
			return QString::number(value);
		} else {
			return double(value);
		}
	}
	static bool Decode(const QJsonValue &json, T &value) {
		if constexpr (std::is_same_v<T, bool>) {
			if (!json.isBool()) {
				return false;
			}
			value = json.toBool();
		} else if constexpr (sizeof(T) >= 8 && std::is_integral_v<T>) {
			if (!json.isString()) {
				return false;
			}
			auto ok = false;
			if constexpr (std::is_signed_v<T>) {
				value = json.toString().toLongLong(&ok);
			} else {
				value = json.toString().toULongLong(&ok);
			}
			if (!ok || QString::number(value) != json.toString()) {
				return false;
			}
		} else {
			if (!json.isDouble()) {
				return false;
			}
			const auto number = json.toDouble();
			if (!std::isfinite(number)
			    || number < std::numeric_limits<T>::lowest()
			    || number > std::numeric_limits<T>::max()) {
				return false;
			}
			if constexpr (std::is_integral_v<T>) {
				if (std::trunc(number) != number) {
					return false;
				}
			}
			value = T(number);
		}
		return true;
	}
};

template <typename T> struct Codec<T, std::enable_if_t<std::is_enum_v<T>>> {
	static QJsonValue Encode(T value) {
		return int(value);
	}
	static bool Decode(const QJsonValue &json, T &value) {
		auto number = int();
		if (!Codec<int>::Decode(json, number)) {
			return false;
		}
		value = T(number);
		return true;
	}
};

template <> struct Codec<QString> {
	static QJsonValue Encode(const QString &value) {
		return value;
	}
	static bool Decode(const QJsonValue &json, QString &value) {
		if (!json.isString() || json.toString().size() > 65536) {
			return false;
		}
		value = json.toString();
		return true;
	}
};

template <> struct Codec<QByteArray> {
	static QJsonValue Encode(const QByteArray &value) {
		return QString::fromLatin1(value.toBase64());
	}
	static bool Decode(const QJsonValue &json, QByteArray &value) {
		if (!json.isString() || json.toString().size() > 1024 * 1024) {
			return false;
		}
		const auto text = json.toString().toLatin1();
		value = QByteArray::fromBase64(text);
		return value.toBase64() == text;
	}
};

template <typename T> struct Codec<std::optional<T>> {
	static QJsonValue Encode(const std::optional<T> &value) {
		return value ? Codec<T>::Encode(*value) : QJsonValue::Null;
	}
	static bool Decode(const QJsonValue &json, std::optional<T> &value) {
		if (json.isNull()) {
			value.reset();
			return true;
		}
		auto parsed = T();
		if (!Codec<T>::Decode(json, parsed)) {
			return false;
		}
		value = std::move(parsed);
		return true;
	}
};

template <typename T> struct Codec<std::vector<T>> {
	static QJsonValue Encode(const std::vector<T> &values) {
		auto result = QJsonArray();
		for (const auto &value : values) {
			result.push_back(Codec<T>::Encode(value));
		}
		return result;
	}
	static bool Decode(const QJsonValue &json, std::vector<T> &values) {
		if (!json.isArray() || json.toArray().size() > 10000) {
			return false;
		}
		values.clear();
		for (const auto &entry : json.toArray()) {
			auto value = T();
			if (!Codec<T>::Decode(entry, value)) {
				return false;
			}
			values.push_back(std::move(value));
		}
		return true;
	}
};

template <> struct Codec<QJsonObject> {
	static QJsonValue Encode(const QJsonObject &value) {
		return value;
	}
	static bool Decode(const QJsonValue &json, QJsonObject &value) {
		if (!json.isObject() || json.toObject().size() > 10000) {
			return false;
		}
		value = json.toObject();
		return true;
	}
};

template <typename T> struct Codec<base::flat_set<T>> {
	static QJsonValue Encode(const base::flat_set<T> &value) {
		return Codec<std::vector<T>>::Encode(
		    std::vector<T>(value.begin(), value.end()));
	}
	static bool Decode(const QJsonValue &json, base::flat_set<T> &value) {
		auto entries = std::vector<T>();
		if (!Codec<std::vector<T>>::Decode(json, entries)) {
			return false;
		}
		value.clear();
		for (auto &entry : entries) {
			value.emplace(std::move(entry));
		}
		return true;
	}
};

template <typename T> struct Codec<base::flat_map<QString, T>> {
	static QJsonValue Encode(const base::flat_map<QString, T> &value) {
		auto result = QJsonObject();
		for (const auto &[key, entry] : value) {
			result.insert(key, Codec<T>::Encode(entry));
		}
		return result;
	}
	static bool Decode(
	    const QJsonValue &json, base::flat_map<QString, T> &value) {
		if (!json.isObject() || json.toObject().size() > 10000) {
			return false;
		}
		value.clear();
		const auto object = json.toObject();
		for (auto i = object.begin(); i != object.end(); ++i) {
			auto entry = T();
			if (i.key().size() > 65536 || !Codec<T>::Decode(*i, entry)) {
				return false;
			}
			value.emplace(i.key(), std::move(entry));
		}
		return true;
	}
};

class Archive final {
public:
	Archive() = default;
	Archive(QJsonObject incoming, bool apply)
	    : _object(std::move(incoming)), _reading(true), _apply(apply) {}

	template <typename T, typename Validator>
	void field(const char* key, T &target, Validator validate) {
		const auto name = QString::fromLatin1(key);
		if (!_reading) {
			_object.insert(name, Codec<T>::Encode(target));
		} else if (_object.contains(name)) {
			auto parsed = T();
			if (!Codec<T>::Decode(_object.value(name), parsed)
			    || !validate(parsed)) {
				_ok = false;
			} else if (_apply) {
				target = std::move(parsed);
			}
		}
	}
	template <typename T> void field(const char* key, T &target) {
		field(key, target, [](const T &) { return true; });
	}
	template <typename T, typename Validator>
	void field(const char* key, rpl::variable<T> &target, Validator validate) {
		auto value = target.current();
		field(key, value, validate);
		if (_apply && _ok && contains(key)) {
			target = std::move(value);
		}
	}
	template <typename T>
	void field(const char* key, rpl::variable<T> &target) {
		field(key, target, [](const T &) { return true; });
	}
	template <typename T, typename Min, typename Max>
	void bounded(const char* key, T &target, Min min, Max max) {
		field(key, target, [=](const auto &value) {
			return double(value) >= double(min) && double(value) <= double(max);
		});
	}
	[[nodiscard]] bool contains(const char* key) const {
		return _object.contains(QString::fromLatin1(key));
	}
	[[nodiscard]] bool applying() const {
		return _apply;
	}
	[[nodiscard]] bool ok() const {
		return _ok;
	}
	[[nodiscard]] QJsonObject object() const {
		return _object;
	}

private:
	QJsonObject _object;
	bool _reading = false;
	bool _apply = false;
	bool _ok = true;

};

} // namespace Storage::SettingsBackup
