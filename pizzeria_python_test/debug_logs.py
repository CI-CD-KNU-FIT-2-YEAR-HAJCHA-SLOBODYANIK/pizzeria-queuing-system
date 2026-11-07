"""Просмотрщик технических уведомлений C++-симуляций.

Запуск: python debug_logs.py
Нужен только Python с Tkinter. Не изменяет исходные JSON-отчёты.
"""

from __future__ import annotations

import json
import re
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
import tkinter as tk
from tkinter import filedialog, messagebox, ttk


SEVERITY_NAMES = {1: "1 — Рекомендации", 2: "2 — Предупреждения", 3: "3 — Критические"}
SEVERITY_COLORS = {1: "#187a43", 2: "#aa7100", 3: "#bc3232"}
CHECKED, UNCHECKED = "☑", "☐"
FILE_NAME = re.compile(r"^(?P<session>\d{8}_\d{4})_g(?P<generation>\d+)_r\d+$")


@dataclass(frozen=True)
class Notice:
    run_id: str
    generation: str
    level: int
    code: str
    timestamp: object
    thread_id: object
    message: str
    recommendation: str
    source: Path


@dataclass
class Run:
    run_id: str
    generation: str
    session: str
    status: str
    failure: str
    path: Path
    notices: list[Notice]


def detect_folder(script_path: Path) -> Path:
    """Находит generated/reports относительно Python-файла или рабочего каталога."""
    candidates = []
    for base in (script_path.parent, Path.cwd()):
        candidates += [base / "generated" / "reports",
                       base / "pizzeria_python_test" / "generated" / "reports",
                       base / "reports"]
        for parent in list(base.parents)[:3]:
            candidates.append(parent / "pizzeria_python_test" / "generated" / "reports")
    for folder in candidates:
        if folder.is_dir():
            return folder.resolve()
    return (script_path.parent / "generated" / "reports").resolve()


def load_reports(folder: Path) -> tuple[list[Run], list[str]]:
    """Сканирует только настоящие run-отчёты, не analysis_*.json и manifests."""
    runs: list[Run] = []
    errors: list[str] = []
    if not folder.is_dir():
        return runs, [f"Папка не существует: {folder}"]
    for path in sorted(folder.rglob("*.json")):
        if path.name.startswith("analysis_") or path.name.startswith("manifest_"):
            continue
        try:
            with path.open("r", encoding="utf-8-sig") as f:
                data = json.load(f)
            if not isinstance(data, dict) or not isinstance(data.get("results"), dict):
                continue
            rid = str(data.get("run_id") or path.stem)
            match = FILE_NAME.match(rid)
            # Prefer session+generation so different sessions' g001 aren't mixed.
            session = match.group("session") if match else path.parent.name.removeprefix("reports_").split("_g")[0]
            generation = (f"{session} / g{int(match.group('generation')):03d}"
                          if match else path.parent.name.removeprefix("reports_"))
            notices: list[Notice] = []
            raw = data["results"].get("notifications") or []
            for row in raw:
                if not isinstance(row, dict):
                    continue
                try:
                    level = int(row.get("severity", 0))
                except (ValueError, TypeError):
                    level = 0
                code = str(row.get("code") or "(без кода)")
                notices.append(Notice(
                    run_id=rid, generation=generation, level=level, code=code,
                    timestamp=row.get("timestamp_sec", "—"),
                    thread_id=row.get("thread_id", "—"),
                    message=str(row.get("message") or ""),
                    recommendation=str(row.get("recommendation") or ""),
                    source=path,
                ))
            failure = data.get("failure")
            if isinstance(failure, dict):
                failure = json.dumps(failure, ensure_ascii=False)
            runs.append(Run(rid, generation, session, str(data.get("status", "unknown")),
                            str(failure or ""), path, notices))
        except (OSError, UnicodeError, ValueError, TypeError) as exc:
            errors.append(f"{path}: {exc}")
    runs.sort(key=lambda x: (x.generation, x.run_id))
    return runs, errors


class DebugViewer(tk.Tk):
    def __init__(self, folder: Path):
        super().__init__()
        self.title("Pizzeria — Debug Logs")
        self.geometry("1400x820")
        self.minsize(990, 580)
        self.folder = folder
        self.runs: list[Run] = []
        self.errors: list[str] = []
        self.run_map: dict[str, Run] = {}
        self.scope_map: dict[str, tuple[str, str]] = {}
        self.notices: list[Notice] = []
        self.visible: list[Notice] = []
        self.code_enabled: dict[tuple[int, str], bool] = {}
        self.checkbox_map: dict[str, tuple[int, str | None]] = {}
        self.level_enabled: dict[int, bool] = {1: True, 2: True, 3: True, 0: True}
        self.find_text = tk.StringVar()
        self.folder_text = tk.StringVar(value=str(folder))
        self.counter = tk.StringVar(value="Нет загруженных отчётов")
        self._build_ui()
        self.reload()

    def _build_ui(self):
        bar = ttk.Frame(self, padding=(8, 8))
        bar.pack(fill="x")
        ttk.Label(bar, text="Отчёты:").pack(side="left")
        ttk.Entry(bar, textvariable=self.folder_text).pack(side="left", fill="x", expand=True, padx=(7, 5))
        ttk.Button(bar, text="Папка…", command=self.choose_folder).pack(side="left", padx=3)
        ttk.Button(bar, text="Открыть JSON…", command=self.open_file).pack(side="left", padx=3)
        ttk.Button(bar, text="Обновить", command=self.reload).pack(side="left", padx=3)

        body = ttk.Panedwindow(self, orient="horizontal")
        body.pack(fill="both", expand=True, padx=8, pady=(0, 6))

        left = ttk.Frame(body)
        body.add(left, weight=3)
        ttk.Label(left, text="Попытки / итерации / симуляции", padding=(0, 0, 0, 4)).pack(anchor="w")
        self.scopes = ttk.Treeview(left, show="tree", selectmode="browse")
        self.scopes.pack(fill="both", expand=True)
        self.scopes.bind("<<TreeviewSelect>>", lambda _e: self.scope_changed())
        ttk.Label(left, text="Показываются сохранённые JSON-отчёты", foreground="gray").pack(anchor="w", pady=(4, 0))

        center = ttk.Panedwindow(body, orient="vertical")
        body.add(center, weight=8)

        filters_frame = ttk.Frame(center)
        center.add(filters_frame, weight=2)
        ttk.Label(filters_frame, text="Уровни → коды (клик по ☑ / ☐ или строке)").pack(anchor="w")
        treebox = ttk.Frame(filters_frame)
        treebox.pack(fill="both", expand=True, pady=(4, 0))
        self.filters = ttk.Treeview(treebox, columns=("count",), show="tree headings")
        self.filters.heading("#0", text="Фильтр")
        self.filters.heading("count", text="Всего")
        self.filters.column("count", width=80, minwidth=55, anchor="e", stretch=False)
        self.filters.pack(side="left", fill="both", expand=True)
        yscroll = ttk.Scrollbar(treebox, orient="vertical", command=self.filters.yview)
        yscroll.pack(side="right", fill="y")
        self.filters.configure(yscrollcommand=yscroll.set)
        self.filters.bind("<ButtonRelease-1>", self.toggle_filter)
        fbuttons = ttk.Frame(filters_frame)
        fbuttons.pack(fill="x", pady=(3, 0))
        ttk.Button(fbuttons, text="Все коды", command=lambda: self.set_all_filters(True)).pack(side="left")
        ttk.Button(fbuttons, text="Снять все", command=lambda: self.set_all_filters(False)).pack(side="left", padx=5)
        ttk.Button(fbuttons, text="Только критические", command=self.only_critical).pack(side="left")

        results = ttk.Frame(center)
        center.add(results, weight=5)
        searchbar = ttk.Frame(results)
        searchbar.pack(fill="x", pady=(0, 4))
        ttk.Label(searchbar, text="Поиск по коду / сообщению:").pack(side="left")
        ttk.Entry(searchbar, textvariable=self.find_text).pack(side="left", expand=True, fill="x", padx=6)
        ttk.Button(searchbar, text="Скопировать строки", command=self.copy_rows).pack(side="right")
        self.find_text.trace_add("write", lambda *_: self.update_results())

        gridframe = ttk.Frame(results)
        gridframe.pack(fill="both", expand=True)
        cols = ("run", "level", "code", "time", "thread", "message")
        self.table = ttk.Treeview(gridframe, columns=cols, show="headings", selectmode="extended")
        for c, title, width in [("run", "Прогон", 190), ("level", "Ур.", 43),
                                ("code", "Код", 185), ("time", "Сек.", 75),
                                ("thread", "Поток", 65), ("message", "Сообщение", 580)]:
            self.table.heading(c, text=title)
            self.table.column(c, width=width, minwidth=40, stretch=c == "message")
        for k, v in SEVERITY_COLORS.items():
            self.table.tag_configure(f"level{k}", foreground=v)
        self.table.pack(side="left", fill="both", expand=True)
        gridscroll = ttk.Scrollbar(gridframe, orient="vertical", command=self.table.yview)
        gridscroll.pack(side="right", fill="y")
        self.table.configure(yscrollcommand=gridscroll.set)
        self.table.bind("<<TreeviewSelect>>", lambda _e: self.show_detail())

        detail_frame = ttk.Frame(center)
        center.add(detail_frame, weight=3)
        detailbar = ttk.Frame(detail_frame)
        detailbar.pack(fill="x")
        ttk.Label(detailbar, text="Подробности выбранного уведомления").pack(side="left")
        ttk.Button(detailbar, text="Копировать", command=self.copy_detail).pack(side="right")
        self.detail = tk.Text(detail_frame, height=8, wrap="word", font=("Consolas", 10), undo=False)
        self.detail.pack(fill="both", expand=True, pady=(4, 0))
        self.detail.configure(state="disabled")

        status = ttk.Frame(self, padding=(8, 0, 8, 8))
        status.pack(fill="x")
        ttk.Label(status, textvariable=self.counter).pack(side="left", fill="x", expand=True)
        ttk.Button(status, text="Копировать все видимые", command=lambda: self.copy_rows(all_rows=True)).pack(side="right")
        ttk.Button(status, text="Экспорт TXT…", command=self.export_txt).pack(side="right", padx=5)

    def choose_folder(self):
        path = filedialog.askdirectory(initialdir=self.folder if self.folder.exists() else Path.cwd(),
                                      title="Выберите папку generated/reports или её родителя")
        if path:
            self.folder_text.set(path)
            self.reload()

    def open_file(self):
        path = filedialog.askopenfilename(title="Открыть отчёт симуляции", filetypes=[("JSON", "*.json")],
                                          initialdir=self.folder if self.folder.exists() else Path.cwd())
        if path:
            self.folder_text.set(str(Path(path).parent))
            self.reload()
            wanted = Path(path).stem
            iid = f"run:{wanted}"
            if self.scopes.exists(iid):
                self.scopes.selection_set(iid)
                self.scopes.see(iid)
                self.scope_changed()

    def reload(self):
        self.folder = Path(self.folder_text.get()).expanduser().resolve()
        self.runs, self.errors = load_reports(self.folder)
        # Folder may have reports in nested reports_... directories; leave loaded scope on refresh if present.
        selected = self.scopes.selection()[0] if self.scopes.selection() else "all"
        self.scopes.delete(*self.scopes.get_children())
        self.scope_map.clear()
        self.run_map = {r.run_id: r for r in self.runs}
        notice_count = sum(len(r.notices) for r in self.runs)
        self.scopes.insert("", "end", iid="all", text=f"Все попытки ({len(self.runs)} прогонов, {notice_count} уведомлений)", open=True)
        self.scope_map["all"] = ("all", "")
        by_session: dict[str, dict[str, list[Run]]] = {}
        for run in self.runs:
            by_session.setdefault(run.session, {}).setdefault(run.generation, []).append(run)
        for session, generations in sorted(by_session.items(), reverse=True):
            sid = f"session:{session}"
            session_runs = [r for group in generations.values() for r in group]
            self.scopes.insert("all", "end", iid=sid,
                               text=f"Сессия {session} — {len(session_runs)} прогонов",
                               open=True)
            self.scope_map[sid] = ("session", session)
            for gen, runs in sorted(generations.items()):
                gid = f"gen:{gen}"
                self.scopes.insert(sid, "end", iid=gid,
                                   text=f"{gen.split(' / ')[-1]} — {len(runs)} прогонов, {sum(len(r.notices) for r in runs)} уведомлений",
                                   open=True)
                self.scope_map[gid] = ("gen", gen)
                for run in runs:
                    iid = f"run:{run.run_id}"
                    self.scopes.insert(gid, "end", iid=iid,
                                       text=f"{run.run_id.rsplit('_', 1)[-1]} — {run.status} ({len(run.notices)})")
                    self.scope_map[iid] = ("run", run.run_id)
        if not self.scopes.exists(selected):
            selected = "all"
        self.scopes.selection_set(selected)
        self.scope_changed()
        if self.errors:
            messagebox.showwarning("Не все файлы удалось прочитать",
                                   f"Ошибок чтения: {len(self.errors)}\n\n" + "\n".join(self.errors[:5]))

    def scope_changed(self):
        sel = self.scopes.selection()
        kind, key = self.scope_map.get(sel[0] if sel else "all", ("all", ""))
        chosen = (self.runs if kind == "all" else
                  [r for r in self.runs if r.session == key] if kind == "session" else
                  [r for r in self.runs if r.generation == key] if kind == "gen" else
                  [r for r in self.runs if r.run_id == key])
        self.notices = [n for r in chosen for n in r.notices]
        self._build_filter_tree()
        self.update_results()
        failures = [r for r in chosen if r.status != "completed"]
        intro = (f"Выбрано прогонов: {len(chosen)}\n"
                 f"Завершённых успешно: {len(chosen)-len(failures)}\n"
                 f"Неуспешных: {len(failures)}\n"
                 f"Уведомлений: {len(self.notices)}\n\n")
        if failures:
            intro += "НЕУСПЕШНЫЕ ПРОГОНЫ:\n" + "\n".join(
                f"• {r.run_id}: {r.status}" + (f" — {r.failure}" if r.failure else "") for r in failures[:80]
            )
            if len(failures) > 80:
                intro += f"\n... и ещё {len(failures)-80}"
        self.set_detail(intro)

    def _build_filter_tree(self):
        self.filters.delete(*self.filters.get_children())
        self.checkbox_map.clear()
        levels = sorted({n.level for n in self.notices} | {1, 2, 3})
        for level in levels:
            codes = Counter(n.code for n in self.notices if n.level == level)
            for code in codes:
                self.code_enabled.setdefault((level, code), self.level_enabled.get(level, True))
            enabled = all(self.code_enabled[(level, code)] for code in codes) if codes else self.level_enabled.get(level, True)
            # A partially selected level is represented by a hollow checkbox.
            partial = codes and any(self.code_enabled[(level, code)] for code in codes) and not enabled
            symbol = "◩" if partial else CHECKED if enabled else UNCHECKED
            name = SEVERITY_NAMES.get(level, f"{level} — Другой уровень")
            lid = f"level:{level}"
            self.filters.insert("", "end", iid=lid, text=f"{symbol} {name}", values=(sum(codes.values()),), open=True)
            self.checkbox_map[lid] = (level, None)
            for i, (code, count) in enumerate(sorted(codes.items(), key=lambda x: (-x[1], x[0]))):
                ciid = f"code:{level}:{i}"
                self.filters.insert(lid, "end", iid=ciid,
                                    text=f"{CHECKED if self.code_enabled[(level, code)] else UNCHECKED} {code}",
                                    values=(count,))
                self.checkbox_map[ciid] = (level, code)

    def toggle_filter(self, event):
        iid = self.filters.identify_row(event.y)
        if iid not in self.checkbox_map:
            return
        level, code = self.checkbox_map[iid]
        if code is None:
            codes = {n.code for n in self.notices if n.level == level}
            target = not all(self.code_enabled.get((level, c), True) for c in codes) if codes else not self.level_enabled.get(level, True)
            self.level_enabled[level] = target
            for c in codes:
                self.code_enabled[(level, c)] = target
        else:
            self.code_enabled[(level, code)] = not self.code_enabled.get((level, code), True)
        self._build_filter_tree()
        self.update_results()

    def set_all_filters(self, on: bool):
        for k in self.level_enabled:
            self.level_enabled[k] = on
        for n in self.notices:
            self.code_enabled[(n.level, n.code)] = on
        self._build_filter_tree()
        self.update_results()

    def only_critical(self):
        self.set_all_filters(False)
        self.level_enabled[3] = True
        for n in self.notices:
            if n.level == 3:
                self.code_enabled[(n.level, n.code)] = True
        self._build_filter_tree()
        self.update_results()

    def update_results(self):
        query = self.find_text.get().casefold().strip()
        self.visible = [n for n in self.notices
                        if self.code_enabled.get((n.level, n.code), self.level_enabled.get(n.level, True))
                        and (not query or query in (n.code + " " + n.message + " " + n.recommendation + " " + n.run_id).casefold())]
        self.table.delete(*self.table.get_children())
        # Tk Treeview can't display millions of rows efficiently. Keep all records for export.
        display = self.visible[:10000]
        for index, n in enumerate(display):
            self.table.insert("", "end", iid=str(index),
                              values=(n.run_id, n.level, n.code, n.timestamp, n.thread_id,
                                      n.message.replace("\n", " ")[:280]),
                              tags=(f"level{n.level}",))
        levels = Counter(n.level for n in self.visible)
        limited = f" (показаны первые {len(display)})" if len(self.visible) > len(display) else ""
        self.counter.set(f"Уведомлений: {len(self.visible)}{limited}  |  "
                         f"1: {levels[1]}   2: {levels[2]}   3: {levels[3]}"
                         f"  |  файлов: {len(self.runs)}")

    @staticmethod
    def format_notice(n: Notice) -> str:
        return (f"Прогон: {n.run_id}\n"
                f"Уровень: {n.level} ({SEVERITY_NAMES.get(n.level, 'другой')})\n"
                f"Код: {n.code}\n"
                f"Время: {n.timestamp} сек\n"
                f"Поток C++: {n.thread_id}\n"
                f"Сообщение: {n.message}\n"
                f"Рекомендация: {n.recommendation or '—'}\n"
                f"Файл: {n.source}\n")

    def show_detail(self):
        rows = self.table.selection()
        if not rows:
            return
        idx = int(rows[0])
        if idx < len(self.visible):
            self.set_detail(self.format_notice(self.visible[idx]))

    def set_detail(self, content: str):
        self.detail.configure(state="normal")
        self.detail.delete("1.0", "end")
        self.detail.insert("1.0", content)
        self.detail.configure(state="disabled")

    def copy_text(self, content: str):
        self.clipboard_clear()
        self.clipboard_append(content)
        self.update_idletasks()

    def copy_detail(self):
        self.copy_text(self.detail.get("1.0", "end-1c"))

    def _selected_text(self, all_rows=False):
        indices = (range(len(self.visible)) if all_rows else
                   sorted(int(i) for i in self.table.selection()))
        return ("\n" + "=" * 53 + "\n").join(
            self.format_notice(self.visible[i]) for i in indices if i < len(self.visible))

    def copy_rows(self, all_rows=False):
        text = self._selected_text(all_rows)
        if text:
            self.copy_text(text)
        else:
            messagebox.showinfo("Копирование", "Выбери хотя бы одну строку")

    def export_txt(self):
        if not self.visible:
            messagebox.showinfo("Экспорт", "По текущим фильтрам уведомлений нет")
            return
        path = filedialog.asksaveasfilename(title="Сохранить отфильтрованные ошибки",
                                            defaultextension=".txt", filetypes=[("Текст", "*.txt")],
                                            initialfile="debug_issues_NOFORGIT.txt")
        if path:
            Path(path).write_text(self._selected_text(all_rows=True), encoding="utf-8")


def main():
    location = Path(sys.argv[1]) if len(sys.argv) > 1 else detect_folder(Path(__file__).resolve())
    app = DebugViewer(location)
    app.mainloop()


if __name__ == "__main__":
    main()
