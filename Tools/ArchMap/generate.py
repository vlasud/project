#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Сборщик карты архитектуры гейммода.

Читает Src/, Docs/ и Sql/ и складывает всё, что нужно приложению, в data.js
(глобал window.ARCH). Именно .js, а не .json: страница открывается прямо с диска
по file://, а fetch() оттуда браузер запрещает.

Карта СОБИРАЕТСЯ ИЗ КОДА, а не пишется руками, — иначе она разъедется с проектом на
первой же правке. Всё, что тут извлекается, опирается на конвенции проекта:
  * модуль        — папка в Src/Services или Src/Systems с парой X.h/X.cpp;
  * слой          — Core, если путь содержит /Core/;
  * описание      — блок // прямо над `class X`, в проекте он всегда содержательный;
  * зависимости   — getService<X>() и члены `XService &m_...`;
  * настройки     — static constexpr в заголовке и constexpr в анон-namespace .cpp;
  * команды       — add("имя", ..., "описание", HelpCategory::Категория);
  * саморегистрация — registerItem/addGood/addService/registerDispatch/registerType;
  * таблицы БД    — getTable("x") и SQL-строки.

Запуск:  python Tools/ArchMap/generate.py
"""

import io
import json
import os
import re
import sys
from datetime import datetime

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "Src")
DOCS = os.path.join(ROOT, "Docs")
SQL = os.path.join(ROOT, "Sql")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data.js")


def read(path):
    with io.open(path, encoding="utf-8", errors="replace") as handle:
        return handle.read()


# ----------------------------------------------------------------- извлечение

def leading_comment(header_text, class_name):
    """Блок // прямо над объявлением класса — человеческое описание модуля."""
    match = re.search(r"^class\s+%s\b" % re.escape(class_name), header_text, re.M)
    if not match:
        return ""
    lines = header_text[: match.start()].split("\n")
    collected = []
    for line in reversed(lines):
        stripped = line.strip()
        if stripped.startswith("//"):
            collected.append(stripped[2:].strip())
            continue
        if not stripped and collected:
            break  # пустая строка выше блока — конец описания
        if not stripped:
            continue
        break
    return "\n".join(reversed(collected)).strip()


CONST_RE = re.compile(
    r"static\s+constexpr\s+(?P<type>[\w:<>,\s\*&]+?)\s+(?P<name>[A-Z][A-Z0-9_]*)\s*(?:=|\{)\s*(?P<value>[^;]+);"
)
CPP_CONST_RE = re.compile(
    r"^(?:const|constexpr)\s+(?P<type>[\w:<>,\s\*&]+?)\s+(?P<name>[A-Z][A-Z0-9_]*)\s*(?:=|\{)\s*(?P<value>[^;]+);",
    re.M,
)


def clean_value(raw):
    value = raw.strip().rstrip("}").strip()
    value = re.sub(r"\s+", " ", value)
    return value[:80]


def constant_note(text, start_pos, end_pos):
    """Смысл константы: комментарий в конце строки, а если его нет — блок // над ней.
    В проекте встречаются оба стиля, и терять второй нельзя: там как раз объяснения
    балансовых чисел."""
    line_end = text.find("\n", end_pos)
    tail = text[end_pos : line_end if line_end != -1 else len(text)]
    match = re.search(r"//\s*(.+)$", tail)
    if match:
        return match.group(1).strip()

    lines = text[:start_pos].split("\n")[:-1]
    collected = []
    for line in reversed(lines):
        stripped = line.strip()
        if stripped.startswith("//"):
            collected.append(stripped[2:].strip())
            continue
        break
    return " ".join(reversed(collected)).strip()


def constants_of(text, regex, scope):
    found = []
    for match in regex.finditer(text):
        name = match.group("name")
        if name in ("MAX_PLAYERS",):
            continue
        found.append(
            {
                "name": name,
                "type": re.sub(r"\s+", " ", match.group("type")).strip(),
                "value": clean_value(match.group("value")),
                "note": constant_note(text, match.start(), match.end()),
                "scope": scope,
            }
        )
    return found


COMMAND_RE = re.compile(r"\.add\(\s*\n?\s*\"(?P<name>[a-z0-9_]+)\"")
CATEGORY_RE = re.compile(r"HelpCategory::(\w+)")
STRING_RE = re.compile(r"\"([^\"\\]{4,})\"")
ADMIN_RE = re.compile(r"PermissionSpec::admin\((\w+[^)]*)\)")


def commands_of(text):
    """Команды игрока: имя, описание и раздел /help — то, что видно снаружи."""
    result = []
    for match in COMMAND_RE.finditer(text):
        window = text[match.end() : match.end() + 1600]
        category = CATEGORY_RE.search(window)
        if not category:
            continue
        head = window[: category.start()]
        strings = STRING_RE.findall(head)
        description = strings[-1] if strings else ""
        admin = ADMIN_RE.search(head)
        result.append(
            {
                "name": match.group("name"),
                "description": description,
                "category": category.group(1),
                "admin": admin.group(1) if admin else "",
            }
        )
    return result


REGISTRATION_PATTERNS = [
    ("предмет", re.compile(r"registerItem\(\s*([A-Za-z_][\w:]*)")),
    ("товар точки", re.compile(r"addGood\(\s*[\w:]*::(\w+)")),
    ("услуга точки", re.compile(r"addService\(\s*[\w:]*::(\w+)")),
    ("служба телефона", re.compile(r"registerDispatch\(\s*[\w:]*::(\w+)")),
    ("тип бизнеса", re.compile(r"registerType\(\s*[\w:]*::(\w+)")),
    ("фракция", re.compile(r"registerFaction\(\s*([A-Za-z_][\w:]*)")),
    ("работа", re.compile(r"registerJob\(\s*([A-Za-z_][\w:]*)")),
]

HOOK_PATTERNS = [
    ("старт сессии", "subscribeStart"),
    ("сохранение", "subscribeSave"),
    ("конец сессии", "subscribeEnd"),
    ("смерть", "subscribeDeath"),
    ("таймер", "setInterval"),
    ("отложенно", "setTimeout"),
    ("пер-плеерный таймер", "setPlayerInterval"),
    ("диалоги", "m_dialogService.show"),
    ("числовой диалог", "showNumberInput"),
    ("речь в чате", "subscribeSpeech"),
]

TABLE_RES = [
    re.compile(r"getTable\(\s*\"(\w+)\""),
    re.compile(r"\bFROM\s+`?(\w+)`?", re.I),
    re.compile(r"\bINTO\s+`?(\w+)`?", re.I),
    re.compile(r"\bUPDATE\s+`?(\w+)`?", re.I),
]


def tables_of(text, known):
    found = set()
    for regex in TABLE_RES:
        for name in regex.findall(text):
            if name in known:
                found.add(name)
    return sorted(found)


def module_paths():
    for kind, folder in (("service", "Services"), ("system", "Systems")):
        base = os.path.join(SRC, folder)
        for dirpath, _dirnames, filenames in os.walk(base):
            headers = [f for f in filenames if f.endswith(".h")]
            for header in headers:
                name = header[:-2]
                if not os.path.exists(os.path.join(dirpath, name + ".cpp")):
                    continue
                if os.path.basename(dirpath) != name:
                    continue  # модуль = папка со своим именем; служебные хедеры мимо
                yield kind, name, dirpath


def collect_modules(known_tables):
    modules = {}
    for kind, name, dirpath in module_paths():
        header = read(os.path.join(dirpath, name + ".h"))
        source = read(os.path.join(dirpath, name + ".cpp"))
        both = header + "\n" + source

        deps = set(re.findall(r"getService<(\w+)>", both))
        deps |= set(re.findall(r"^\s{2,}(\w+Service)\s*&\s*m_\w+;", header, re.M))
        deps.discard(name)

        registrations = []
        for label, regex in REGISTRATION_PATTERNS:
            for value in regex.findall(both):
                registrations.append({"what": label, "value": value})

        hooks = [label for label, needle in HOOK_PATTERNS if needle in both]

        modules[name] = {
            "name": name,
            "kind": kind,
            "layer": "core" if os.sep + "Core" + os.sep in dirpath + os.sep else "business",
            "path": os.path.relpath(dirpath, ROOT).replace("\\", "/"),
            "summary": leading_comment(header, name),
            "loc": len(header.split("\n")) + len(source.split("\n")),
            "deps": sorted(deps),
            "constants": constants_of(header, CONST_RE, "public")
            + constants_of(source, CPP_CONST_RE, "internal"),
            "commands": commands_of(source),
            "registrations": registrations,
            "hooks": hooks,
            "tables": tables_of(both, known_tables),
            "docs": [],
        }
    return modules


def registration_order(path, regex):
    if not os.path.exists(path):
        return []
    return re.findall(regex, read(path))


def collect_tables():
    path = os.path.join(SQL, "schema_all.sql")
    if not os.path.exists(path):
        return []
    text = read(path)
    tables = []
    for name in re.findall(r"CREATE TABLE IF NOT EXISTS\s+`(\w+)`", text):
        block = text.split("`%s`" % name, 1)[1]
        columns = re.findall(r"^\s+`(\w+)`\s+([A-Z]+[\w()]*)", block.split(");", 1)[0], re.M)
        tables.append({"name": name, "columns": [{"name": c, "type": t} for c, t in columns]})
    return tables


def attach_docs(modules):
    """Док привязывается к модулю, если упоминает его имя. Явных ссылок в коде нет."""
    docs = []
    if not os.path.isdir(DOCS):
        return docs
    for dirpath, _dirnames, filenames in os.walk(DOCS):
        for filename in sorted(filenames):
            if not filename.endswith(".md"):
                continue
            path = os.path.join(dirpath, filename)
            text = read(path)
            rel = os.path.relpath(path, ROOT).replace("\\", "/")
            title = ""
            for line in text.split("\n"):
                if line.startswith("# "):
                    title = line[2:].strip()
                    break
            entry = {"file": rel, "title": title or filename, "lines": len(text.split("\n")), "modules": []}
            for name, module in modules.items():
                if name in text:
                    entry["modules"].append(name)
                    module["docs"].append(rel)
            docs.append(entry)
    return docs


def server_dir():
    """Папка сервера лежит рядом с проектом (D:\\workspace\\Server), а не внутри него."""
    for candidate in (os.path.join(ROOT, "Server"), os.path.join(os.path.dirname(ROOT), "Server")):
        if os.path.isdir(candidate):
            return candidate
    return None


def load_json(path):
    try:
        return json.loads(read(path))
    except Exception as error:  # битый или недописанный файл не должен ронять сборку
        print("не прочитан %s: %s" % (path, error))
        return None


def business_type_names():
    """Порядок enum Type = число в businesses.json; человеческое имя — из registerType."""
    header = os.path.join(SRC, "Services", "BusinessService", "BusinessService.h")
    if not os.path.exists(header):
        return []
    block = re.search(r"enum class Type\s*\{(.+?)\}", read(header), re.S)
    if not block:
        return []
    names = []
    for line in block.group(1).split("\n"):
        match = re.match(r"\s*(\w+)\s*,", line)
        if match and match.group(1) != "Count":
            names.append(match.group(1))

    titles = {}
    for dirpath, _dirnames, filenames in os.walk(os.path.join(SRC, "Systems")):
        for filename in filenames:
            if not filename.endswith(".cpp"):
                continue
            for enum_name, title in re.findall(r"registerType\(\s*[\w:]*::(\w+)\s*,\s*\"([^\"]+)\"",
                                               read(os.path.join(dirpath, filename))):
                titles[enum_name] = title
    return [{"key": n, "title": titles.get(n, n)} for n in names]


def collect_bus_route():
    """Маршрут автобуса — таблица RouteNode в коде работы, а не контент сервера.
    Кольцевой: после последнего чекпоинта снова первый."""
    path = os.path.join(SRC, "Systems", "BusJobSystem", "BusJobSystem.cpp")
    if not os.path.exists(path):
        return []
    block = re.search(r"RouteNode\s+ROUTE\[[^\]]*\]\s*=\s*\{(.+?)\n\};", read(path), re.S)
    if not block:
        return []
    nodes = []
    for x, y, _z, stop in re.findall(
            r"\{\s*\{\s*(-?[\d.]+)f\s*,\s*(-?[\d.]+)f\s*,\s*(-?[\d.]+)f\s*\}\s*,\s*(true|false)\s*\}",
            block.group(1)):
        nodes.append({"x": float(x), "y": float(y), "stop": stop == "true"})
    return nodes


def collect_world():
    """Точки мира для карты: дома и бизнесы — дев-контент из json сервера."""
    world = {"houses": [], "businesses": [], "businessTypes": business_type_names(), "places": [],
             "busRoute": collect_bus_route()}

    catalog = os.path.join(SRC, "Services", "PlaceCatalogService", "PlaceCatalogService.cpp")
    if os.path.exists(catalog):
        for name, x, y in re.findall(
                r"\{\"([^\"]+)\",\s*\{\s*(-?[\d.]+)f,\s*(-?[\d.]+)f", read(catalog)):
            world["places"].append({"name": name, "x": float(x), "y": float(y)})

    server = server_dir()
    if not server:
        return world

    houses = load_json(os.path.join(server, "houses.json"))
    for house in houses or []:
        position = house.get("entrance") or []
        if len(position) >= 2:
            world["houses"].append({
                "id": house.get("id"), "x": position[0], "y": position[1],
                "price": house.get("price", 0), "parking": house.get("parkingCap", 0),
            })

    raw = load_json(os.path.join(server, "businesses.json")) or {}
    for business in (raw.get("businesses") if isinstance(raw, dict) else raw) or []:
        position = business.get("entrance") or []
        if len(position) >= 2:
            world["businesses"].append({
                "id": business.get("id"), "x": position[0], "y": position[1],
                "price": business.get("price", 0), "type": business.get("type", 0),
                "balance": business.get("balance", 0),
            })
    return world


def main():
    tables = collect_tables()
    known = {t["name"] for t in tables}
    modules = collect_modules(known)
    docs = attach_docs(modules)

    # Обратные связи считаем здесь, а не в браузере: «кто на меня опирается» — главный
    # вопрос при оценке риска правки, и он нужен сразу.
    for module in modules.values():
        module["usedBy"] = sorted(
            other["name"] for other in modules.values() if module["name"] in other["deps"]
        )
        module["deps"] = [d for d in module["deps"] if d in modules]

    payload = {
        "generatedAt": datetime.now().strftime("%d.%m.%Y %H:%M"),
        "modules": sorted(modules.values(), key=lambda m: m["name"]),
        "serviceOrder": registration_order(
            os.path.join(SRC, "Services", "ServiceRegister.cpp"), r"registerService<(\w+)>"
        ),
        "systemOrder": registration_order(
            os.path.join(SRC, "Systems", "SystemRegister.cpp"), r"make_unique<(\w+)>"
        ),
        "tables": tables,
        "docs": docs,
        "world": collect_world(),
    }

    body = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    with io.open(OUT, "w", encoding="utf-8") as handle:
        handle.write("// Сгенерировано Tools/ArchMap/generate.py — руками не править.\n")
        handle.write("window.ARCH = " + body + ";\n")

    # Версия в теге <script>: без неё браузер отдаёт закэшированный data.js, и после
    # пересборки карта молча показывает вчерашний проект.
    page = os.path.join(os.path.dirname(OUT), "index.html")
    if os.path.exists(page):
        html = read(page)
        stamp = datetime.now().strftime("%Y%m%d%H%M%S")
        patched = re.sub(r'<script src="data\.js[^"]*"></script>',
                         '<script src="data.js?v=%s"></script>' % stamp, html)
        if patched != html:
            with io.open(page, "w", encoding="utf-8") as handle:
                handle.write(patched)

    print("модулей: %d, команд: %d, констант: %d, таблиц: %d, доков: %d"
          % (len(modules),
             sum(len(m["commands"]) for m in modules.values()),
             sum(len(m["constants"]) for m in modules.values()),
             len(tables), len(docs)))
    print("на карте: домов %d, бизнесов %d, ориентиров %d, чекпоинтов автобуса %d"
          % (len(payload["world"]["houses"]), len(payload["world"]["businesses"]),
             len(payload["world"]["places"]), len(payload["world"]["busRoute"])))
    print("записано: " + os.path.relpath(OUT, ROOT).replace("\\", "/"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
