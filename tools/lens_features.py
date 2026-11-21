"""DwoLens features: what each class does, split by entry point, and how each one unfolds.

    Blueprints  the entry nodes of event and function graphs (events, input, component events, custom events,
                functions), walked along the execution wires: calls, variable sets, branches, sequences, casts,
                loops, spawns, widgets, timers. Calls into the Blueprint's own functions are expanded in place.
    C++         read from the project's source (an estimate, marked as such): lifecycle overrides, input bindings
                (BindAction/BindAxis), delegate bindings (AddDynamic/AddUObject), timers (SetTimer), RPC and
                OnRep functions, Blueprint-callable functions. Member function bodies are scanned for calls and
                if/else/loops; calls to the class's own functions are expanded (two levels).

A Blueprint call into a C++ function of the project continues into that function's body, so a Blueprint input
event and the C++ code it triggers read as one flow. Output: out/features.json (schema lens.features/1).
"""
from __future__ import annotations

import re
from collections import defaultdict
from pathlib import Path

SCHEMA = "lens.features/1"
CATEGORIES = [
    ("lifecycle", "생명주기"), ("input", "입력"), ("ui", "UI·컴포넌트 이벤트"), ("collision", "충돌·오버랩"),
    ("interface", "인터페이스 구현"), ("network", "네트워크"), ("timer", "타이머·델리게이트"), ("event", "이벤트"),
    ("public", "공개 함수"),
]
LIFECYCLE = {
    "ReceiveBeginPlay", "ReceiveEndPlay", "ReceiveTick", "ReceiveDestroyed", "ReceivePossessed", "ReceiveUnpossessed",
    "Construct", "Destruct", "PreConstruct", "OnInitialized", "UserConstructionScript", "ReceiveInit", "ReceiveShutdown",
    "BeginPlay", "EndPlay", "Tick", "NativeConstruct", "NativeDestruct", "NativeOnInitialized", "NativePreConstruct",
    "NativeTick", "PostInitializeComponents", "OnConstruction", "PossessedBy", "UnPossessed", "OnPossess", "OnUnPossess",
    "Initialize", "Deinitialize", "Destroyed", "Landed", "BeginDestroy", "PostLoad",
}
UI_DELEGATES = ("OnClicked", "OnPressed", "OnReleased", "OnHovered", "OnUnhovered", "OnValueChanged", "OnTextChanged",
                "OnTextCommitted", "OnCheckStateChanged", "OnSelectionChanged", "OnItemClicked")
COLLISION_WORDS = ("Overlap", "Hit")
# other C++ overrides the engine calls on its own, by category
CPP_OVERRIDES = {
    "NotifyHit": "collision", "NotifyActorBeginOverlap": "collision", "NotifyActorEndOverlap": "collision",
    "OnMovementModeChanged": "event", "OnJumped_Implementation": "event", "Falling": "event",
    "TakeDamage": "event", "SetupPlayerInputComponent": "input", "SetupInputComponent": "input",
    "OnRep_PlayerState": "network", "GetLifetimeReplicatedProps": "network",
}
MAX_DEPTH = 2          # how deep own-function calls are expanded
MAX_STEPS = 60         # per block, to keep pathological graphs readable


def normalize_class(path: str) -> str:
    if not path:
        return path
    head, _, tail = path.rpartition(".")
    return f"{head}.{tail[5:]}" if tail.startswith("SKEL_") else path


def class_key(path: str) -> str:
    """The graph.json key of a class path: Blueprint classes are bp:, native ones uclass:."""
    path = normalize_class(path)
    return f"bp:{path}" if path.startswith("/") and not path.startswith("/Script/") else f"uclass:{path}"


def short(path: str) -> str:
    """Display name of a class path: /Game/X/BP_Door.BP_Door_C -> BP_Door."""
    name = path.rsplit(".", 1)[-1].replace("SKEL_", "") if path else ""
    return name[:-2] if name.endswith("_C") and path.startswith("/") and not path.startswith("/Script/") else name


def pretty_action(name: str) -> str:
    """JumpAction -> Jump, IA_Move -> Move."""
    name = re.sub(r"^(IA_|InputAction_)", "", name.split("->")[-1].split(".")[-1])
    return re.sub(r"(Action|InputAction)$", "", name) or name


# ============================================================================ C++ source index

def _blank(m: re.Match) -> str:
    return re.sub(r"[^\n]", " ", m.group(0))


def strip_comments(text: str) -> str:
    return re.sub(r"//[^\n]*|/\*.*?\*/", _blank, text, flags=re.S)


def strip_strings(text: str) -> str:
    return re.sub(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', _blank, text)


def match_bracket(text: str, i: int, open_ch: str, close_ch: str) -> int:
    """Index just past the bracket that closes text[i] (an open bracket), or len(text)."""
    depth = 0
    for j in range(i, len(text)):
        c = text[j]
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return j + 1
    return len(text)


DEF_RE = re.compile(r"\b([AUFSI][A-Za-z0-9_]*)::(~?[A-Za-z_]\w*)\s*\(")


class CppIndex:
    """Member function bodies of the project's C++ (Source/ and project plugins)."""

    def __init__(self, root: Path):
        self.root = root
        self.bodies: dict[tuple[str, str], list[dict]] = defaultdict(list)   # (PrefixedClass, Method) -> [{body, code, file, line}]
        bases = [root / "Source"]
        plugins = root / "Plugins"
        if plugins.exists():
            bases += [d / "Source" for d in plugins.iterdir() if d.is_dir() and not d.name.startswith("__")]
        for base in bases:
            if not base.exists():
                continue
            for cpp in base.rglob("*.cpp"):
                if "Intermediate" in cpp.parts:
                    continue
                try:
                    raw = cpp.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    continue
                self._index(cpp, raw)

    def _index(self, cpp: Path, raw: str):
        code = strip_comments(raw)          # strings kept: input action names, delegate names
        bare = strip_strings(code)          # structure: no braces or parens hiding in strings
        rel = cpp.relative_to(self.root).as_posix()
        for m in DEF_RE.finditer(bare):
            open_paren = m.end() - 1
            close = match_bracket(bare, open_paren, "(", ")")
            k = close
            # qualifiers, then '{' for a definition (a ';' or anything else means a call or declaration)
            tail = re.match(r"\s*(?:const\s*|override\s*|final\s*|noexcept\s*)*(:[^{;]*)?\{", bare[k:])
            if not tail:
                continue
            body_open = k + tail.end() - 1
            body_close = match_bracket(bare, body_open, "{", "}")
            line = bare.count("\n", 0, m.start()) + 1
            self.bodies[(m.group(1), m.group(2))].append({
                "body": bare[body_open + 1: body_close - 1], "code": code[body_open + 1: body_close - 1],
                "file": rel, "line": line})

    def methods_of(self, prefixed: str) -> list[str]:
        return sorted({m for (c, m) in self.bodies if c == prefixed})

    def has(self, prefixed: str, method: str) -> bool:
        return (prefixed, method) in self.bodies

    def first(self, prefixed: str, method: str) -> dict | None:
        b = self.bodies.get((prefixed, method))
        return b[0] if b else None


KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "decltype", "static_cast", "dynamic_cast", "const_cast",
            "reinterpret_cast", "catch", "new", "delete", "case", "do", "else", "TEXT", "check", "checkf", "ensure",
            "ensureMsgf", "verify", "UE_LOG", "LOCTEXT", "NSLOCTEXT", "Super", "TEXTVIEW", "DECLARE_CYCLE_STAT",
            "SCOPE_CYCLE_COUNTER", "QUICK_SCOPE_CYCLE_COUNTER", "MoveTemp", "Forward", "IsValid", "StaticClass",
            "Num", "Add", "Emplace", "Contains", "Find", "Remove", "Reset", "Empty", "Append", "Insert", "IsEmpty",
            "ToString", "Printf", "Format", "FromString", "Equals", "Len"}
NOISE_PREFIXES = ("Get", "Is", "Has", "Can", "Should")
CALL_RE = re.compile(r"(?:\b(\w+)\s*(?:->|\.|::)\s*)?\b(~?[A-Za-z_]\w*)\s*(?:<\s*([^<>;()]*?)\s*>)?\s*\(")


def _short_text(s: str, n: int = 48) -> str:
    s = re.sub(r"\s+", " ", s).strip()
    return s if len(s) <= n else s[: n - 1] + "…"


class CppFlow:
    """Turns a member function body into steps (estimated: source patterns, not a compiler)."""

    def __init__(self, index: CppIndex, keys: dict[str, str]):
        self.index = index
        self.keys = keys        # prefixed class name -> graph key (project classes)

    def method(self, prefixed: str, method: str, depth: int, seen: set) -> list[dict]:
        entry = self.index.first(prefixed, method)
        if not entry or (prefixed, method) in seen or depth > MAX_DEPTH:
            return []
        seen = seen | {(prefixed, method)}
        return self.block(entry["body"], prefixed, depth, seen)

    def block(self, text: str, cls: str, depth: int, seen: set) -> list[dict]:
        steps: list[dict] = []
        i, n = 0, len(text)
        while i < n and len(steps) < MAX_STEPS:
            while i < n and text[i] in " \t\r\n;":
                i += 1
            if i >= n:
                break
            m = re.match(r"(if|for|while|switch)\s*\(", text[i:])
            if m:
                kw = m.group(1)
                p = i + m.end() - 1
                q = match_bracket(text, p, "(", ")")
                cond = text[p + 1: q - 1]
                body, i = self._statement(text, q)
                if kw == "if":
                    step = {"kind": "branch", "text": "조건: " + _short_text(cond), "estimated": True,
                            "branches": [{"label": "참", "steps": self.block(body, cls, depth, seen)}]}
                    rest = re.match(r"\s*else\b", text[i:])
                    if rest:
                        body2, i = self._statement(text, i + rest.end())
                        step["branches"].append({"label": "거짓", "steps": self.block(body2, cls, depth, seen)})
                    steps.append(step)
                else:
                    label = {"for": "반복", "while": "반복", "switch": "경우별"}[kw]
                    steps.append({"kind": "loop" if kw != "switch" else "branch", "text": f"{label}: " + _short_text(cond),
                                  "estimated": True, "branches": [{"label": label, "steps": self.block(body, cls, depth, seen)}]})
                continue
            if text[i] == "{":
                j = match_bracket(text, i, "{", "}")
                steps += self.block(text[i + 1: j - 1], cls, depth, seen)
                i = j
                continue
            if re.match(r"(else|do|try)\b", text[i:]):
                i += re.match(r"\w+", text[i:]).end()
                continue
            j = self._statement_end(text, i)
            steps += self.calls(text[i:j], cls, depth, seen)
            i = j
        return _dedupe(steps)

    def _statement(self, text: str, i: int) -> tuple[str, int]:
        """Body of an if/for/while: a {block} or a single statement. Returns (body, index after it)."""
        while i < len(text) and text[i] in " \t\r\n":
            i += 1
        if i < len(text) and text[i] == "{":
            j = match_bracket(text, i, "{", "}")
            return text[i + 1: j - 1], j
        j = self._statement_end(text, i)
        return text[i:j], j

    @staticmethod
    def _statement_end(text: str, i: int) -> int:
        depth = 0
        for j in range(i, len(text)):
            c = text[j]
            if c in "({[":
                depth += 1
            elif c in ")}]":
                depth -= 1
            elif c == ";" and depth <= 0:
                return j + 1
        return len(text)

    def calls(self, stmt: str, cls: str, depth: int, seen: set) -> list[dict]:
        out = []
        a = re.match(r"\s*(?:this\s*->\s*)?([A-Za-z_]\w*)\s*(?:[+\-*/|&]?=)(?!=)\s*(.+)", stmt, re.S)
        if a and (a.group(1)[0].isupper() or re.match(r"b[A-Z]", a.group(1))) and not re.match(r"\s*(const\s+)?[A-Z]\w*\s*[*&]?\s+\w+\s*=", stmt):
            out.append({"kind": "set", "text": f"설정 {a.group(1)}", "estimated": True})
        for m in CALL_RE.finditer(stmt):
            qual, name, targ = m.group(1), m.group(2), (m.group(3) or "").strip()
            if name in KEYWORDS or name.isupper() or qual in ("Super", "FMath", "UKismetMathLibrary", "FString", "FName", "FText"):
                continue
            before = re.search(r"([A-Za-z_][\w:<>]*)\s*[*&]?\s+$", stmt[: m.start()])
            if not qual and before and before.group(1) not in ("return", "else", "co_return", "throw", "new"):
                continue    # a variable declared with constructor arguments: FRotator YawRotation(...)
            if name in ("Cast", "CastChecked", "ExactCast") and targ:
                out.append({"kind": "cast", "text": f"형변환 {targ.strip('* ')}", "estimated": True})
                continue
            if name.startswith("SpawnActor"):
                out.append({"kind": "spawn", "text": f"액터 스폰 {targ.strip('* ')}".strip(), "estimated": True})
                continue
            if name == "CreateWidget":
                out.append({"kind": "widget", "text": f"위젯 생성 {targ.strip('* ')}".strip(), "estimated": True})
                continue
            if name == "SetTimer":
                t = re.search(r"&\s*\w+::(\w+)", stmt)
                out.append({"kind": "timer", "text": "타이머 → " + (t.group(1) if t else "람다"), "estimated": True})
                continue
            if name in ("BindAction", "BindAxis", "AddDynamic", "AddUniqueDynamic", "AddUObject", "AddLambda", "BindUObject"):
                t = re.search(r"&\s*\w+::(\w+)", stmt)
                out.append({"kind": "bind", "text": f"연결 → {t.group(1) if t else '람다'}", "estimated": True})
                continue
            if re.match(r"^[FTEU][A-Z]", name) and not qual:
                continue    # constructor of a value type (FVector(...), TArray<>...)
            if name.startswith(NOISE_PREFIXES) and not self.index.has(cls, name):
                continue
            if qual and re.match(r"^[FTE][A-Z]", qual) and qual != "this":
                continue    # FVector::Dist, FMath-like statics
            step = {"kind": "call", "text": (f"{qual}.{name}" if qual and qual != "this" else name), "estimated": True}
            own = (not qual or qual == "this") and self.index.has(cls, name)
            if qual and qual[0] in "AU" and qual in self.keys:
                step["target"] = self.keys[qual]
            if own and depth < MAX_DEPTH:
                inner = self.method(cls, name, depth + 1, seen)
                if inner:
                    step["inner"] = inner
            out.append(step)
        return out


def _dedupe(steps: list[dict]) -> list[dict]:
    """Consecutive identical steps become one, with 'repeat' counting them."""
    out = []
    for s in steps:
        if out and out[-1]["text"] == s["text"] and "branches" not in s and "branches" not in out[-1]:
            out[-1]["repeat"] = out[-1].get("repeat", 1) + 1
            continue
        out.append(s)
    return out


# ============================================================================ Blueprints

ENTRY_NODES = {"K2Node_Event", "K2Node_CustomEvent", "K2Node_ComponentBoundEvent", "K2Node_FunctionEntry",
               "K2Node_InputAction", "K2Node_EnhancedInputAction", "K2Node_InputKey", "K2Node_InputAxisEvent",
               "K2Node_InputTouch", "K2Node_InputAxisKeyEvent", "K2Node_InputVectorAxisEvent"}
INPUT_NODES = {"K2Node_InputAction", "K2Node_EnhancedInputAction", "K2Node_InputKey", "K2Node_InputAxisEvent",
               "K2Node_InputTouch", "K2Node_InputAxisKeyEvent", "K2Node_InputVectorAxisEvent"}


class BpFlow:
    def __init__(self, bp: dict, bp_by_class: dict, cpp: CppFlow, prefixed_of: dict, native_parents: list[str]):
        self.bp = bp
        self.bp_by_class = bp_by_class
        self.cpp = cpp
        self.prefixed_of = prefixed_of          # /Script/Mod.Name -> AName / UName
        self.native_parents = native_parents    # project native ancestors (paths), nearest first
        self.functions = {g["name"]: g for g in bp["graphs"] if g["kind"] == "function"}

    def graph_steps(self, graph: dict, start: str, depth: int, seen: set) -> list[dict]:
        nodes = {n["id"]: n for n in graph["nodes"]}
        out = defaultdict(list)
        for l in graph["links"]:
            if l["kind"] == "exec":
                out[l["from"]].append((l["fromPin"], l["to"]))
        visited = set()

        def walk(nid: str | None) -> list[dict]:
            steps = []
            while nid and nid not in visited and len(steps) < MAX_STEPS:
                visited.add(nid)
                node = nodes.get(nid)
                if not node:
                    break
                nexts = out.get(nid, [])
                if nid != start:
                    step = self.describe(node, depth, seen)
                    if step:
                        steps.append(step)
                labelled = node["class"] in ("K2Node_IfThenElse", "K2Node_DynamicCast", "K2Node_MacroInstance") or                     node["class"].startswith("K2Node_Switch")
                if len(nexts) > 1 or (labelled and nexts and nid != start and steps):
                    if steps and steps[-1].get("kind") in ("branch", "loop", "sequence", "cast", "macro") and nid != start:
                        holder = steps[-1]
                    else:
                        holder = {"kind": "sequence", "text": node.get("title", ""), "branches": []}
                        steps.append(holder)
                    holder.setdefault("branches", [])
                    for pin, to in sorted(nexts, key=lambda x: x[0]):
                        holder["branches"].append({"label": pin_label(pin, node["class"]), "steps": walk(to)})
                    break
                nid = nexts[0][1] if nexts else None
            return _dedupe(steps)

        return _dedupe(walk(start))

    def describe(self, n: dict, depth: int, seen: set) -> dict | None:
        cls, member, owner = n["class"], n.get("member", ""), n.get("memberOwner", "")
        title = n.get("title", "")
        if cls in ("EdGraphNode_Comment", "K2Node_Knot"):
            return None
        if cls == "K2Node_IfThenElse":
            return {"kind": "branch", "text": "분기"}
        if cls == "K2Node_ExecutionSequence":
            return {"kind": "sequence", "text": "순서대로"}
        if cls.startswith("K2Node_Switch"):
            return {"kind": "branch", "text": title or "경우별"}
        if cls == "K2Node_DynamicCast":
            return {"kind": "cast", "text": f"형변환 {short(member) or title}", "target": class_key(member) if member else ""}
        if cls == "K2Node_MacroInstance":
            return {"kind": "loop" if "Loop" in member or "Loop" in title else "macro", "text": title}
        if cls == "K2Node_VariableSet":
            return {"kind": "set", "text": f"설정 {member or title}"}
        if cls == "K2Node_SpawnActorFromClass":
            return {"kind": "spawn", "text": title}
        if cls == "K2Node_CreateWidget":
            return {"kind": "widget", "text": title}
        if cls == "K2Node_Timeline":
            return {"kind": "timer", "text": f"타임라인 {title}"}
        if cls in ("K2Node_CallDelegate", "K2Node_CallDelegateFunction"):
            return {"kind": "dispatch", "text": f"디스패처 호출 {member or title}"}
        if n.get("role") == "call" or cls in ("K2Node_CallFunction", "K2Node_Message", "K2Node_CallParentFunction"):
            owner_n = normalize_class(owner)
            name = member or title
            mine = owner_n == normalize_class(self.bp["generatedClass"])
            step = {"kind": "call", "text": f"{short(owner_n)}.{name}" if owner_n and not mine else name}
            if "Delay" in name or "SetTimer" in name:
                step["kind"] = "timer"
            if owner_n and owner_n != normalize_class(self.bp["generatedClass"]) and                     not owner_n.startswith(("/Script/Engine", "/Script/CoreUObject", "/Script/UMG", "/Script/EnhancedInput")):
                step["target"] = class_key(owner_n)
            if depth < MAX_DEPTH:
                # the Blueprint's own function graph, else a project C++ function of an ancestor
                own_bp = owner_n == normalize_class(self.bp["generatedClass"]) or not owner_n
                if own_bp and name in self.functions and ("bp", name) not in seen:
                    g = self.functions[name]
                    entry = next((x["id"] for x in g["nodes"] if x["class"] == "K2Node_FunctionEntry"), None)
                    if entry:
                        inner = BpFlow(self.bp, self.bp_by_class, self.cpp, self.prefixed_of, self.native_parents).graph_steps(
                            g, entry, depth + 1, seen | {("bp", name)})
                        if inner:
                            step["inner"] = inner
                elif owner_n in self.prefixed_of or (owner_n and any(owner_n == p for p in self.native_parents)):
                    pref = self.prefixed_of.get(owner_n)
                    if pref:
                        inner = self.cpp.method(pref, name, depth + 1, set())
                        if inner:
                            step["inner"] = inner
                            step["estimated"] = True
            return step
        if cls in ("K2Node_FunctionResult",):
            return None
        if not title or title in ("Get", "Set", "Return Node") or n.get("role") == "variable":
            return None
        return {"kind": "other", "text": title}


def pin_label(pin: str, node_class: str = "") -> str:
    if node_class == "K2Node_IfThenElse":
        return {"then": "참", "else": "거짓"}.get(pin, pin)
    if node_class == "K2Node_DynamicCast":
        return {"then": "성공", "CastFailed": "실패"}.get(pin, pin)
    if pin.startswith("then_") and pin[5:].isdigit():
        return str(int(pin[5:]) + 1)
    return {"then": "다음", "else": "거짓", "Is Valid": "유효", "Is Not Valid": "무효", "CastFailed": "실패",
            "LoopBody": "반복", "Completed": "완료", "Finished": "완료", "Update": "갱신", "Exec": "다음"}.get(pin, pin)


# ============================================================================ build

def bp_category(n: dict, interfaces: set[str]) -> str:
    cls, member, owner, title = n["class"], n.get("member", ""), normalize_class(n.get("memberOwner", "")), n.get("title", "")
    if cls in INPUT_NODES:
        return "input"
    if cls == "K2Node_FunctionEntry":
        return "lifecycle" if title == "Construction Script" else "public"
    if cls == "K2Node_ComponentBoundEvent":
        return "collision" if any(w in title for w in COLLISION_WORDS) else "ui"
    if cls == "K2Node_CustomEvent":
        return "event"
    if member in LIFECYCLE or member.replace("Receive", "") in LIFECYCLE:
        return "lifecycle"
    if any(w in member for w in COLLISION_WORDS):
        return "collision"
    if owner in interfaces or "/BPI_" in owner or short(owner).startswith(("BPI_", "I")) and "Interface" in owner:
        return "interface"
    return "event"


def build_features(raw: dict, project_root: Path, symbols: list[dict]) -> dict:
    """raw: {'reflection': ..., 'blueprints': [...]}; symbols: graph.json symbols (for keys and names)."""
    reflection = raw["reflection"]
    blueprints = raw["blueprints"]
    native = {normalize_class(c["path"]): c for c in reflection["native"]}
    interfaces = {normalize_class(c["path"]) for c in reflection["native"] + reflection["blueprint"] if c.get("interface")}
    # prefixed C++ names: AActorThing / UObjectThing. The source tells which prefix a class uses.
    index = CppIndex(project_root)
    declared = {c for (c, _) in index.bodies}
    prefixed_of, keys = {}, {}
    for path, c in native.items():
        for p in ("A", "U", "F", "S", "I"):
            if p + c["name"] in declared:
                prefixed_of[path] = p + c["name"]
                keys[p + c["name"]] = class_key(path)
                break
    cpp = CppFlow(index, keys)
    bp_by_class = {normalize_class(b["generatedClass"]): b for b in blueprints}

    classes: dict[str, dict] = {}

    def entry(key: str, name: str, kind: str) -> dict:
        return classes.setdefault(key, {"key": key, "name": name, "kind": kind, "features": []})

    # ---- C++ classes
    for path, c in native.items():
        pref = prefixed_of.get(path)
        if not pref:
            continue
        cls = entry(class_key(path), c["name"], "class")
        used = set()
        methods = index.methods_of(pref)
        for m in methods:
            if m in LIFECYCLE:
                body = index.first(pref, m)
                cls["features"].append({"name": m, "category": "lifecycle", "trigger": f"{m} (오버라이드)", "owner": c["name"],
                                        "source": "cpp", "where": f"{body['file']}:{body['line']}", "estimated": True,
                                        "steps": cpp.method(pref, m, 0, set())})
                used.add(m)
        for m in methods:
            cat = CPP_OVERRIDES.get(m)
            if cat and m not in used and cat != "input":
                body = index.first(pref, m)
                cls["features"].append({"name": m.replace("_Implementation", ""), "category": cat, "trigger": f"{m} (오버라이드)",
                                        "owner": c["name"], "source": "cpp", "where": f"{body['file']}:{body['line']}",
                                        "estimated": True, "steps": cpp.method(pref, m, 0, set())})
                used.add(m)
        for m in methods:
            for body in index.bodies[(pref, m)]:
                code = body["code"]
                where = body["file"]
                for b in re.finditer(r"BindAction\(\s*([\w\.\->]+)\s*,\s*ETriggerEvent::(\w+)\s*,\s*this\s*,\s*&\s*\w+::(\w+)", code):
                    action, trig, fn = b.groups()
                    cls["features"].append({"name": f"{pretty_action(action)} · {trig}", "category": "input", "trigger": f"{action} · {trig}",
                                            "owner": c["name"], "source": "cpp", "estimated": True,
                                            "where": f"{where}:{body['line'] + code.count(chr(10), 0, b.start())}",
                                            "steps": [{"kind": "call", "text": fn, "estimated": True, "inner": cpp.method(pref, fn, 1, set())}]})
                    used.add(fn)
                for b in re.finditer(r"Bind(Action|Axis)\(\s*(?:TEXT\()?\"([^\"]+)\"\)?\s*,\s*(?:(IE_\w+)\s*,\s*)?this\s*,\s*&\s*\w+::(\w+)", code):
                    kind_, action, trig, fn = b.groups()
                    cls["features"].append({"name": f"{action}" + (f" · {trig[3:]}" if trig else ""), "category": "input",
                                            "trigger": f"{action}" + (f" · {trig}" if trig else " (축)"), "owner": c["name"], "source": "cpp",
                                            "estimated": True, "where": f"{where}:{body['line'] + code.count(chr(10), 0, b.start())}",
                                            "steps": [{"kind": "call", "text": fn, "estimated": True, "inner": cpp.method(pref, fn, 1, set())}]})
                    used.add(fn)
                for b in re.finditer(r"(\w+)\s*(?:->|\.)\s*(On\w+)\s*\.\s*(?:AddDynamic|AddUniqueDynamic|AddUObject)\(\s*this\s*,\s*&\s*\w+::(\w+)", code):
                    obj, delegate, fn = b.groups()
                    cat = "collision" if any(w in delegate for w in COLLISION_WORDS) else "ui" if delegate.startswith(UI_DELEGATES) else "timer"
                    cls["features"].append({"name": f"{obj}.{delegate}", "category": cat, "trigger": f"{obj}.{delegate} → {fn}",
                                            "owner": c["name"], "source": "cpp", "estimated": True,
                                            "where": f"{where}:{body['line'] + code.count(chr(10), 0, b.start())}",
                                            "steps": [{"kind": "call", "text": fn, "estimated": True, "inner": cpp.method(pref, fn, 1, set())}]})
                    used.add(fn)
                for b in re.finditer(r"SetTimer\(\s*[^,]+,\s*this\s*,\s*&\s*\w+::(\w+)\s*,\s*([^,\)]+)", code):
                    fn, rate = b.groups()
                    cls["features"].append({"name": f"타이머 → {fn}", "category": "timer", "trigger": f"SetTimer({_short_text(rate, 20)})",
                                            "owner": c["name"], "source": "cpp", "estimated": True,
                                            "where": f"{where}:{body['line'] + code.count(chr(10), 0, b.start())}",
                                            "steps": [{"kind": "call", "text": fn, "estimated": True, "inner": cpp.method(pref, fn, 1, set())}]})
                    used.add(fn)
        for f in c.get("functions", []):
            flags, name = f.get("flags", []), f["name"]
            body_name = name + "_Implementation" if index.has(pref, name + "_Implementation") else name
            if not index.has(pref, body_name) or name in used:
                continue
            body = index.first(pref, body_name)
            cat = "network" if any(x.startswith("Net") and x != "Native" for x in flags) or name.startswith("OnRep_") else \
                  "public" if "BlueprintCallable" in flags and "BlueprintPure" not in flags else None
            if not cat:
                continue
            trig = "RPC " + ", ".join(x for x in flags if x.startswith("Net") and x != "Native") if cat == "network" and not name.startswith("OnRep_") else \
                   "복제 값 변경" if name.startswith("OnRep_") else "Blueprint에서 호출"
            cls["features"].append({"name": name, "category": cat, "trigger": trig, "owner": c["name"], "source": "cpp", "estimated": True,
                                    "where": f"{body['file']}:{body['line']}", "steps": cpp.method(pref, body_name, 0, set())})
        for m in methods:
            if m.startswith("OnRep_") and m not in {f["name"] for f in cls["features"]}:
                body = index.first(pref, m)
                cls["features"].append({"name": m, "category": "network", "trigger": "복제 값 변경", "owner": c["name"], "source": "cpp",
                                        "estimated": True, "where": f"{body['file']}:{body['line']}", "steps": cpp.method(pref, m, 0, set())})

    # ---- Blueprints
    for bp in blueprints:
        gen = normalize_class(bp["generatedClass"])
        name = short(gen)
        cls = entry(class_key(gen), name, "blueprint")
        cls["parent"] = class_key(normalize_class(bp.get("parent", ""))) if bp.get("parent") else ""
        parents, p = [], normalize_class(bp.get("parent", ""))
        while p in native:
            parents.append(p)
            p = normalize_class(native[p].get("super", ""))
        flow = BpFlow(bp, bp_by_class, cpp, prefixed_of, parents)
        for g in bp["graphs"]:
            if g["kind"] not in ("event", "function"):
                continue
            for n in g["nodes"]:
                if n["class"] not in ENTRY_NODES:
                    continue
                if n["class"] == "K2Node_FunctionEntry":
                    title = "Construction Script" if n.get("title") == "Construction Script" else g["name"]
                    trigger = "엔진이 배치·변경 시 실행" if title == "Construction Script" else "함수"
                else:
                    title = n.get("title", "")
                    owner = normalize_class(n.get("memberOwner", ""))
                    trigger = {"K2Node_ComponentBoundEvent": "컴포넌트 이벤트", "K2Node_CustomEvent": "커스텀 이벤트"}.get(n["class"], "")
                    if not trigger:
                        trigger = ("입력" if n["class"] in INPUT_NODES else
                                   f"{short(owner)} 구현" if owner and owner != normalize_class(bp.get("parent", "")) else "이벤트")
                steps = flow.graph_steps(g, n["id"], 0, set())
                if not steps:
                    continue
                cls["features"].append({"name": title, "category": bp_category(n, interfaces), "trigger": trigger, "owner": name,
                                        "source": "bp", "where": f"{bp['asset'].split('.')[0]} · {g['name']}", "steps": steps})

    order = {c: i for i, (c, _) in enumerate(CATEGORIES)}
    for cls in classes.values():
        cls["features"].sort(key=lambda f: (order.get(f["category"], 99), f["name"]))
        for i, f in enumerate(cls["features"]):
            f["id"] = f"{cls['key']}#{i}"
    out = [c for c in classes.values() if c["features"]]
    return {"schema": SCHEMA, "categories": [{"id": c, "name": n} for c, n in CATEGORIES], "classes": sorted(out, key=lambda c: c["key"])}


# ============================================================================ HTML report section

STEP_LABELS = {"call": "호출", "set": "값 설정", "branch": "분기", "sequence": "순서대로", "loop": "반복", "cast": "형변환",
               "spawn": "스폰", "widget": "위젯", "timer": "타이머", "dispatch": "디스패처", "bind": "연결", "macro": "매크로",
               "other": "노드"}
REPORT_CSS = """
.feat-class>summary b{color:var(--ink)}.feat-cat{margin:12px 0 4px;font-size:13px;color:var(--ink3)}
.feat{border-top:none;margin:2px 0 2px 4px;padding-top:2px}.feat>summary{color:var(--ink)}
ol.flow,ul.br{margin:4px 0 6px;padding-left:22px}ol.flow li{margin:1px 0}ul.br{list-style:none;padding-left:14px}
.k{display:inline-block;min-width:5.2em;color:var(--ink3);font:12px Consolas,monospace}.lbl{color:var(--det);font-weight:600}
.inner{border-left:2px dashed var(--rule);margin-left:4px}.est{color:var(--ink3);font-size:11px}
"""


def render_html(doc: dict, max_classes: int = 400) -> str:
    import html as _html
    esc = _html.escape
    cats = dict((c["id"], c["name"]) for c in doc.get("categories", []))

    def steps_html(steps: list[dict], cls: str = "flow") -> str:
        items = []
        for s in steps:
            text = esc(s["text"]) + (f" ×{s['repeat']}" if s.get("repeat", 1) > 1 else "")
            li = f"<span class='k'>{STEP_LABELS.get(s['kind'], s['kind'])}</span>{text}"
            if s.get("branches"):
                li += "<ul class='br'>" + "".join(
                    f"<li><span class='lbl'>{esc(b['label'])}</span>{steps_html(b['steps']) if b['steps'] else ' <span class=est>(없음)</span>'}</li>"
                    for b in s["branches"]) + "</ul>"
            if s.get("inner"):
                li += steps_html(s["inner"], "flow inner")
            items.append(f"<li>{li}</li>")
        return f"<ol class='{cls}'>{''.join(items)}</ol>"

    classes = doc.get("classes", [])
    parts = []
    for c in classes[:max_classes]:
        groups = defaultdict(list)
        for f in c["features"]:
            groups[f["category"]].append(f)
        body = []
        for cat, name in cats.items():
            if cat not in groups:
                continue
            body.append(f"<div class='feat-cat'>{esc(name)} · {len(groups[cat])}</div>")
            for f in groups[cat]:
                est = " <span class='est'>C++ 소스 추정</span>" if f.get("estimated") else ""
                flow = steps_html(f["steps"]) if f["steps"] else "<p class='meta'>단계 없음</p>"
                body.append(f"<details class='feat'><summary><b>{esc(f['name'])}</b> <span class='meta'>{esc(f['trigger'])} · "
                            f"{esc(f['where'])}</span>{est}</summary>{flow}</details>")
        kind = "Blueprint" if c["kind"] == "blueprint" else "C++"
        parts.append(f"<details class='feat-class'><summary><b>{esc(c['name'])}</b> <span class='meta'>{kind} · 기능 {len(c['features'])}개</span>"
                     f"</summary>{''.join(body)}</details>")
    more = f"<p class='meta'>클래스 {len(classes)}개 중 {max_classes}개만 표시했습니다. 전체는 features.json에 있습니다.</p>" if len(classes) > max_classes else ""
    total = sum(len(c["features"]) for c in classes)
    return (f"<section><h2>기능</h2><p class='meta'>클래스 {len(classes)}개 · 기능 {total}개. 진입점(이벤트, 입력, 오버라이드, 바인딩, 공개 함수)별로 "
            f"나누고 각 기능이 실행하는 단계를 펼쳤습니다. C++ 단계는 소스 패턴으로 읽은 추정입니다.</p>{''.join(parts) or '<p class=meta>기능 없음</p>'}{more}</section>")
