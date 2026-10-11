#!/usr/bin/env python3
"""
tools/check_doc_classes.py

Automates verification of Godot doc_classes XML files against GDExtension C++ bindings
and GDScript class definitions. Detects:
  - Undocumented or obsolete methods, properties, signals, and constants
  - Signature drift (parameter count, names, default values, qualifiers)
  - Property setter/getter mismatches
  - Blank or placeholder descriptions
  - Stale docs via Git diff (C++ implementation modified without updating XML doc)
  - Behavioral drift (undocumented error return codes or emitted signals)
  - Automatic stub generation for missing XML documentation tags
"""

import argparse
from dataclasses import dataclass, field
import json
import os
import re
import subprocess
import sys
from typing import Dict, List, Optional, Set, Tuple
import xml.etree.ElementTree as ET


# -----------------------------------------------------------------------------
# Data Models
# -----------------------------------------------------------------------------

@dataclass
class ParamInfo:
    name: str
    type_name: str = ""
    default_val: Optional[str] = None
    index: int = 0


@dataclass
class MethodInfoDef:
    name: str
    class_name: str = ""
    is_static: bool = False
    is_const: bool = False
    return_type: str = "void"
    params: List[ParamInfo] = field(default_factory=list)
    description: str = ""
    file_path: str = ""
    line_start: int = 0
    line_end: int = 0
    body: str = ""
    returned_errors: List[str] = field(default_factory=list)
    emitted_signals: List[str] = field(default_factory=list)


@dataclass
class PropertyInfoDef:
    name: str
    class_name: str = ""
    type_name: str = ""
    setter: str = ""
    getter: str = ""
    default_val: Optional[str] = None
    description: str = ""


@dataclass
class SignalInfoDef:
    name: str
    class_name: str = ""
    params: List[ParamInfo] = field(default_factory=list)
    description: str = ""


@dataclass
class ConstantInfoDef:
    name: str
    class_name: str = ""
    value: str = ""
    enum_name: str = ""
    description: str = ""


@dataclass
class ClassInfoDef:
    name: str
    inherits: str = ""
    brief_description: str = ""
    description: str = ""
    methods: Dict[str, MethodInfoDef] = field(default_factory=dict)
    properties: Dict[str, PropertyInfoDef] = field(default_factory=dict)
    signals: Dict[str, SignalInfoDef] = field(default_factory=dict)
    constants: Dict[str, ConstantInfoDef] = field(default_factory=dict)
    xml_path: Optional[str] = None
    source_path: Optional[str] = None


@dataclass
class Issue:
    severity: str  # "ERROR", "WARNING", "INFO"
    code: str      # e.g. "MISSING_METHOD", "SIGNATURE_DRIFT", "STALE_DOC"
    class_name: str
    item_name: str
    message: str
    file_path: Optional[str] = None
    line: Optional[int] = None


# -----------------------------------------------------------------------------
# Color Formatting
# -----------------------------------------------------------------------------

class Colors:
    RED = "\033[91m"
    GREEN = "\033[92m"
    YELLOW = "\033[93m"
    BLUE = "\033[94m"
    CYAN = "\033[96m"
    BOLD = "\033[1m"
    RESET = "\033[0m"

    @classmethod
    def disable(cls):
        cls.RED = ""
        cls.GREEN = ""
        cls.YELLOW = ""
        cls.BLUE = ""
        cls.CYAN = ""
        cls.BOLD = ""
        cls.RESET = ""


# -----------------------------------------------------------------------------
# C++ Parser (Extracts _bind_methods and function bodies)
# -----------------------------------------------------------------------------

class CppParser:
    def __init__(self, src_dir: str):
        self.src_dir = src_dir

    def parse_all(self) -> Dict[str, ClassInfoDef]:
        classes: Dict[str, ClassInfoDef] = {}
        cpp_files: List[str] = []
        for root, _, files in os.walk(self.src_dir):
            for f in files:
                if f.endswith((".cpp", ".h", ".hpp")):
                    cpp_files.append(os.path.join(root, f))

        # 1. Parse _bind_methods() blocks
        for file_path in cpp_files:
            try:
                with open(file_path, "r", encoding="utf-8", errors="replace") as f:
                    content = f.read()
            except OSError:
                continue

            self._parse_bind_methods_blocks(file_path, content, classes)

        # 2. Parse method bodies for behavior checking and git line mapping (only in .cpp files)
        for file_path in cpp_files:
            if not file_path.endswith(".cpp"):
                continue
            try:
                with open(file_path, "r", encoding="utf-8", errors="replace") as f:
                    content = f.read()
            except OSError:
                continue

            self._map_method_implementations(file_path, content, classes)

        return classes

    def _parse_bind_methods_blocks(self, file_path: str, content: str, classes: Dict[str, ClassInfoDef]):
        # Match void ClassName::_bind_methods() { ... }
        # Accounting for multiline bodies with nested braces
        pattern = re.compile(r"void\s+([A-Za-z0-9_]+)::_bind_methods\s*\(\s*\)\s*\{", re.MULTILINE)
        for match in pattern.finditer(content):
            class_name = match.group(1)
            start_pos = match.end() - 1
            body, _ = self._extract_brace_block(content, start_pos)

            if class_name not in classes:
                classes[class_name] = ClassInfoDef(name=class_name, source_path=file_path)
            cdef = classes[class_name]
            cdef.source_path = file_path

            self._parse_bindings_content(class_name, body, cdef, file_path)

    def _extract_brace_block(self, text: str, open_brace_pos: int) -> Tuple[str, int]:
        depth = 0
        in_string = False
        in_char = False
        in_line_comment = False
        in_block_comment = False
        escape = False
        i = open_brace_pos
        start_idx = open_brace_pos

        while i < len(text):
            ch = text[i]

            if in_line_comment:
                if ch == "\n":
                    in_line_comment = False
            elif in_block_comment:
                if ch == "*" and i + 1 < len(text) and text[i + 1] == "/":
                    in_block_comment = False
                    i += 1
            elif in_string:
                if escape:
                    escape = False
                elif ch == "\\":
                    escape = True
                elif ch == '"':
                    in_string = False
            elif in_char:
                if escape:
                    escape = False
                elif ch == "\\":
                    escape = True
                elif ch == "'":
                    in_char = False
            else:
                if ch == "/" and i + 1 < len(text) and text[i + 1] == "/":
                    in_line_comment = True
                    i += 1
                elif ch == "/" and i + 1 < len(text) and text[i + 1] == "*":
                    in_block_comment = True
                    i += 1
                elif ch == '"':
                    in_string = True
                elif ch == "'":
                    in_char = True
                elif ch == "{":
                    depth += 1
                elif ch == "}":
                    depth -= 1
                    if depth == 0:
                        return text[start_idx + 1:i], i
            i += 1
        return text[start_idx + 1:], len(text)

    def _parse_bindings_content(self, class_name: str, body: str, cdef: ClassInfoDef, file_path: str):
        # 1. bind_method and bind_static_method
        # Matches: ClassDB::bind_method(D_METHOD("name", "arg1", ...), ...)
        #      or: ClassDB::bind_static_method("ClassName" or get_class_static(), D_METHOD("name", ...), ...)
        method_pattern = re.compile(
            r"ClassDB::bind_(method|static_method)\s*\(\s*(?:(?:get_class_static\(\)|[A-Za-z0-9_\":]+)\s*,\s*)?D_METHOD\s*\(\s*([^)]+?)\s*\)",
            re.MULTILINE | re.DOTALL,
        )
        for m in method_pattern.finditer(body):
            is_static = (m.group(1) == "static_method")
            args_str = m.group(2)
            parsed_args = self._parse_string_list(args_str)
            if not parsed_args:
                continue

            method_name = parsed_args[0]
            param_names = parsed_args[1:]
            params = [ParamInfo(name=pname, index=idx) for idx, pname in enumerate(param_names)]

            cdef.methods[method_name] = MethodInfoDef(
                name=method_name,
                class_name=class_name,
                is_static=is_static,
                params=params,
                file_path=file_path,
            )

        # 2. ADD_PROPERTY(PropertyInfo(Variant::INT, "name"), "setter", "getter")
        prop_pattern = re.compile(
            r"ADD_PROPERTY\s*\(\s*PropertyInfo\s*\(\s*(?:Variant::)?([A-Za-z0-9_]+)\s*,\s*\"([A-Za-z0-9_]+)\"[^)]*?\)\s*,\s*\"([A-Za-z0-9_]*)\"\s*,\s*\"([A-Za-z0-9_]*)\"\s*\);",
            re.MULTILINE,
        )
        for m in prop_pattern.finditer(body):
            type_name = m.group(1).lower()
            prop_name = m.group(2)
            setter = m.group(3)
            getter = m.group(4)

            cdef.properties[prop_name] = PropertyInfoDef(
                name=prop_name,
                class_name=class_name,
                type_name=type_name,
                setter=setter,
                getter=getter,
            )

        # 3. ADD_SIGNAL(MethodInfo("signal_name", PropertyInfo(...)))
        signal_pattern = re.compile(
            r"ADD_SIGNAL\s*\(\s*MethodInfo\s*\(\s*\"([A-Za-z0-9_]+)\"(.*?)\)\s*\);",
            re.MULTILINE | re.DOTALL,
        )
        for m in signal_pattern.finditer(body):
            sig_name = m.group(1)
            sig_params_raw = m.group(2)
            sig_params: List[ParamInfo] = []
            param_matches = re.finditer(r"PropertyInfo\s*\(\s*(?:Variant::)?([A-Za-z0-9_]+)\s*,\s*\"([A-Za-z0-9_]+)\"", sig_params_raw)
            for pidx, pm in enumerate(param_matches):
                sig_params.append(ParamInfo(name=pm.group(2), type_name=pm.group(1).lower(), index=pidx))

            cdef.signals[sig_name] = SignalInfoDef(
                name=sig_name,
                class_name=class_name,
                params=sig_params,
            )

        # 4. BIND_ENUM_CONSTANT / BIND_CONSTANT
        const_pattern = re.compile(
            r"BIND_(?:ENUM_)?CONSTANT\s*\(\s*([A-Za-z0-9_]+)\s*\);",
            re.MULTILINE,
        )
        for m in const_pattern.finditer(body):
            const_name = m.group(1)
            cdef.constants[const_name] = ConstantInfoDef(
                name=const_name,
                class_name=class_name,
            )

    def _parse_string_list(self, raw_str: str) -> List[str]:
        # Extract quoted strings in order, e.g. "foo", "bar", "baz"
        return re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', raw_str)

    def _map_method_implementations(self, file_path: str, full_text: str, classes: Dict[str, ClassInfoDef]):
        # Single regex pass finding any ClassName::method_name( ... ) {
        pattern = re.compile(
            r"\b([A-Za-z0-9_]+)::([A-Za-z0-9_]+)\s*\([^;{]*?\)\s*(?:const\s*)?\{",
            re.MULTILINE,
        )
        for m in pattern.finditer(full_text):
            cname = m.group(1)
            mname = m.group(2)
            cdef = classes.get(cname)
            if not cdef or mname not in cdef.methods:
                continue

            mdef = cdef.methods[mname]
            start_char = m.start()
            open_brace = m.end() - 1
            body, end_char = self._extract_brace_block(full_text, open_brace)

            start_line = full_text[:start_char].count("\n") + 1
            end_line = full_text[:end_char].count("\n") + 1

            mdef.file_path = file_path
            mdef.line_start = start_line
            mdef.line_end = end_line
            mdef.body = body

            # Extract return ERR_* or Error
            errors_found = set(re.findall(r"\breturn\s+(ERR_[A-Za-z0-9_]+|OK)\b", body))
            mdef.returned_errors = sorted(list(errors_found))

            # Extract emit_signal("...")
            signals_emitted = set(re.findall(r'emit_signal\s*\(\s*"([A-Za-z0-9_]+)"', body))
            mdef.emitted_signals = sorted(list(signals_emitted))


# -----------------------------------------------------------------------------
# GDScript Parser (For classes like ResourceImporterDirectStorageTexture)
# -----------------------------------------------------------------------------

class GDScriptParser:
    def __init__(self, scripts_dir: str):
        self.scripts_dir = scripts_dir

    def parse_all(self) -> Dict[str, ClassInfoDef]:
        classes: Dict[str, ClassInfoDef] = {}
        if not os.path.exists(self.scripts_dir):
            return classes

        for root, _, files in os.walk(self.scripts_dir):
            for f in files:
                if f.endswith(".gd"):
                    path = os.path.join(root, f)
                    self._parse_file(path, classes)
        return classes

    def _parse_file(self, file_path: str, classes: Dict[str, ClassInfoDef]):
        try:
            with open(file_path, "r", encoding="utf-8", errors="replace") as f:
                lines = f.readlines()
        except OSError:
            return

        class_name = None
        inherits = ""

        # First pass: find class_name
        for line in lines:
            line_str = line.strip()
            cn_match = re.match(r"^class_name\s+([A-Za-z0-9_]+)(?:\s+extends\s+([A-Za-z0-9_]+))?", line_str)
            if cn_match:
                class_name = cn_match.group(1)
                inherits = cn_match.group(2) or ""
                break
            ext_match = re.match(r"^extends\s+([A-Za-z0-9_]+)", line_str)
            if ext_match and not inherits:
                inherits = ext_match.group(1)

        if not class_name:
            return

        cdef = ClassInfoDef(name=class_name, inherits=inherits, source_path=file_path)
        classes[class_name] = cdef

        full_script = "".join(lines)
        # If class implements _get_import_options, extract options as properties
        if "_get_import_options" in full_script:
            import_opts = re.findall(r'"name":\s*"([^"]+)"', full_script)
            for opt_name in import_opts:
                cdef.properties[opt_name] = PropertyInfoDef(
                    name=opt_name,
                    class_name=class_name,
                )

        # Second pass: methods, properties, signals, constants
        for idx, line in enumerate(lines):
            # Only top-level declarations (no leading spaces or tabs)
            if line.startswith((" ", "\t")):
                continue

            line_str = line.strip()
            # func / static func method_name(...) -> ReturnType:
            func_m = re.match(r"^(?:static\s+)?func\s+([A-Za-z0-9_]+)\s*\((.*?)\)(?:\s*->\s*([A-Za-z0-9_]+))?:", line_str)
            if func_m:
                mname = func_m.group(1)
                # Ignore private internal methods starting with underscore
                if mname.startswith("_"):
                    continue
                params_str = func_m.group(2)
                ret_type = func_m.group(3) or "void"

                params: List[ParamInfo] = []
                if params_str.strip():
                    raw_args = [a.strip() for a in params_str.split(",") if a.strip()]
                    for pidx, raw_arg in enumerate(raw_args):
                        arg_m = re.match(r"^([A-Za-z0-9_]+)(?:\s*:\s*([A-Za-z0-9_]+))?(?:\s*=\s*(.+))?", raw_arg)
                        if arg_m:
                            pname = arg_m.group(1)
                            ptype = arg_m.group(2) or ""
                            pdef = arg_m.group(3)
                            params.append(ParamInfo(name=pname, type_name=ptype, default_val=pdef, index=pidx))

                cdef.methods[mname] = MethodInfoDef(
                    name=mname,
                    class_name=class_name,
                    is_static=("static " in line_str),
                    return_type=ret_type,
                    params=params,
                    file_path=file_path,
                    line_start=idx + 1,
                )

            # var / @export var (only top-level)
            var_m = re.match(r"^(?:@(?:export|onready)\s+)?(?:static\s+)?var\s+([A-Za-z0-9_]+)(?:\s*:\s*([A-Za-z0-9_]+))?", line_str)
            if var_m:
                pname = var_m.group(1)
                if not pname.startswith("_"):
                    ptype = var_m.group(2) or ""
                    cdef.properties[pname] = PropertyInfoDef(
                        name=pname,
                        class_name=class_name,
                        type_name=ptype,
                    )

            # signal
            sig_m = re.match(r"^signal\s+([A-Za-z0-9_]+)(?:\s*\((.*?)\))?", line_str)
            if sig_m:
                sname = sig_m.group(1)
                cdef.signals[sname] = SignalInfoDef(name=sname, class_name=class_name)

            # const
            const_m = re.match(r"^const\s+([A-Za-z0-9_]+)\s*(?::\s*[A-Za-z0-9_]+)?\s*=", line_str)
            if const_m:
                cname = const_m.group(1)
                if not cname.startswith("_"):
                    cdef.constants[cname] = ConstantInfoDef(name=cname, class_name=class_name)


# -----------------------------------------------------------------------------
# XML Parser (doc_classes/*.xml)
# -----------------------------------------------------------------------------

class XmlDocParser:
    def __init__(self, xml_dir: str):
        self.xml_dir = xml_dir

    def parse_all(self) -> Dict[str, ClassInfoDef]:
        classes: Dict[str, ClassInfoDef] = {}
        if not os.path.exists(self.xml_dir):
            return classes

        for fname in os.listdir(self.xml_dir):
            if fname.endswith(".xml"):
                full_path = os.path.join(self.xml_dir, fname)
                try:
                    cdef = self.parse_file(full_path)
                    if cdef:
                        classes[cdef.name] = cdef
                except ET.ParseError as e:
                    print(f"Error parsing XML {full_path}: {e}", file=sys.stderr)
        return classes

    def parse_file(self, full_path: str) -> Optional[ClassInfoDef]:
        tree = ET.parse(full_path)
        root = tree.getroot()
        if root.tag != "class":
            return None

        class_name = root.attrib.get("name", "")
        inherits = root.attrib.get("inherits", "")

        brief_desc_el = root.find("brief_description")
        brief_desc = brief_desc_el.text.strip() if (brief_desc_el is not None and brief_desc_el.text) else ""

        desc_el = root.find("description")
        desc = desc_el.text.strip() if (desc_el is not None and desc_el.text) else ""

        cdef = ClassInfoDef(
            name=class_name,
            inherits=inherits,
            brief_description=brief_desc,
            description=desc,
            xml_path=full_path,
        )

        # Methods
        methods_el = root.find("methods")
        if methods_el is not None:
            for mel in methods_el.findall("method"):
                mname = mel.attrib.get("name", "")
                qualifiers = mel.attrib.get("qualifiers", "")
                is_static = ("static" in qualifiers)
                is_const = ("const" in qualifiers)

                ret_el = mel.find("return")
                ret_type = ret_el.attrib.get("type", "void") if ret_el is not None else "void"

                m_desc_el = mel.find("description")
                m_desc = m_desc_el.text.strip() if (m_desc_el is not None and m_desc_el.text) else ""

                params: List[ParamInfo] = []
                for pel in mel.findall("param"):
                    p_idx = int(pel.attrib.get("index", len(params)))
                    p_name = pel.attrib.get("name", "")
                    p_type = pel.attrib.get("type", "")
                    p_def = pel.attrib.get("default", None)
                    params.append(ParamInfo(name=p_name, type_name=p_type, default_val=p_def, index=p_idx))

                params.sort(key=lambda p: p.index)
                cdef.methods[mname] = MethodInfoDef(
                    name=mname,
                    class_name=class_name,
                    is_static=is_static,
                    is_const=is_const,
                    return_type=ret_type,
                    params=params,
                    description=m_desc,
                )

        # Members (properties)
        members_el = root.find("members")
        if members_el is not None:
            for mel in members_el.findall("member"):
                pname = mel.attrib.get("name", "")
                ptype = mel.attrib.get("type", "")
                setter = mel.attrib.get("setter", "")
                getter = mel.attrib.get("getter", "")
                default_val = mel.attrib.get("default", None)
                pdesc = mel.text.strip() if mel.text else ""

                cdef.properties[pname] = PropertyInfoDef(
                    name=pname,
                    class_name=class_name,
                    type_name=ptype,
                    setter=setter,
                    getter=getter,
                    default_val=default_val,
                    description=pdesc,
                )

        # Signals
        signals_el = root.find("signals")
        if signals_el is not None:
            for sel in signals_el.findall("signal"):
                sname = sel.attrib.get("name", "")
                s_desc_el = sel.find("description")
                s_desc = s_desc_el.text.strip() if (s_desc_el is not None and s_desc_el.text) else ""

                params: List[ParamInfo] = []
                for pel in sel.findall("param"):
                    p_idx = int(pel.attrib.get("index", len(params)))
                    p_name = pel.attrib.get("name", "")
                    p_type = pel.attrib.get("type", "")
                    params.append(ParamInfo(name=p_name, type_name=p_type, index=p_idx))

                params.sort(key=lambda p: p.index)
                cdef.signals[sname] = SignalInfoDef(
                    name=sname,
                    class_name=class_name,
                    params=params,
                    description=s_desc,
                )

        # Constants
        constants_el = root.find("constants")
        if constants_el is not None:
            for cel in constants_el.findall("constant"):
                cname = cel.attrib.get("name", "")
                val = cel.attrib.get("value", "")
                enum_name = cel.attrib.get("enum", "")
                cdesc = cel.text.strip() if cel.text else ""

                cdef.constants[cname] = ConstantInfoDef(
                    name=cname,
                    class_name=class_name,
                    value=val,
                    enum_name=enum_name,
                    description=cdesc,
                )

        return cdef


# -----------------------------------------------------------------------------
# Git Diff Analyzer (Detects staleness between C++ and XML)
# -----------------------------------------------------------------------------

class GitDiffAnalyzer:
    def __init__(self, diff_ref: Optional[str] = None):
        self.diff_ref = diff_ref

    def get_changed_lines(self) -> Dict[str, Set[int]]:
        """Returns a dict mapping absolute file paths to a set of changed line numbers."""
        changed: Dict[str, Set[int]] = {}

        cmd = ["git", "diff", "-U0"]
        if self.diff_ref:
            cmd.append(self.diff_ref)

        try:
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=True)
            diff_text = res.stdout
        except (subprocess.SubprocessError, FileNotFoundError):
            return changed

        # Parse unified diff headers: +++ b/path/to/file and @@ -x,y +start,count @@
        current_file = None
        for line in diff_text.splitlines():
            if line.startswith("+++ b/"):
                rel_path = line[6:].strip()
                current_file = os.path.abspath(rel_path)
                if current_file not in changed:
                    changed[current_file] = set()
            elif line.startswith("@@ ") and current_file:
                # @@ -old,old_cnt +new_start,new_cnt @@
                m = re.search(r"\+(\d+)(?:,(\d+))?", line)
                if m:
                    start = int(m.group(1))
                    count = int(m.group(2)) if m.group(2) is not None else 1
                    for line_num in range(start, start + max(1, count)):
                        changed[current_file].add(line_num)
        return changed


# -----------------------------------------------------------------------------
# Documentation Validator Engine
# -----------------------------------------------------------------------------

class DocValidator:
    def __init__(
        self,
        code_classes: Dict[str, ClassInfoDef],
        xml_classes: Dict[str, ClassInfoDef],
        git_diff_ref: Optional[str] = None,
        check_behavior: bool = False,
    ):
        self.code_classes = code_classes
        self.xml_classes = xml_classes
        self.git_diff_ref = git_diff_ref
        self.check_behavior = check_behavior
        self.issues: List[Issue] = []

    def validate_all(self, target_classes: Optional[Set[str]] = None) -> List[Issue]:
        self.issues.clear()

        # Determine all relevant class names
        all_class_names = set(self.xml_classes.keys()) | set(self.code_classes.keys())
        if target_classes:
            all_class_names = all_class_names.intersection(target_classes)

        git_analyzer = GitDiffAnalyzer(diff_ref=self.git_diff_ref) if self.git_diff_ref else None
        changed_lines = git_analyzer.get_changed_lines() if git_analyzer else {}

        for cname in sorted(all_class_names):
            code_c = self.code_classes.get(cname)
            xml_c = self.xml_classes.get(cname)

            if not xml_c and code_c:
                self.issues.append(Issue(
                    severity="ERROR",
                    code="MISSING_CLASS_DOC",
                    class_name=cname,
                    item_name=cname,
                    message=f"Class '{cname}' is defined in code ({code_c.source_path}) but has no XML doc in doc_classes/.",
                    file_path=code_c.source_path,
                ))
                continue

            if xml_c and not code_c:
                # Class in XML but not in C++/GDScript
                self.issues.append(Issue(
                    severity="WARNING",
                    code="OBSOLETE_CLASS_DOC",
                    class_name=cname,
                    item_name=cname,
                    message=f"Class '{cname}' exists in XML ({xml_c.xml_path}) but was not found in registered C++ or GDScript classes.",
                    file_path=xml_c.xml_path,
                ))
                continue

            # Both code and XML exist for this class
            self._validate_class(code_c, xml_c, changed_lines)

        return self.issues

    def _validate_class(self, code_c: ClassInfoDef, xml_c: ClassInfoDef, changed_lines: Dict[str, Set[int]]):
        # 1. Brief description & description checks
        if not xml_c.brief_description:
            self.issues.append(Issue(
                severity="WARNING",
                code="EMPTY_BRIEF_DESC",
                class_name=code_c.name,
                item_name=code_c.name,
                message=f"Class '{code_c.name}' has empty <brief_description> in XML.",
                file_path=xml_c.xml_path,
            ))
        if not xml_c.description:
            self.issues.append(Issue(
                severity="WARNING",
                code="EMPTY_CLASS_DESC",
                class_name=code_c.name,
                item_name=code_c.name,
                message=f"Class '{code_c.name}' has empty <description> in XML.",
                file_path=xml_c.xml_path,
            ))

        # 2. Methods validation
        self._validate_methods(code_c, xml_c, changed_lines)

        # 3. Properties validation
        self._validate_properties(code_c, xml_c)

        # 4. Signals validation
        self._validate_signals(code_c, xml_c)

        # 5. Constants validation
        self._validate_constants(code_c, xml_c)

    def _validate_methods(self, code_c: ClassInfoDef, xml_c: ClassInfoDef, changed_lines: Dict[str, Set[int]]):
        code_methods = code_c.methods
        xml_methods = xml_c.methods

        # Missing methods in XML
        for mname, mcode in code_methods.items():
            if mname not in xml_methods:
                # Private/internal callback methods starting with underscore are not required to be documented
                if mname.startswith("_"):
                    continue
                self.issues.append(Issue(
                    severity="ERROR",
                    code="MISSING_METHOD",
                    class_name=code_c.name,
                    item_name=mname,
                    message=f"Method '{code_c.name}.{mname}' is bound in code but missing from XML doc.",
                    file_path=xml_c.xml_path,
                ))
            else:
                mxml = xml_methods[mname]
                # Signature check: parameter counts & names
                self._check_method_signature(code_c.name, mcode, mxml, xml_c.xml_path)

                # Check empty description
                if not mxml.description or mxml.description.upper() in ["TODO", "TBD"]:
                    self.issues.append(Issue(
                        severity="WARNING",
                        code="EMPTY_METHOD_DESC",
                        class_name=code_c.name,
                        item_name=mname,
                        message=f"Method '{code_c.name}.{mname}' has empty description in XML.",
                        file_path=xml_c.xml_path,
                    ))

                # Check git staleness
                if changed_lines and mcode.file_path:
                    abs_src = os.path.abspath(mcode.file_path)
                    abs_xml = os.path.abspath(xml_c.xml_path) if xml_c.xml_path else None
                    src_changes = changed_lines.get(abs_src, set())
                    xml_changes = changed_lines.get(abs_xml, set()) if abs_xml else set()

                    # Did lines within [line_start, line_end] change?
                    method_lines = set(range(mcode.line_start, mcode.line_end + 1))
                    if method_lines.intersection(src_changes) and not xml_changes:
                        self.issues.append(Issue(
                            severity="WARNING",
                            code="STALE_DOC",
                            class_name=code_c.name,
                            item_name=mname,
                            message=f"Method '{code_c.name}.{mname}' implementation was modified in C++ ({mcode.file_path}:{mcode.line_start}-{mcode.line_end}), but {os.path.basename(xml_c.xml_path)} was not updated.",
                            file_path=mcode.file_path,
                            line=mcode.line_start,
                        ))

                # Behavior checking: error codes & signal emissions
                if self.check_behavior and mcode.body:
                    self._check_behavior_drift(code_c.name, mcode, mxml, xml_c.xml_path)

        # Obsolete methods in XML
        for mname in xml_methods:
            if mname not in code_methods:
                self.issues.append(Issue(
                    severity="WARNING",
                    code="OBSOLETE_METHOD",
                    class_name=code_c.name,
                    item_name=mname,
                    message=f"Method '{code_c.name}.{mname}' is documented in XML but not bound in code.",
                    file_path=xml_c.xml_path,
                ))

    def _check_method_signature(self, class_name: str, mcode: MethodInfoDef, mxml: MethodInfoDef, xml_path: str):
        # 1. Parameter count check
        if len(mcode.params) != len(mxml.params):
            self.issues.append(Issue(
                severity="ERROR",
                code="SIGNATURE_DRIFT_PARAM_COUNT",
                class_name=class_name,
                item_name=mcode.name,
                message=f"Method '{class_name}.{mcode.name}' parameter count mismatch: C++ has {len(mcode.params)} params ({[p.name for p in mcode.params]}), XML has {len(mxml.params)} params ({[p.name for p in mxml.params]}).",
                file_path=xml_path,
            ))
            return

        # 2. Parameter names check
        for idx, (pcode, pxml) in enumerate(zip(mcode.params, mxml.params)):
            if pcode.name != pxml.name:
                self.issues.append(Issue(
                    severity="WARNING",
                    code="SIGNATURE_DRIFT_PARAM_NAME",
                    class_name=class_name,
                    item_name=mcode.name,
                    message=f"Method '{class_name}.{mcode.name}' param {idx} name mismatch: C++ has '{pcode.name}', XML has '{pxml.name}'.",
                    file_path=xml_path,
                ))

        # 3. Static qualifier check
        if mcode.is_static != mxml.is_static:
            self.issues.append(Issue(
                severity="WARNING",
                code="QUALIFIER_DRIFT",
                class_name=class_name,
                item_name=mcode.name,
                message=f"Method '{class_name}.{mcode.name}' static qualifier mismatch: C++ is_static={mcode.is_static}, XML is_static={mxml.is_static}.",
                file_path=xml_path,
            ))

    def _check_behavior_drift(self, class_name: str, mcode: MethodInfoDef, mxml: MethodInfoDef, xml_path: str):
        desc = mxml.description
        # Check returned errors mentioned in doc
        for err in mcode.returned_errors:
            if err == "OK":
                continue
            # Look for ERR_* or [constant ERR_*] in description
            if err not in desc:
                self.issues.append(Issue(
                    severity="WARNING",
                    code="BEHAVIOR_UNDOCUMENTED_ERROR",
                    class_name=class_name,
                    item_name=mcode.name,
                    message=f"Method '{class_name}.{mcode.name}' returns '{err}' in code, but '{err}' is not mentioned in XML description.",
                    file_path=xml_path,
                ))

        # Check emitted signals mentioned in doc
        for sig in mcode.emitted_signals:
            if sig not in desc and f"[signal {sig}]" not in desc:
                self.issues.append(Issue(
                    severity="WARNING",
                    code="BEHAVIOR_UNDOCUMENTED_SIGNAL",
                    class_name=class_name,
                    item_name=mcode.name,
                    message=f"Method '{class_name}.{mcode.name}' emits signal '{sig}' in code, but signal is not referenced in XML description.",
                    file_path=xml_path,
                ))

    def _validate_properties(self, code_c: ClassInfoDef, xml_c: ClassInfoDef):
        code_props = code_c.properties
        xml_props = xml_c.properties

        for pname, pcode in code_props.items():
            if pname not in xml_props:
                self.issues.append(Issue(
                    severity="ERROR",
                    code="MISSING_PROPERTY",
                    class_name=code_c.name,
                    item_name=pname,
                    message=f"Property '{code_c.name}.{pname}' is registered in code but missing from XML doc.",
                    file_path=xml_c.xml_path,
                ))
            else:
                pxml = xml_props[pname]
                # Setter / Getter check
                if pcode.setter and pxml.setter and pcode.setter != pxml.setter:
                    self.issues.append(Issue(
                        severity="WARNING",
                        code="PROPERTY_SETTER_MISMATCH",
                        class_name=code_c.name,
                        item_name=pname,
                        message=f"Property '{code_c.name}.{pname}' setter mismatch: code has '{pcode.setter}', XML has '{pxml.setter}'.",
                        file_path=xml_c.xml_path,
                    ))
                if pcode.getter and pxml.getter and pcode.getter != pxml.getter:
                    self.issues.append(Issue(
                        severity="WARNING",
                        code="PROPERTY_GETTER_MISMATCH",
                        class_name=code_c.name,
                        item_name=pname,
                        message=f"Property '{code_c.name}.{pname}' getter mismatch: code has '{pcode.getter}', XML has '{pxml.getter}'.",
                        file_path=xml_c.xml_path,
                    ))
                if not pxml.description:
                    self.issues.append(Issue(
                        severity="WARNING",
                        code="EMPTY_PROPERTY_DESC",
                        class_name=code_c.name,
                        item_name=pname,
                        message=f"Property '{code_c.name}.{pname}' has empty description in XML.",
                        file_path=xml_c.xml_path,
                    ))

        for pname in xml_props:
            if pname not in code_props:
                self.issues.append(Issue(
                    severity="WARNING",
                    code="OBSOLETE_PROPERTY",
                    class_name=code_c.name,
                    item_name=pname,
                    message=f"Property '{code_c.name}.{pname}' is in XML doc but not bound in code.",
                    file_path=xml_c.xml_path,
                ))

    def _validate_signals(self, code_c: ClassInfoDef, xml_c: ClassInfoDef):
        code_sigs = code_c.signals
        xml_sigs = xml_c.signals

        for sname, scode in code_sigs.items():
            if sname not in xml_sigs:
                self.issues.append(Issue(
                    severity="ERROR",
                    code="MISSING_SIGNAL",
                    class_name=code_c.name,
                    item_name=sname,
                    message=f"Signal '{code_c.name}.{sname}' is registered in code but missing from XML doc.",
                    file_path=xml_c.xml_path,
                ))
            else:
                sxml = xml_sigs[sname]
                if not sxml.description:
                    self.issues.append(Issue(
                        severity="WARNING",
                        code="EMPTY_SIGNAL_DESC",
                        class_name=code_c.name,
                        item_name=sname,
                        message=f"Signal '{code_c.name}.{sname}' has empty description in XML.",
                        file_path=xml_c.xml_path,
                    ))

        for sname in xml_sigs:
            if sname not in code_sigs:
                self.issues.append(Issue(
                    severity="WARNING",
                    code="OBSOLETE_SIGNAL",
                    class_name=code_c.name,
                    item_name=sname,
                    message=f"Signal '{code_c.name}.{sname}' is in XML doc but not bound in code.",
                    file_path=xml_c.xml_path,
                ))

    def _validate_constants(self, code_c: ClassInfoDef, xml_c: ClassInfoDef):
        code_consts = code_c.constants
        xml_consts = xml_c.constants

        for cname, ccode in code_consts.items():
            if cname not in xml_consts:
                self.issues.append(Issue(
                    severity="ERROR",
                    code="MISSING_CONSTANT",
                    class_name=code_c.name,
                    item_name=cname,
                    message=f"Constant '{code_c.name}.{cname}' is bound in code but missing from XML doc.",
                    file_path=xml_c.xml_path,
                ))

        for cname in xml_consts:
            if cname not in code_consts:
                self.issues.append(Issue(
                    severity="INFO",
                    code="EXTRA_CONSTANT_IN_XML",
                    class_name=code_c.name,
                    item_name=cname,
                    message=f"Constant '{code_c.name}.{cname}' documented in XML is not directly found in BIND_CONSTANT (may be registered via enum type).",
                    file_path=xml_c.xml_path,
                ))


# -----------------------------------------------------------------------------
# Stub Generator (Inserts missing XML elements into doc_classes)
# -----------------------------------------------------------------------------

class StubGenerator:
    @staticmethod
    def generate_stubs_for_class(code_c: ClassInfoDef, xml_path: str):
        if not os.path.exists(xml_path):
            return

        with open(xml_path, "r", encoding="utf-8") as f:
            content = f.read()

        # Parse existing XML
        tree = ET.parse(xml_path)
        root = tree.getroot()

        existing_methods = {el.attrib.get("name") for el in root.findall(".//method")}
        existing_props = {el.attrib.get("name") for el in root.findall(".//member")}
        existing_sigs = {el.attrib.get("name") for el in root.findall(".//signal")}
        existing_consts = {el.attrib.get("name") for el in root.findall(".//constant")}

        methods_to_add = [m for mname, m in code_c.methods.items() if mname not in existing_methods]
        props_to_add = [p for pname, p in code_c.properties.items() if pname not in existing_props]
        sigs_to_add = [s for sname, s in code_c.signals.items() if sname not in existing_sigs]
        consts_to_add = [c for cname, c in code_c.constants.items() if cname not in existing_consts]

        if not (methods_to_add or props_to_add or sigs_to_add or consts_to_add):
            return 0

        added_count = 0

        # Inject methods into <methods>
        if methods_to_add:
            methods_block = ""
            for m in methods_to_add:
                qual = ' qualifiers="static"' if m.is_static else ''
                method_xml = f'\t\t<method name="{m.name}"{qual}>\n'
                method_xml += f'\t\t\t<return type="{m.return_type}" />\n'
                for p in m.params:
                    def_str = f' default="{p.default_val}"' if p.default_val else ''
                    type_str = f' type="{p.type_name}"' if p.type_name else ' type="Variant"'
                    method_xml += f'\t\t\t<param index="{p.index}" name="{p.name}"{type_str}{def_str} />\n'
                method_xml += '\t\t\t<description>\n\t\t\t</description>\n\t\t</method>\n'
                methods_block += method_xml
                added_count += 1

            if "</methods>" in content:
                content = content.replace("</methods>", methods_block + "\t</methods>")
            elif "<methods>" in content:
                content = content.replace("<methods>", "<methods>\n" + methods_block)
            else:
                # Add <methods> block before <members> or </class>
                block = f"\t<methods>\n{methods_block}\t</methods>\n"
                if "<members>" in content:
                    content = content.replace("<members>", block + "\t<members>")
                else:
                    content = content.replace("</class>", block + "</class>")

        # Inject properties into <members>
        if props_to_add:
            props_block = ""
            for p in props_to_add:
                type_attr = f' type="{p.type_name.capitalize()}"' if p.type_name else ' type="Variant"'
                setter_attr = f' setter="{p.setter}"' if p.setter else ''
                getter_attr = f' getter="{p.getter}"' if p.getter else ''
                props_block += f'\t\t<member name="{p.name}"{type_attr}{setter_attr}{getter_attr}>\n\t\t</member>\n'
                added_count += 1

            if "</members>" in content:
                content = content.replace("</members>", props_block + "\t</members>")
            else:
                block = f"\t<members>\n{props_block}\t</members>\n"
                if "<signals>" in content:
                    content = content.replace("<signals>", block + "\t<signals>")
                elif "<constants>" in content:
                    content = content.replace("<constants>", block + "\t<constants>")
                else:
                    content = content.replace("</class>", block + "</class>")

        # Write back updated XML
        with open(xml_path, "w", encoding="utf-8") as f:
            f.write(content)

        return added_count


# -----------------------------------------------------------------------------
# Main CLI & Report Formatting
# -----------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Verify Godot doc_classes XML against C++ and GDScript bindings."
    )
    parser.add_argument(
        "--xml-dir",
        default="project/addons/directstorage_accelerator/doc_classes",
        help="Path to doc_classes directory containing XML files.",
    )
    parser.add_argument(
        "--src-dir",
        default="src",
        help="Path to C++ source directory.",
    )
    parser.add_argument(
        "--scripts-dir",
        default="project/addons/directstorage_accelerator/scripts",
        help="Path to addon GDScript directory.",
    )
    parser.add_argument(
        "--classes",
        help="Comma-separated list of class names to validate (default: all).",
    )
    parser.add_argument(
        "--check", "--ci",
        action="store_true",
        help="Exit with non-zero exit code if any ERROR is found.",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Exit with non-zero exit code if any ERROR or WARNING is found.",
    )
    parser.add_argument(
        "--git-diff",
        nargs="?",
        const="",
        default=None,
        help="Check for stale docs where C++ was edited in git diff but XML was not updated. Optional ref (e.g. HEAD, origin/main).",
    )
    parser.add_argument(
        "--check-behavior",
        action="store_true",
        help="Cross-check method bodies for unreferenced error returns and emitted signals in docs.",
    )
    parser.add_argument(
        "--generate-stubs",
        action="store_true",
        help="Automatically generate missing XML element stubs in doc_classes/.",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Output results in JSON format.",
    )
    parser.add_argument(
        "--no-color",
        action="store_true",
        help="Disable ANSI color output.",
    )

    args = parser.parse_args()

    if args.no_color or not sys.stdout.isatty():
        Colors.disable()

    target_classes = set([c.strip() for c in args.classes.split(",") if c.strip()]) if args.classes else None

    # Parse C++ and GDScript
    cpp_parser = CppParser(src_dir=args.src_dir)
    code_classes = cpp_parser.parse_all()

    gd_parser = GDScriptParser(scripts_dir=args.scripts_dir)
    gd_classes = gd_parser.parse_all()

    # Merge code classes (C++ takes precedence if duplicate)
    for cname, cdef in gd_classes.items():
        if cname not in code_classes:
            code_classes[cname] = cdef

    # Parse XML docs
    xml_parser = XmlDocParser(xml_dir=args.xml_dir)
    xml_classes = xml_parser.parse_all()

    # Optional Stub Generation
    if args.generate_stubs:
        total_stubs = 0
        for cname in (target_classes or code_classes.keys()):
            if cname in code_classes and cname in xml_classes:
                xml_path = xml_classes[cname].xml_path
                if xml_path:
                    added = StubGenerator.generate_stubs_for_class(code_classes[cname], xml_path)
                    total_stubs += added
        print(f"Generated {total_stubs} XML stubs across classes.")
        # Re-parse XML after generating stubs
        xml_classes = xml_parser.parse_all()

    # Validate
    validator = DocValidator(
        code_classes=code_classes,
        xml_classes=xml_classes,
        git_diff_ref=args.git_diff,
        check_behavior=args.check_behavior,
    )
    issues = validator.validate_all(target_classes=target_classes)

    # Output formatting
    if args.json:
        report = [
            {
                "severity": i.severity,
                "code": i.code,
                "class": i.class_name,
                "item": i.item_name,
                "message": i.message,
                "file": i.file_path,
                "line": i.line,
            }
            for i in issues
        ]
        print(json.dumps(report, indent=2))
    else:
        errors = [i for i in issues if i.severity == "ERROR"]
        warnings = [i for i in issues if i.severity == "WARNING"]
        infos = [i for i in issues if i.severity == "INFO"]

        print(f"{Colors.BOLD}=== Godot Doc Classes Verification Report ==={Colors.RESET}")
        print(f"Scanned {len(xml_classes)} XML docs against {len(code_classes)} code classes.")
        print()

        if not issues:
            print(f"{Colors.GREEN}[OK] All doc_classes are fully synchronized with code!{Colors.RESET}")
        else:
            for issue in issues:
                if issue.severity == "ERROR":
                    tag = f"{Colors.RED}[ERROR:{issue.code}]{Colors.RESET}"
                elif issue.severity == "WARNING":
                    tag = f"{Colors.YELLOW}[WARN:{issue.code}]{Colors.RESET}"
                else:
                    tag = f"{Colors.CYAN}[INFO:{issue.code}]{Colors.RESET}"

                loc = f" ({issue.file_path}:{issue.line})" if issue.line else (f" ({issue.file_path})" if issue.file_path else "")
                print(f"{tag} {Colors.BOLD}{issue.class_name}{Colors.RESET}: {issue.message}{loc}")

            print()
            print(f"Summary: {len(errors)} errors, {len(warnings)} warnings, {len(infos)} info notes.")

    # Determine exit code
    has_errors = any(i.severity == "ERROR" for i in issues)
    has_warnings = any(i.severity == "WARNING" for i in issues)

    if args.strict and (has_errors or has_warnings):
        sys.exit(1)
    elif args.check and has_errors:
        sys.exit(1)
    else:
        sys.exit(0)


if __name__ == "__main__":
    main()
