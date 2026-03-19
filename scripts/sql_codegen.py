#!/usr/bin/env python3
"""Generate corouv::sql::ModelMeta specializations from SQL annotations.

Usage example:
  ./scripts/sql_codegen.py \
    --input include/models/user.h \
    --output include/models/user_sql_meta.gen.h \
    --clang-arg=-Iinclude

The script expects SQL annotations emitted by macros in `corouv/sql/reflect.h`:
  SQL_TABLE("users"), SQL_COL("id"), SQL_PK

Extensibility:
  --plugin /path/to/plugin.py

If present, plugin may define:
  transform(models, args) -> list[ModelDef] | None
  render(models, args, default_render) -> str
"""

from __future__ import annotations

import argparse
import dataclasses
import importlib.util
import pathlib
import re
import sys

try:
    from clang import cindex
except ImportError as exc:  # pragma: no cover - import guard
    raise SystemExit(
        "error: python package 'clang' is required (pip install clang)"
    ) from exc


ANNOTATE_PREFIX_TABLE = "table:"
ANNOTATE_PREFIX_COLUMN = "col:"


@dataclasses.dataclass
class FieldDef:
    member: str
    column: str
    primary_key: bool
    annotated: bool = False


@dataclasses.dataclass
class ModelDef:
    cpp_type: str
    table: str
    fields: list[FieldDef]
    source_file: str
    line: int


def snake_case(name: str) -> str:
    s1 = re.sub(r"(.)([A-Z][a-z]+)", r"\1_\2", name)
    s2 = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", s1)
    return s2.replace("__", "_").strip("_").lower()


def escape_cpp_string(text: str) -> str:
    return text.replace("\\", "\\\\").replace('"', '\\"')


def cursor_annotations(cursor: cindex.Cursor) -> list[str]:
    out: list[str] = []
    for child in cursor.get_children():
        if child.kind == cindex.CursorKind.ANNOTATE_ATTR:
            raw = child.displayname or child.spelling
            if raw:
                out.append(raw)
    return out


def qualified_cpp_name(cursor: cindex.Cursor) -> str:
    parts = [cursor.spelling]
    cur = cursor.semantic_parent
    while cur is not None and cur.kind != cindex.CursorKind.TRANSLATION_UNIT:
        if cur.kind in {
            cindex.CursorKind.NAMESPACE,
            cindex.CursorKind.STRUCT_DECL,
            cindex.CursorKind.CLASS_DECL,
            cindex.CursorKind.CLASS_TEMPLATE,
        } and cur.spelling:
            parts.append(cur.spelling)
        cur = cur.semantic_parent
    return "::".join(reversed(parts))


def parse_field(field_cursor: cindex.Cursor) -> FieldDef:
    annotations = cursor_annotations(field_cursor)
    column = field_cursor.spelling
    primary_key = False
    annotated = False
    for ann in annotations:
        if ann == "pk":
            primary_key = True
            annotated = True
        elif ann.startswith(ANNOTATE_PREFIX_COLUMN):
            column = ann[len(ANNOTATE_PREFIX_COLUMN) :]
            annotated = True
    return FieldDef(
        member=field_cursor.spelling,
        column=column,
        primary_key=primary_key,
        annotated=annotated,
    )


def parse_model(record_cursor: cindex.Cursor, include_unannotated: bool) -> ModelDef | None:
    annotations = cursor_annotations(record_cursor)

    table = ""
    table_annotated = False
    for ann in annotations:
        if ann.startswith(ANNOTATE_PREFIX_TABLE):
            table = ann[len(ANNOTATE_PREFIX_TABLE) :]
            table_annotated = True
            break
    if not table:
        table = snake_case(record_cursor.spelling)

    fields: list[FieldDef] = []
    any_field_annotated = False
    for child in record_cursor.get_children():
        if child.kind == cindex.CursorKind.FIELD_DECL and child.spelling:
            field = parse_field(child)
            any_field_annotated = any_field_annotated or field.annotated
            fields.append(field)

    if not include_unannotated and not table_annotated and not any_field_annotated:
        return None
    if not fields:
        return None

    file_name = ""
    line = 0
    if record_cursor.location.file is not None:
        file_name = str(record_cursor.location.file)
        line = int(record_cursor.location.line)

    return ModelDef(
        cpp_type=qualified_cpp_name(record_cursor),
        table=table,
        fields=fields,
        source_file=file_name,
        line=line,
    )


def collect_models(
    tu: cindex.TranslationUnit,
    target_files: set[pathlib.Path],
    include_unannotated: bool,
) -> list[ModelDef]:
    models: list[ModelDef] = []
    seen_usr: set[str] = set()

    def visit(cursor: cindex.Cursor) -> None:
        if cursor.kind in {
            cindex.CursorKind.STRUCT_DECL,
            cindex.CursorKind.CLASS_DECL,
        } and cursor.is_definition() and cursor.spelling:
            loc_file = cursor.location.file
            if loc_file is not None:
                loc_path = pathlib.Path(str(loc_file)).resolve()
                if loc_path in target_files:
                    usr = cursor.get_usr() or qualified_cpp_name(cursor)
                    if usr not in seen_usr:
                        seen_usr.add(usr)
                        model = parse_model(cursor, include_unannotated)
                        if model is not None:
                            models.append(model)

        for child in cursor.get_children():
            visit(child)

    visit(tu.cursor)
    return models


def default_render(models: list[ModelDef], args: argparse.Namespace) -> str:
    lines: list[str] = []
    lines.append("// Generated by scripts/sql_codegen.py. Do not edit manually.")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <array>")
    lines.append("#include <string_view>")
    lines.append("")
    lines.append('#include "corouv/sql/model.h"')
    lines.append("")
    lines.append("namespace corouv::sql {")
    lines.append("")

    for model in models:
        lines.append(f"template <>")
        lines.append(f"struct ModelMeta<{model.cpp_type}> {{")
        lines.append(
            f'    static constexpr std::string_view table = "{escape_cpp_string(model.table)}";'
        )
        if model.fields:
            lines.append(
                f"    static constexpr std::array<ColumnMeta, {len(model.fields)}> columns{{{{"
            )
            for field in model.fields:
                lines.append(
                    "        ColumnMeta{\"%s\", \"%s\", %s},"
                    % (
                        escape_cpp_string(field.member),
                        escape_cpp_string(field.column),
                        "true" if field.primary_key else "false",
                    )
                )
            lines.append("    }};")
        else:
            lines.append("    static constexpr std::array<ColumnMeta, 0> columns{};")
        lines.append("};")
        lines.append("")

    lines.append("}  // namespace corouv::sql")
    lines.append("")
    return "\n".join(lines)


def load_plugin(path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("corouv_sql_codegen_plugin", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load plugin: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate corouv::sql::ModelMeta specializations via libclang."
    )
    parser.add_argument(
        "--input",
        "-i",
        action="append",
        required=True,
        help="C++ header/source file containing SQL annotations (repeatable).",
    )
    parser.add_argument(
        "--output",
        "-o",
        required=True,
        help="Generated header output path.",
    )
    parser.add_argument(
        "--clang-arg",
        action="append",
        default=[],
        help="Additional argument forwarded to clang parser (repeatable).",
    )
    parser.add_argument(
        "--std",
        default="c++20",
        help="C++ language standard to parse (default: c++20).",
    )
    parser.add_argument(
        "--libclang",
        help="Explicit path to libclang shared library.",
    )
    parser.add_argument(
        "--plugin",
        help="Optional python plugin path exposing transform()/render() hooks.",
    )
    parser.add_argument(
        "--include-unannotated",
        action="store_true",
        help="Generate metadata for all records (table defaults to snake_case).",
    )
    parser.add_argument(
        "--no-force-clang-annotate",
        action="store_true",
        help=(
            "Do not define COROUV_SQL_FORCE_CLANG_ANNOTATE. "
            "By default this define is added for codegen stability."
        ),
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    cindex.Config.set_compatibility_check(False)
    if args.libclang:
        cindex.Config.set_library_file(args.libclang)

    input_files = [pathlib.Path(p).resolve() for p in args.input]
    for p in input_files:
        if not p.exists():
            raise SystemExit(f"error: input file not found: {p}")

    clang_args = ["-x", "c++", f"-std={args.std}", *args.clang_arg]
    if not args.no_force_clang_annotate:
        clang_args.append("-DCOROUV_SQL_FORCE_CLANG_ANNOTATE=1")

    index = cindex.Index.create()
    all_models: list[ModelDef] = []
    for path in input_files:
        tu = index.parse(str(path), args=clang_args)
        hard_errors = [
            d
            for d in tu.diagnostics
            if d.severity >= cindex.Diagnostic.Error
        ]
        if hard_errors:
            details = "\n".join(str(d) for d in hard_errors)
            raise SystemExit(
                f"error: failed to parse {path} with libclang:\n{details}"
            )

        models = collect_models(
            tu=tu,
            target_files={path},
            include_unannotated=args.include_unannotated,
        )
        all_models.extend(models)

    # Remove duplicates by C++ type, keep first one.
    dedup: dict[str, ModelDef] = {}
    for model in all_models:
        dedup.setdefault(model.cpp_type, model)
    models = list(dedup.values())
    models.sort(key=lambda m: m.cpp_type)

    plugin = None
    if args.plugin:
        plugin = load_plugin(pathlib.Path(args.plugin).resolve())
        if hasattr(plugin, "transform"):
            transformed = plugin.transform(models, args)
            if transformed is not None:
                models = transformed

    rendered: str
    if plugin is not None and hasattr(plugin, "render"):
        rendered = plugin.render(models, args, default_render)
    else:
        rendered = default_render(models, args)

    out_path = pathlib.Path(args.output).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
