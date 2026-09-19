/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/details/mtproto_dump_to_json.h"

#include "mtproto/details/mtproto_dump_to_text.h"
#include "scheme-dump_to_json.h"
#include "logs.h"

#include <cstring>
#include <zlib.h>

namespace MTP::details {
namespace {

struct JsonWriter {
	DumpToTextBuffer buffer;
	bool ok = true;
	bool write = true;

	JsonWriter &add(const char *data, int len = -1) {
		if (write && ok) {
			buffer.add(data, len);
		}
		return *this;
	}
	JsonWriter &add(const QString &data) {
		if (write && ok) {
			buffer.add(data);
		}
		return *this;
	}
	JsonWriter &addSpaces(int level) {
		if (write && ok) {
			buffer.addSpaces(level);
		}
		return *this;
	}
};

struct FlagValues {
	static constexpr auto kMax = 4;
	const char *names[kMax] = {};
	uint32 values[kMax] = {};
	int count = 0;

	void set(const char *name, uint32 value) {
		for (auto i = 0; i != count; ++i) {
			if (name && names[i] && !std::strcmp(names[i], name)) {
				values[i] = value;
				return;
			}
		}
		if (count < kMax) {
			names[count] = name;
			values[count] = value;
			++count;
		}
	}
	[[nodiscard]] bool present(const char *name, uint8 bit) const {
		if (!name) {
			return true;
		}
		for (auto i = 0; i != count; ++i) {
			if (names[i] && !std::strcmp(names[i], name)) {
				return (values[i] & (uint32(1) << bit)) != 0;
			}
		}
		return false;
	}
};

[[nodiscard]] bool IsUtf8(const QByteArray &bytes) {
	return QString::fromUtf8(bytes).toUtf8() == bytes;
}

void AddJsonString(JsonWriter &to, const QByteArray &bytes) {
	to.add("\"");
	for (const auto ch : bytes) {
		const auto u = static_cast<uchar>(ch);
		switch (ch) {
		case '\\': to.add("\\\\"); break;
		case '"': to.add("\\\""); break;
		case '\b': to.add("\\b"); break;
		case '\f': to.add("\\f"); break;
		case '\n': to.add("\\n"); break;
		case '\r': to.add("\\r"); break;
		case '\t': to.add("\\t"); break;
		default:
			if (u < 0x20) {
				auto encoded = QByteArray::number(u, 16);
				while (encoded.size() < 4) {
					encoded.prepend('0');
				}
				to.add("\\u").add(encoded.constData(), encoded.size());
			} else {
				const char raw[1] = { ch };
				to.add(raw, 1);
			}
			break;
		}
	}
	to.add("\"");
}

void AddJsonBase64(JsonWriter &to, const QByteArray &bytes) {
	const auto encoded = bytes.toBase64();
	to.add("\"").add(encoded.constData(), encoded.size()).add("\"");
}

void AddJsonNumber(JsonWriter &to, qint64 value) {
	to.add(QString::number(value));
}

void AddJsonNumber(JsonWriter &to, quint64 value) {
	to.add(QString::number(static_cast<qulonglong>(value)));
}

void Indent(JsonWriter &to, int level) {
	to.add("\n").addSpaces(level);
}

[[nodiscard]] bool DumpValue(
	JsonWriter &to,
	const mtpPrime *&from,
	const mtpPrime *end,
	JsonTlKind kind,
	JsonTlKind vectorInner,
	mtpTypeId bareId,
	int level);
[[nodiscard]] bool DumpBoxed(
	JsonWriter &to,
	const mtpPrime *&from,
	const mtpPrime *end,
	int level);
[[nodiscard]] bool DumpConstructor(
	JsonWriter &to,
	const mtpPrime *&from,
	const mtpPrime *end,
	const JsonTlConstructor *ctor,
	int level);

[[nodiscard]] bool DumpGzip(
		JsonWriter &to,
		const mtpPrime *&from,
		const mtpPrime *end,
		int level) {
	MTPstring packed;
	if (!packed.read(from, end)) {
		return false;
	}
	const auto packedLen = uint32(packed.v.size());
	const auto unpackedChunk = packedLen;
	mtpBuffer result;
	result.resize(0);

	z_stream stream;
	stream.zalloc = nullptr;
	stream.zfree = nullptr;
	stream.opaque = nullptr;
	stream.avail_in = 0;
	stream.next_in = nullptr;
	auto res = inflateInit2(&stream, 16 + MAX_WBITS);
	if (res != Z_OK) {
		return false;
	}
	stream.avail_in = packedLen;
	stream.next_in = reinterpret_cast<Bytef*>(packed.v.data());
	stream.avail_out = 0;
	while (!stream.avail_out) {
		result.resize(result.size() + unpackedChunk);
		stream.avail_out = unpackedChunk * sizeof(mtpPrime);
		stream.next_out = (Bytef*)&result[result.size() - unpackedChunk];
		res = inflate(&stream, Z_NO_FLUSH);
		if (res != Z_OK && res != Z_STREAM_END) {
			inflateEnd(&stream);
			return false;
		}
	}
	if (stream.avail_out & 0x03) {
		inflateEnd(&stream);
		return false;
	}
	result.resize(result.size() - (stream.avail_out >> 2));
	inflateEnd(&stream);
	if (result.empty()) {
		return false;
	}
	const mtpPrime *newFrom = result.constData();
	const mtpPrime *newEnd = result.constData() + result.size();
	return DumpBoxed(to, newFrom, newEnd, level);
}

bool DumpConstructor(
		JsonWriter &to,
		const mtpPrime *&from,
		const mtpPrime *end,
		const JsonTlConstructor *ctor,
		int level) {
	if (!ctor) {
		return false;
	}
	if (!std::strcmp(ctor->name, "boolTrue")
		|| !std::strcmp(ctor->name, "true")) {
		to.add("true");
		return true;
	} else if (!std::strcmp(ctor->name, "boolFalse")) {
		to.add("false");
		return true;
	} else if (!std::strcmp(ctor->name, "null")) {
		to.add("null");
		return true;
	}

	to.add("{");
	auto first = true;
	const auto emitKey = [&](const char *key) {
		if (!first) {
			to.add(",");
		}
		first = false;
		Indent(to, level + 1);
		AddJsonString(to, QByteArray(key));
		to.add(": ");
	};
	emitKey("_");
	AddJsonString(to, QByteArray(ctor->name));

	FlagValues flags;
	const auto fields = JsonTlFields();
	for (auto i = 0; i != ctor->fieldCount; ++i) {
		const auto &field = fields[ctor->firstField + i];
		if (!flags.present(field.flagName, field.flagBit)) {
			continue;
		}
		if (field.kind == JsonTlKind::Flags) {
			MTPint value;
			if (!value.read(from, end, mtpc_int)) {
				return false;
			}
			flags.set(field.name, uint32(value.v));
			continue;
		} else if (field.kind == JsonTlKind::True) {
			emitKey(field.name);
			to.add("true");
			continue;
		}
		emitKey(field.name);
		if (!DumpValue(
				to,
				from,
				end,
				field.kind,
				field.vectorInner,
				field.bareId,
				level + 1)) {
			return false;
		}
	}
	if (!first) {
		Indent(to, level);
	}
	to.add("}");
	return to.ok;
}

bool DumpBoxed(
		JsonWriter &to,
		const mtpPrime *&from,
		const mtpPrime *end,
		int level) {
	if (from >= end) {
		return false;
	}
	const auto cons = tl::Reader<mtpPrime>::Get(from, end);
	if (cons == mtpc_gzip_packed) {
		return DumpGzip(to, from, end, level);
	}
	const auto ctor = JsonTlConstructorById(cons);
	if (!ctor) {
		LOG(("JSON dump: unknown constructor 0x%1, remaining primes: %2"
			).arg(cons, 8, 16, QChar('0')
			).arg(int(end - from)));
		to.add("{\"_\":\"unknown\",\"id\":");
		AddJsonNumber(to, quint64(cons));
		to.add("}");
		return false;
	}
	return DumpConstructor(to, from, end, ctor, level);
}

bool DumpValue(
		JsonWriter &to,
		const mtpPrime *&from,
		const mtpPrime *end,
		JsonTlKind kind,
		JsonTlKind vectorInner,
		mtpTypeId bareId,
		int level) {
	switch (kind) {
	case JsonTlKind::Int: {
		MTPint value;
		if (!value.read(from, end, mtpc_int)) {
			return false;
		}
		AddJsonNumber(to, qint64(value.v));
		return true;
	}
	case JsonTlKind::Long: {
		MTPlong value;
		if (!value.read(from, end, mtpc_long)) {
			return false;
		}
		AddJsonNumber(to, quint64(value.v));
		return true;
	}
	case JsonTlKind::Double: {
		MTPdouble value;
		if (!value.read(from, end, mtpc_double)) {
			return false;
		}
		to.add(QString::number(value.v, 'g', 17));
		return true;
	}
	case JsonTlKind::String:
	case JsonTlKind::Bytes: {
		MTPstring value;
		if (!value.read(from, end, mtpc_string)) {
			return false;
		}
		if (kind == JsonTlKind::Bytes || !IsUtf8(value.v)) {
			AddJsonBase64(to, value.v);
		} else {
			AddJsonString(to, value.v);
		}
		return true;
	}
	case JsonTlKind::Int128: {
		MTPint128 value;
		if (!value.read(from, end, mtpc_int128)) {
			return false;
		}
		AddJsonString(
			to,
			(QString::number(static_cast<qulonglong>(value.h), 16)
				+ QString::number(static_cast<qulonglong>(value.l), 16)
			).toUtf8());
		return true;
	}
	case JsonTlKind::Int256: {
		MTPint256 value;
		if (!value.read(from, end, mtpc_int256)) {
			return false;
		}
		AddJsonString(
			to,
			(QString::number(static_cast<qulonglong>(value.h.h), 16)
				+ QString::number(static_cast<qulonglong>(value.h.l), 16)
				+ QString::number(static_cast<qulonglong>(value.l.h), 16)
				+ QString::number(static_cast<qulonglong>(value.l.l), 16)
			).toUtf8());
		return true;
	}
	case JsonTlKind::True:
		to.add("true");
		return true;
	case JsonTlKind::Boxed:
		return DumpBoxed(to, from, end, level);
	case JsonTlKind::Bare:
		return DumpConstructor(
			to,
			from,
			end,
			JsonTlConstructorById(bareId),
			level);
	case JsonTlKind::Vector: {
		if (from >= end) {
			return false;
		}
		const auto count = int32(*from++);
		if (count < 0) {
			return false;
		}
		to.add("[");
		if (!count) {
			to.add("]");
			return true;
		}
		for (auto i = 0; i != count; ++i) {
			if (i) {
				to.add(",");
			}
			Indent(to, level + 1);
			if (!DumpValue(
					to,
					from,
					end,
					vectorInner,
					JsonTlKind::Int,
					bareId,
					level + 1)) {
				return false;
			}
		}
		Indent(to, level);
		to.add("]");
		return to.ok;
	}
	case JsonTlKind::Flags:
	default:
		return false;
	}
}

[[nodiscard]] bool IsMessagesContainer(const char *name) {
	return !std::strcmp(name, "messages.messages")
		|| !std::strcmp(name, "messages.channelMessages")
		|| !std::strcmp(name, "messages.messagesSlice");
}

[[nodiscard]] bool DumpFirstMessage(
		JsonWriter &to,
		const mtpPrime *&from,
		const mtpPrime *end,
		const JsonTlConstructor *ctor,
		int level) {
	FlagValues flags;
	const auto fields = JsonTlFields();
	for (auto i = 0; i != ctor->fieldCount; ++i) {
		const auto &field = fields[ctor->firstField + i];
		if (!flags.present(field.flagName, field.flagBit)) {
			continue;
		}
		if (field.kind == JsonTlKind::Flags) {
			MTPint value;
			if (!value.read(from, end, mtpc_int)) {
				return false;
			}
			flags.set(field.name, uint32(value.v));
			continue;
		} else if (field.kind == JsonTlKind::True) {
			continue;
		}
		if (!std::strcmp(field.name, "messages")
			&& field.kind == JsonTlKind::Vector) {
			if (from >= end) {
				return false;
			}
			const auto count = int32(*from++);
			if (count <= 0) {
				return false;
			}
			return DumpBoxed(to, from, end, level);
		}
		auto skip = JsonWriter();
		skip.write = false;
		if (!DumpValue(
				skip,
				from,
				end,
				field.kind,
				field.vectorInner,
				field.bareId,
				0)) {
			return false;
		}
	}
	return false;
}

[[nodiscard]] QString FinishDump(const JsonWriter &writer) {
	if (writer.buffer.size <= 0) {
		return {};
	}
	return QString::fromUtf8(writer.buffer.p, writer.buffer.size);
}

} // namespace

QString DumpToJson(const mtpPrime *from, const mtpPrime *end) {
	if (!from || from >= end) {
		return {};
	}
	auto cursor = from;
	const auto cons = mtpTypeId(*cursor);
	if (const auto ctor = JsonTlConstructorById(cons)) {
		if (IsMessagesContainer(ctor->name)) {
			++cursor;
			JsonWriter unwrapped;
			if (DumpFirstMessage(unwrapped, cursor, end, ctor, 0)) {
				return FinishDump(unwrapped);
			}
			if (const auto partial = FinishDump(unwrapped)
				; !partial.isEmpty()) {
				LOG(("JSON dump: unwrap failed, keeping partial JSON."));
				return partial;
			}
		}
	} else {
		LOG(("JSON dump: top constructor 0x%1 not in schema, primes: %2"
			).arg(cons, 8, 16, QChar('0')
			).arg(int(end - from)));
	}
	JsonWriter full;
	auto fullFrom = from;
	const auto ok = DumpBoxed(full, fullFrom, end, 0);
	const auto json = FinishDump(full);
	if (!ok) {
		LOG(("JSON dump: full dump failed, produced %1 bytes."
			).arg(json.size()));
	}
	return json;
}

} // namespace MTP::details
