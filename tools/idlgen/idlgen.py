#!/usr/bin/env python3
"""Generate the C ABI constants from idl/*.yaml.

The IDL is a restricted YAML subset: maps, lists of maps, and scalars.
This script is the only writer of include/legato/legato_idl.h and
engine/src/presets.gen.cpp.
"""

from __future__ import annotations

import argparse
import os
import sys


def parse_yaml(text):
    lines = []
    for number, raw in enumerate(text.splitlines(), 1):
        if "\t" in raw:
            raise SystemExit("tabs are not allowed in IDL (%d)" % number)
        stripped = strip_comment(raw).rstrip()
        if not stripped.strip():
            continue
        indent = len(stripped) - len(stripped.lstrip(" "))
        lines.append((indent, stripped.strip(), number))
    return parse_block(lines, 0, 0)


def strip_comment(line):
    in_string = False
    quote = ""
    for index, char in enumerate(line):
        if in_string:
            if char == quote:
                in_string = False
            continue
        if char in ("'", '"'):
            in_string = True
            quote = char
            continue
        if char == "#" and (index == 0 or line[index - 1] == " "):
            return line[:index]
    return line


def parse_block(lines, index, indent):
    if index >= len(lines):
        return "", index
    if lines[index][0] < indent:
        return "", index
    text = lines[index][1]
    if text.startswith("- "):
        return parse_seq(lines, index, lines[index][0])
    return parse_map(lines, index, lines[index][0])


def parse_seq(lines, index, indent):
    items = []
    while index < len(lines) and lines[index][0] == indent and lines[index][1].startswith("- "):
        payload = lines[index][1][2:].strip()
        child_index = index + 1
        if payload == "":
            node, index = parse_block(lines, child_index, indent + 1)
            items.append(node)
            continue
        key, value, has_key = split_key(payload)
        if not has_key:
            items.append(unquote(payload))
            index = child_index
            continue
        mapping = []
        if value == "":
            node, child_index = parse_block(lines, child_index, indent + 1)
            mapping.append((key, node))
        else:
            mapping.append((key, unquote(value)))
        while child_index < len(lines) and lines[child_index][0] > indent and not (
            lines[child_index][0] == indent and lines[child_index][1].startswith("- ")
        ):
            if lines[child_index][1].startswith("- "):
                break
            key, value, has_key = split_key(lines[child_index][1])
            if not has_key:
                raise SystemExit("expected key at line %d" % lines[child_index][2])
            next_index = child_index + 1
            if value == "":
                node, next_index = parse_block(lines, next_index, lines[child_index][0] + 1)
                mapping.append((key, node))
            else:
                mapping.append((key, unquote(value)))
            child_index = next_index
        items.append(mapping)
        index = child_index
    return items, index


def parse_map(lines, index, indent):
    mapping = []
    while index < len(lines) and lines[index][0] == indent and not lines[index][1].startswith("- "):
        key, value, has_key = split_key(lines[index][1])
        if not has_key:
            raise SystemExit("expected key at line %d" % lines[index][2])
        next_index = index + 1
        if value == "":
            node, next_index = parse_block(lines, next_index, indent + 1)
            mapping.append((key, node))
        else:
            mapping.append((key, unquote(value)))
        index = next_index
    return mapping, index


def split_key(text):
    if text.startswith("- "):
        return "", "", False
    in_string = False
    quote = ""
    for index, char in enumerate(text):
        if in_string:
            if char == quote:
                in_string = False
            continue
        if char in ("'", '"'):
            in_string = True
            quote = char
            continue
        if char == ":":
            key = text[:index].strip()
            value = text[index + 1 :].strip()
            return key, value, True
    return "", "", False


def unquote(value):
    if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
        return value[1:-1]
    return value


def as_map(node):
    if not isinstance(node, list):
        raise SystemExit("expected a map")
    out = {}
    for key, value in node:
        out[key] = value
    return out


def as_list(node):
    if not isinstance(node, list):
        raise SystemExit("expected a list")
    return node


def load_dir(idl_dir):
    docs = {}
    for name in ("enums", "properties", "commands", "events", "errors", "metrics", "presets"):
        path = os.path.join(idl_dir, name + ".yaml")
        with open(path, "r", encoding="utf-8") as handle:
            docs[name] = as_map(parse_yaml(handle.read())[0])
    return docs


def macro(prefix, name):
    cleaned = name.upper().replace(".", "_").replace("-", "_")
    return "LEGATO_%s_%s" % (prefix.upper(), cleaned)


def c_string(value):
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def comma_values(text):
    return [part.strip() for part in text.split(",") if part.strip()]


def bool_value(text):
    if text in ("true", "false"):
        return text
    raise SystemExit("expected true/false, got %s" % text)


def emit_header(docs):
    out = []
    out.append("/* Generated by tools/idlgen/idlgen.py. Do not edit. */")
    out.append("#ifndef LEGATO_IDL_H")
    out.append("#define LEGATO_IDL_H")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("#define LEGATO_ABI_VERSION 1u")
    out.append("")

    for item in as_list(docs["enums"]["enums"]):
        enum = as_map(item)
        type_name = "legato_%s" % enum["name"]
        out.append("typedef enum %s {" % type_name)
        values = comma_values(enum["values"])
        for index, value in enumerate(values):
            out.append("    %s = %d," % (macro(enum["name"], value), index))
        out.append("} %s;" % type_name)
        out.append("")
        out.append("static inline const char* legato_%s_name(int value) {" % enum["name"])
        out.append("    switch (value) {")
        for value in values:
            out.append("    case %s: return %s;" % (macro(enum["name"], value), c_string(value)))
        out.append('    default: return "unknown";')
        out.append("    }")
        out.append("}")
        out.append("")

    out.append("/* Properties */")
    for item in as_list(docs["properties"]["properties"]):
        prop = as_map(item)
        out.append("#define %s %s" % (macro("PROP", prop["name"]), c_string(prop["name"])))
    out.append("")

    out.append("/* Commands */")
    for item in as_list(docs["commands"]["commands"]):
        command = as_map(item)
        out.append("#define %s %s" % (macro("CMD", command["name"]), c_string(command["name"])))
    out.append("")

    out.append("/* Metrics */")
    for item in as_list(docs["metrics"]["metrics"]):
        metric = as_map(item)
        out.append("#define %s %s" % (macro("METRIC", metric["name"]), c_string(metric["name"])))
    out.append("")

    out.append("/* Error codes */")
    for item in as_list(docs["errors"]["errors"]):
        err = as_map(item)
        out.append("#define %s %s" % (macro("ERR", err["name"]), err["code"]))
    out.append("")

    out.append("#endif /* LEGATO_IDL_H */")
    out.append("")
    return "\n".join(out)


def emit_presets(docs):
    out = []
    out.append("/* Generated by tools/idlgen/idlgen.py. Do not edit. */")
    out.append('#include "legato/engine/preset.hpp"')
    out.append("")
    out.append("namespace legato {")
    out.append("")
    presets = [as_map(item) for item in as_list(docs["presets"]["presets"])]
    out.append("const PresetDesc kPresets[] = {")
    for preset in presets:
        out.append("    {%s, %s, %s, %s, %s, %s, %s, %s, %s}," % (
            c_string(preset["name"]),
            preset["buffer_start_s"],
            preset["buffer_resume_s"],
            preset["buffer_max_s"],
            preset["live_target_latency_s"],
            bool_value(preset["exact_seek"]),
            preset["preload_ahead"],
            preset["preload_behind"],
            bool_value(preset["video"]),
        ))
    out.append("};")
    out.append("")
    out.append("const int kPresetCount = %d;" % len(presets))
    out.append("")
    out.append("const PresetDesc* find_preset(const char* name) {")
    out.append("    if (name == nullptr) {")
    out.append("        return nullptr;")
    out.append("    }")
    out.append("    for (int i = 0; i < kPresetCount; ++i) {")
    out.append("        if (std::strcmp(kPresets[i].name, name) == 0) {")
    out.append("            return &kPresets[i];")
    out.append("        }")
    out.append("    }")
    out.append("    return nullptr;")
    out.append("}")
    out.append("")
    out.append("const PresetDesc* default_preset() {")
    out.append('    return find_preset("vod");')
    out.append("}")
    out.append("")
    out.append("}  /* namespace legato */")
    out.append("")
    return "\n".join(out)


def check_preset_fields(docs):
    preset_doc = docs["presets"]
    if "fields" not in preset_doc:
        raise SystemExit("idl/presets.yaml is missing fields: each preset field needs read_by")
    declared = {}
    for item in as_list(preset_doc["fields"]):
        field = as_map(item)
        name = field.get("name", "")
        reader = field.get("read_by", "")
        if not name or not reader:
            raise SystemExit("preset field %s is missing read_by" % (name or "?"))
        declared[name] = reader
    for item in as_list(preset_doc["presets"]):
        preset = as_map(item)
        for key in preset:
            if key == "name":
                continue
            if key not in declared:
                raise SystemExit("preset field %s has no read_by entry" % key)


def emit_errors(docs):
    out = []
    out.append("/* Generated by tools/idlgen/idlgen.py. Do not edit. */")
    out.append('#include "legato/engine/error_info.hpp"')
    out.append("")
    out.append("namespace legato {")
    out.append("")
    errors = [as_map(item) for item in as_list(docs["errors"]["errors"])]
    out.append("const ErrorInfo kErrors[] = {")
    for err in errors:
        if "retryable" not in err:
            raise SystemExit("error %s is missing retryable" % err.get("name", "?"))
        out.append("    {%s, %s, %s, %s, %s}," % (
            c_string(err["name"]),
            err["code"],
            c_string(err["category"]),
            bool_value(err["retryable"]),
            c_string(err["hint"]),
        ))
    out.append("};")
    out.append("")
    out.append("const int kErrorCount = %d;" % len(errors))
    out.append("")
    out.append("const ErrorInfo* error_info(const char* name) {")
    out.append("    if (name == nullptr) {")
    out.append("        return nullptr;")
    out.append("    }")
    out.append("    for (int i = 0; i < kErrorCount; ++i) {")
    out.append("        if (std::strcmp(kErrors[i].name, name) == 0) {")
    out.append("            return &kErrors[i];")
    out.append("        }")
    out.append("    }")
    out.append("    return nullptr;")
    out.append("}")
    out.append("")
    out.append("}  /* namespace legato */")
    out.append("")
    return "\n".join(out)


def write_file(path, contents):
    directory = os.path.dirname(path)
    if directory and not os.path.isdir(directory):
        os.makedirs(directory)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(contents)


def generate(idl_dir, root):
    docs = load_dir(idl_dir)
    check_preset_fields(docs)
    outputs = {
        os.path.join(root, "include", "legato", "legato_idl.h"): emit_header(docs),
        os.path.join(root, "engine", "src", "presets.gen.cpp"): emit_presets(docs),
        os.path.join(root, "engine", "src", "errors.gen.cpp"): emit_errors(docs),
    }
    for path, contents in outputs.items():
        write_file(path, contents)
    return outputs


def check(idl_dir, root):
    docs = load_dir(idl_dir)
    check_preset_fields(docs)
    expected = {
        os.path.join(root, "include", "legato", "legato_idl.h"): emit_header(docs),
        os.path.join(root, "engine", "src", "presets.gen.cpp"): emit_presets(docs),
        os.path.join(root, "engine", "src", "errors.gen.cpp"): emit_errors(docs),
    }
    failed = False
    for path, contents in expected.items():
        if not os.path.exists(path):
            sys.stderr.write("missing %s\n" % path)
            failed = True
            continue
        with open(path, "r", encoding="utf-8") as handle:
            current = handle.read()
        if current != contents:
            sys.stderr.write("out of date: %s\n" % path)
            failed = True
    if failed:
        raise SystemExit(1)


def main(argv):
    parser = argparse.ArgumentParser(description="generate Legato IDL outputs")
    parser.add_argument("--idl", default="idl")
    parser.add_argument("--root", default=".")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    root = os.path.abspath(args.root)
    idl_dir = args.idl if os.path.isabs(args.idl) else os.path.join(root, args.idl)
    if args.check:
        check(idl_dir, root)
    else:
        generate(idl_dir, root)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
