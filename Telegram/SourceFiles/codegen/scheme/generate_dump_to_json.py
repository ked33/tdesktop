'''
Generate a compact TL constructor table for native JSON dumps.
'''
import os
import re
import sys

BUILTIN = {
	'int': 'Int',
	'long': 'Long',
	'double': 'Double',
	'string': 'String',
	'bytes': 'Bytes',
	'int128': 'Int128',
	'int256': 'Int256',
	'true': 'True',
	'#': 'Flags',
}

SKIP_NAMES = {
	'int',
	'long',
	'double',
	'string',
	'bytes',
	'int128',
	'int256',
	'vector',
}

CONSTRUCTOR_RE = re.compile(
	r'([a-zA-Z][a-zA-Z0-9\._]*)#([0-9a-f]+)([^=]*)=\s*([a-zA-Z][a-zA-Z0-9\._<>]*);'
)
PARAM_RE = re.compile(
	r'([a-zA-Z_][a-zA-Z0-9_]*):'
	r'([A-Za-z0-9<>\._]+|\#|[a-z_][a-z0-9_]*\.[0-9]+\?[A-Za-z0-9<>\._]+)$'
)
TEMPLATE_RE = re.compile(r'^\{[A-Za-z]+:Type\}$')
VECTOR_RE = re.compile(r'^[Vv]ector<([A-Za-z0-9\._]+)>$')
FLAG_RE = re.compile(
	r'^([a-z_][a-z0-9_]*)\.([0-9]+)\?([A-Za-z0-9<>\._]+)$'
)


def is_boxed_name(type_name, ctor_ids):
	last = type_name.split('.')[-1]
	if last and last[0].isupper():
		return True
	return type_name not in ctor_ids


def kind_of(type_name, ctor_ids):
	if type_name in BUILTIN:
		return BUILTIN[type_name], 'Int', 0
	vector = VECTOR_RE.match(type_name)
	if vector:
		inner_kind, _inner_vec, inner_id = kind_of(vector.group(1), ctor_ids)
		return 'Vector', inner_kind, inner_id
	if is_boxed_name(type_name, ctor_ids):
		return 'Boxed', 'Int', 0
	return 'Bare', 'Int', ctor_ids[type_name]


def c_string(value):
	return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def parse_tl(path):
	pending = []
	with open(path, encoding='utf-8') as f:
		for raw in f:
			line = raw.split('//', 1)[0].strip()
			if not line or line.startswith('---'):
				continue
			matched = CONSTRUCTOR_RE.match(line)
			if not matched:
				continue
			name, typeid, params, _restype = matched.groups()
			short = name.split('.')[-1]
			if short in SKIP_NAMES:
				continue
			raw_fields = []
			for token in params.strip().split(' '):
				if not token or TEMPLATE_RE.match(token):
					continue
				if token.startswith('!'):
					continue
				parsed = PARAM_RE.match(token)
				if not parsed:
					continue
				field_name, field_type = parsed.groups()
				flag_name = ''
				flag_bit = 0
				flagged = FLAG_RE.match(field_type)
				if flagged:
					flag_name, flag_bit_text, field_type = flagged.groups()
					flag_bit = int(flag_bit_text)
				raw_fields.append((field_name, flag_name, flag_bit, field_type))
			pending.append({
				'id': int(typeid, 16),
				'name': name,
				'raw_fields': raw_fields,
			})
	return pending


def resolve_fields(pending):
	ctor_ids = { item['name']: item['id'] for item in pending }
	constructors = []
	for item in pending:
		fields = []
		for field_name, flag_name, flag_bit, field_type in item['raw_fields']:
			kind, vector_inner, bare_id = kind_of(field_type, ctor_ids)
			fields.append({
				'name': field_name,
				'flag_name': flag_name,
				'flag_bit': flag_bit,
				'kind': kind,
				'vector_inner': vector_inner,
				'bare_id': bare_id,
			})
		constructors.append({
			'id': item['id'],
			'name': item['name'],
			'fields': fields,
		})
	return constructors


def generate(output_path, input_files):
	pending = []
	seen = set()
	for path in input_files:
		for item in parse_tl(path):
			if item['id'] in seen:
				continue
			seen.add(item['id'])
			pending.append(item)
	constructors = resolve_fields(pending)
	constructors.sort(key=lambda item: item['id'])

	fields = []
	ctor_rows = []
	for item in constructors:
		first = len(fields)
		for field in item['fields']:
			fields.append(field)
		ctor_rows.append((item, first, len(item['fields'])))

	header = '''// WARNING! All changes made in this file will be lost!
#pragma once

#include "mtproto/core_types.h"

namespace MTP::details {

enum class JsonTlKind : uint8 {
	Int,
	Long,
	Double,
	String,
	Bytes,
	Int128,
	Int256,
	True,
	Flags,
	Boxed,
	Bare,
	Vector,
};

struct JsonTlField {
	const char *name;
	const char *flagName;
	uint8 flagBit;
	JsonTlKind kind;
	JsonTlKind vectorInner;
	mtpTypeId bareId;
};

struct JsonTlConstructor {
	mtpTypeId id;
	const char *name;
	uint16 firstField;
	uint16 fieldCount;
};

[[nodiscard]] const JsonTlConstructor *JsonTlConstructorById(mtpTypeId id);
[[nodiscard]] const JsonTlField *JsonTlFields();

} // namespace MTP::details
'''

	kind_enum = {
		'Int': 'JsonTlKind::Int',
		'Long': 'JsonTlKind::Long',
		'Double': 'JsonTlKind::Double',
		'String': 'JsonTlKind::String',
		'Bytes': 'JsonTlKind::Bytes',
		'Int128': 'JsonTlKind::Int128',
		'Int256': 'JsonTlKind::Int256',
		'True': 'JsonTlKind::True',
		'Flags': 'JsonTlKind::Flags',
		'Boxed': 'JsonTlKind::Boxed',
		'Bare': 'JsonTlKind::Bare',
		'Vector': 'JsonTlKind::Vector',
	}

	field_lines = []
	for field in fields:
		flag_name = c_string(field['flag_name']) if field['flag_name'] else 'nullptr'
		field_lines.append(
			'\t{ '
			+ c_string(field['name'])
			+ ', '
			+ flag_name
			+ ', '
			+ str(field['flag_bit'])
			+ ', '
			+ kind_enum[field['kind']]
			+ ', '
			+ kind_enum[field['vector_inner']]
			+ ', 0x{:08x} }},'.format(field['bare_id'])
		)

	ctor_lines = []
	for item, first, count in ctor_rows:
		ctor_lines.append(
			'\t{{ 0x{:08x}, {}, {}, {} }},'.format(
				item['id'],
				c_string(item['name']),
				first,
				count,
			)
		)

	source = '''// WARNING! All changes made in this file will be lost!
#include "''' + os.path.basename(output_path) + '''.h"

namespace MTP::details {
namespace {

constexpr JsonTlField kFields[] = {
''' + '\n'.join(field_lines) + '''
};

constexpr JsonTlConstructor kConstructors[] = {
''' + '\n'.join(ctor_lines) + '''
};

} // namespace

const JsonTlField *JsonTlFields() {
	return kFields;
}

const JsonTlConstructor *JsonTlConstructorById(mtpTypeId id) {
	for (const auto &ctor : kConstructors) {
		if (ctor.id == id) {
			return &ctor;
		}
	}
	return nullptr;
}

} // namespace MTP::details
'''

	header_path = output_path + '.h'
	source_path = output_path + '.cpp'
	with open(header_path, 'w', encoding='utf-8', newline='\n') as out:
		out.write(header)
	with open(source_path, 'w', encoding='utf-8', newline='\n') as out:
		out.write(source)
	return len(constructors), len(fields)


def main():
	output_path = ''
	input_files = []
	next_output = False
	for arg in sys.argv[1:]:
		if next_output:
			output_path = arg
			next_output = False
		elif arg == '-o':
			next_output = True
		elif arg.startswith('-o'):
			output_path = arg[2:]
		else:
			input_files.append(arg)
	if not output_path or not input_files:
		raise SystemExit('usage: generate_dump_to_json.py -o<path> <tl files>')
	count, fields = generate(output_path, input_files)
	print('dump_to_json constructors:', count, 'fields:', fields)


if __name__ == '__main__':
	main()
