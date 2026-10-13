#!/usr/bin/env python3
# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
"""
mountxai/workbench.py — MountX AI Workbench (GTK 4 + Libadwaita + GtkSourceView 5).
Modern, dark-themed AI studio with live code editing, native execution,
stdlib explorer, examples gallery, and autonomous self-healing agent.
"""

import os
import sys
import glob
import time
import threading

# Force modern OpenGL (gl) renderer to avoid Mesa Haswell Vulkan glitches
if "GSK_RENDERER" not in os.environ:
    os.environ["GSK_RENDERER"] = "gl"

# Bootstrap sys.path to guarantee mountxai is always importable
_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

import gi
gi.require_version('Gtk', '4.0')
gi.require_version('Adw', '1')
gi.require_version('GtkSource', '5')
from gi.repository import Gtk, Adw, GtkSource, GLib, Gio, Gdk, Pango

from mountxai.config import REPO_ROOT, NATIVE_BIN, API_KEY, DEFAULT_MODEL
from mountxai.engine import solve_task, ask_chat, correct_code, explain_code
from mountxai.runner import run_mountx_code
from mountxai.catalog import STDLIB_CATALOG
from mountxai.extractor import extract_mountx_code

CUSTOM_CSS = """
/* MountX AI Workbench — Premium Dark Theme Palette */
window {
    background-color: #0f131a;
}

.code-studio-view {
    font-family: 'JetBrains Mono', 'Fira Code', 'DejaVu Sans Mono', 'Monospace';
    font-size: 11pt;
    background-color: #12161f;
    color: #e6edf3;
}

.terminal-console {
    font-family: 'JetBrains Mono', 'Fira Code', 'DejaVu Sans Mono', 'Monospace';
    font-size: 10pt;
    background-color: #0b0e14;
    color: #38bdf8;
    padding: 8px;
}

.terminal-titlebar {
    background-color: #161b22;
    border-top: 1px solid rgba(255, 255, 255, 0.08);
    border-bottom: 1px solid rgba(255, 255, 255, 0.05);
    padding: 4px 8px;
}

.badge-pill {
    border-radius: 9999px;
    padding: 2px 10px;
    font-weight: bold;
    font-size: 9pt;
}

.badge-ready {
    background-color: rgba(56, 189, 248, 0.15);
    color: #38bdf8;
    border: 1px solid rgba(56, 189, 248, 0.3);
}

.badge-running {
    background-color: rgba(245, 158, 11, 0.18);
    color: #fbbf24;
    border: 1px solid rgba(245, 158, 11, 0.35);
}

.badge-success {
    background-color: rgba(16, 185, 129, 0.18);
    color: #34d399;
    border: 1px solid rgba(16, 185, 129, 0.35);
}

.badge-error {
    background-color: rgba(239, 68, 68, 0.18);
    color: #f87171;
    border: 1px solid rgba(239, 68, 68, 0.35);
}

.run-btn-hero {
    background: linear-gradient(135deg, #10b981 0%, #059669 100%);
    color: white;
    font-weight: bold;
    border-radius: 8px;
    padding: 6px 14px;
}
.run-btn-hero:hover {
    background: linear-gradient(135deg, #34d399 0%, #10b981 100%);
}

.chat-card {
    border-radius: 10px;
    padding: 12px;
    margin: 4px 0px;
}

.chat-user {
    background-color: rgba(99, 102, 241, 0.15);
    border: 1px solid rgba(99, 102, 241, 0.3);
    margin-left: 20px;
}

.chat-agent {
    background-color: #161b24;
    border: 1px solid rgba(255, 255, 255, 0.08);
    margin-right: 15px;
}

.chat-healing {
    background-color: rgba(245, 158, 11, 0.12);
    border: 1px solid rgba(245, 158, 11, 0.25);
}

.chip-btn {
    border-radius: 9999px;
    font-size: 9pt;
    padding: 4px 10px;
    background-color: #1e2430;
    border: 1px solid rgba(255, 255, 255, 0.08);
}
.chip-btn:hover {
    background-color: #2b3242;
}

.sidebar-frame {
    background-color: #12161f;
    border-right: 1px solid rgba(255, 255, 255, 0.06);
}

.card-frame {
    background-color: #141923;
    border-radius: 8px;
    border: 1px solid rgba(255, 255, 255, 0.06);
    padding: 8px;
}
"""

WELCOME_CODE = """# ==========================================================
#  🚀 Welcome to MountX AI Workbench Studio
#  High-Performance Dynamically-Typed Programming Language
# ==========================================================

mountx
printx("========================================")
printx("  🌟 Welcome to MountX AI Studio 🌟")
printx("========================================")

# Simple Fibonacci Sequence Demonstration
func fib(n) {
    if n <= 1 {
        return n
    }
    return fib(n - 1) + fib(n - 2)
}

printx("Calculating Fibonacci numbers:")
for i in [1, 2, 3, 4, 5, 6, 7, 8, 9, 10] {
    printx("Fibonacci(" + i + ") = " + fib(i))
}

printx("Ready for action! Press 'Ctrl+Enter' to execute.")
unmountx
"""

class MountXWorkbenchWindow(Adw.ApplicationWindow):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.set_title("MountX AI Workbench")
        self.set_default_size(1360, 840)

        self.current_filepath = None
        self.is_executing = False
        self.is_agent_busy = False
        self.last_error = ""

        # Setup custom CSS
        self._setup_css()

        # Toast overlay as root child
        self.toast_overlay = Adw.ToastOverlay()
        self.set_content(self.toast_overlay)

        # Main vertical box
        main_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.toast_overlay.set_child(main_box)

        # 1. Header Bar
        self.header_bar = self._create_header_bar()
        main_box.append(self.header_bar)

        # 2. Main 3-Paned Body
        body_paned = self._create_main_body()
        main_box.append(body_paned)

        # 3. Setup Language & Highlighting
        self._setup_editor_syntax()

        # Load initial welcome code
        self.editor_buffer.set_text(WELCOME_CODE)

        # Register key events (Ctrl+Enter to run, Ctrl+S to save)
        key_controller = Gtk.EventControllerKey()
        key_controller.connect("key-pressed", self._on_key_pressed)
        self.add_controller(key_controller)

    def _setup_css(self):
        provider = Gtk.CssProvider()
        provider.load_from_string(CUSTOM_CSS)
        display = Gdk.Display.get_default()
        if display:
            Gtk.StyleContext.add_provider_for_display(
                display, provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
            )

    def _create_header_bar(self):
        header = Adw.HeaderBar()

        # Left / Start Controls
        start_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)

        self.sidebar_toggle = Gtk.ToggleButton(icon_name="sidebar-show-symbolic", tooltip_text="إظهار/إخفاء المكتبة والأمثلة (Library Sidebar)")
        self.sidebar_toggle.set_active(True)
        self.sidebar_toggle.connect("toggled", self._on_toggle_sidebar)
        start_box.append(self.sidebar_toggle)

        btn_new = Gtk.Button(icon_name="document-new-symbolic", tooltip_text="ملف جديد (Ctrl+N)")
        btn_new.connect("clicked", self._on_new_file)
        start_box.append(btn_new)

        btn_open = Gtk.Button(icon_name="document-open-symbolic", tooltip_text="فتح ملف MountX (Ctrl+O)")
        btn_open.connect("clicked", self._on_open_file)
        start_box.append(btn_open)

        btn_save = Gtk.Button(icon_name="document-save-symbolic", tooltip_text="حفظ الملف (Ctrl+S)")
        btn_save.connect("clicked", self._on_save_file)
        start_box.append(btn_save)

        header.pack_start(start_box)

        # Title widget (Window Title + Subtitle)
        title_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        title_box.set_valign(Gtk.Align.CENTER)
        lbl_title = Gtk.Label(label="MountX AI Workbench")
        lbl_title.add_css_class("title")
        lbl_sub = Gtk.Label(label="Autonomous AI Studio & High-Performance Substrate")
        lbl_sub.add_css_class("subtitle")
        lbl_sub.add_css_class("dim-label")
        title_box.append(lbl_title)
        title_box.append(lbl_sub)
        header.set_title_widget(title_box)

        # Right / End Controls
        end_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)

        # Substrate status pill
        self.lbl_substrate_pill = Gtk.Label(label="⚙️ ./mountx-native (Ready)")
        self.lbl_substrate_pill.add_css_class("badge-pill")
        self.lbl_substrate_pill.add_css_class("badge-ready")
        end_box.append(self.lbl_substrate_pill)

        # Big Run Button
        self.btn_run = Gtk.Button(label="▶ تشغيل (Ctrl+Enter)")
        self.btn_run.add_css_class("run-btn-hero")
        self.btn_run.set_tooltip_text("تشغيل الكود الحالي عبر المترجم الأصلي عالي الأداء")
        self.btn_run.connect("clicked", lambda b: self._execute_code())
        end_box.append(self.btn_run)

        # AI Deck Toggle
        self.ai_toggle = Gtk.ToggleButton(icon_name="chat-message-new-symbolic", tooltip_text="إظهار/إخفاء منصة الذكاء الاصطناعي (AI Deck)")
        self.ai_toggle.set_active(True)
        self.ai_toggle.connect("toggled", self._on_toggle_ai_deck)
        end_box.append(self.ai_toggle)

        header.pack_end(end_box)
        return header

    def _create_main_body(self):
        # Master horizontal paned: Left (Library) vs Right (Studio + AI)
        self.outer_paned = Gtk.Paned(orientation=Gtk.Orientation.HORIZONTAL)
        self.outer_paned.set_vexpand(True)
        self.outer_paned.set_hexpand(True)

        # 1. Left Sidebar: Library & Examples Explorer
        self.sidebar_widget = self._create_sidebar()
        self.sidebar_widget.set_size_request(280, -1)
        self.outer_paned.set_start_child(self.sidebar_widget)

        # 2. Right Sub-Paned: Center (Code + Console) vs Right (AI Deck)
        self.center_right_paned = Gtk.Paned(orientation=Gtk.Orientation.HORIZONTAL)
        self.center_right_paned.set_vexpand(True)
        self.center_right_paned.set_hexpand(True)

        # Center Studio
        center_studio = self._create_code_studio()
        self.center_right_paned.set_start_child(center_studio)

        # Right AI Deck
        self.ai_deck_widget = self._create_ai_deck()
        self.ai_deck_widget.set_size_request(380, -1)
        self.center_right_paned.set_end_child(self.ai_deck_widget)

        # Set default positions
        self.outer_paned.set_position(280)
        self.center_right_paned.set_position(680)

        self.outer_paned.set_end_child(self.center_right_paned)
        return self.outer_paned

    def _create_sidebar(self):
        sidebar_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        sidebar_box.add_css_class("sidebar-frame")
        sidebar_box.set_margin_start(6)
        sidebar_box.set_margin_end(6)
        sidebar_box.set_margin_top(6)
        sidebar_box.set_margin_bottom(6)

        # Stack Switcher for Tabs (Stdlib vs Examples)
        stack = Adw.ViewStack()
        switcher = Adw.ViewSwitcher(stack=stack, policy=Adw.ViewSwitcherPolicy.WIDE)
        sidebar_box.append(switcher)

        # Page 1: Stdlib Browser
        stdlib_page = self._build_stdlib_page()
        stack.add_titled(stdlib_page, "stdlib", "📚 المكتبة (41)")

        # Page 2: Examples Browser
        examples_page = self._build_examples_page()
        stack.add_titled(examples_page, "examples", "💡 الأمثلة")

        sidebar_box.append(stack)
        return sidebar_box

    def _build_stdlib_page(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)

        search_entry = Gtk.SearchEntry(placeholder_text="بحث في المكتبة القياسية...")
        box.append(search_entry)

        scrolled = Gtk.ScrolledWindow()
        scrolled.set_vexpand(True)
        listbox = Gtk.ListBox()
        listbox.set_selection_mode(Gtk.SelectionMode.NONE)
        listbox.add_css_class("boxed-list")
        scrolled.set_child(listbox)
        box.append(scrolled)

        all_modules = STDLIB_CATALOG.list_modules()
        rows = []

        for mod_name in all_modules:
            mod_info = STDLIB_CATALOG.get_module(mod_name)
            row = Adw.ActionRow(title=f"📦 {mod_name}")
            desc = mod_info.get("desc", "")
            if len(desc) > 50:
                desc = desc[:47] + "..."
            row.set_subtitle(GLib.markup_escape_text(desc))

            # Insert button
            btn_insert = Gtk.Button(icon_name="list-add-symbolic", tooltip_text=f"إدراج importx {mod_name}")
            btn_insert.add_css_class("flat")
            btn_insert.connect("clicked", lambda b, m=mod_name: self._insert_import(m))
            row.add_suffix(btn_insert)

            # Open module source button
            btn_view = Gtk.Button(icon_name="document-open-symbolic", tooltip_text="فتح كود الموديول في المحرر")
            btn_view.add_css_class("flat")
            btn_view.connect("clicked", lambda b, info=mod_info: self._open_module_content(info))
            row.add_suffix(btn_view)

            listbox.append(row)
            rows.append((mod_name, desc, row))

        # Search filter connection
        def on_search_changed(entry):
            query = entry.get_text().lower().strip()
            for name, desc, r in rows:
                match = (query in name.lower()) or (query in desc.lower())
                r.set_visible(match)

        search_entry.connect("search-changed", on_search_changed)
        return box

    def _build_examples_page(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)

        search_entry = Gtk.SearchEntry(placeholder_text="بحث في جميع الأمثلة...")
        box.append(search_entry)

        scrolled = Gtk.ScrolledWindow()
        scrolled.set_vexpand(True)
        listbox = Gtk.ListBox()
        listbox.set_selection_mode(Gtk.SelectionMode.NONE)
        listbox.add_css_class("boxed-list")
        scrolled.set_child(listbox)
        box.append(scrolled)

        rows = []

        # 1. Featured Section (Verified existing paths)
        header_featured = Adw.ActionRow(title="🌟 مختارات رئيسية سريعة")
        header_featured.add_css_class("heading")
        listbox.append(header_featured)

        featured = [
            ("🌐 خادم TCP Socket Server", "examples/07_networking_and_web/61_tcp_socket_server.mx", "خادم شبكة كامل لمآخذ TCP وطلبات الويب"),
            ("📡 جلب أسعار وREST API (HTTP)", "examples/07_networking_and_web/58_http_get_fetch.mx", "اتصال بالإنترنت وجلب بيانات JSON عبر HTTP"),
            ("💾 قاعدة بيانات SQLite CRUD", "examples/06_database_sqlite/50_sqlite_crud.mx", "إنشاء جداول وإضافة واستعلام وتحديث البيانات"),
            ("🔢 غربال الأعداد الأولية (Sieve)", "examples/03_math_and_algorithms/28_prime_sieve.mx", "خوارزمية الأعداد الأولية فائقة الأداء"),
            ("⚡ مقياس سرعة النظام (Benchmark)", "examples/05_system_and_io/46_benchmarking.mx", "قياس زمن تنفيذ العمليات والمقارنة الحسابية"),
            ("🌳 شجرة البحث الثنائية (BST)", "examples/02_data_structures/18_binary_search_tree.mx", "هيكل بيانات شجري كائني التوجه OOP"),
            ("🏗️ الكائنات والوراثة OOP", "examples/01_basics/06_inheritance_super.mx", "تطبيق مفاهيم الوراثة وتعدد الأشكال وsuper"),
            ("📋 متتبع النفقات (Expense Tracker)", "examples/08_projects/04_expense_tracker/main.mx", "مشروع متكامل لإدارة ومتابعة المصروفات")
        ]

        for title, rel_path, desc in featured:
            full_path = os.path.join(REPO_ROOT, rel_path)
            if os.path.exists(full_path):
                row = Adw.ActionRow(title=title)
                row.set_subtitle(GLib.markup_escape_text(desc))
                btn_load = Gtk.Button(label="تحميل", icon_name="document-open-symbolic")
                btn_load.add_css_class("pill")
                btn_load.connect("clicked", lambda b, p=full_path: self._load_file_into_editor(p))
                row.add_suffix(btn_load)
                listbox.append(row)
                rows.append((title, desc, row))

        # 2. Dynamic catalog of all other repository examples (66 files)
        header_all = Adw.ActionRow(title="📂 مستكشف جميع أمثلة ومشاريع المستودع")
        header_all.add_css_class("heading")
        listbox.append(header_all)

        pattern = os.path.join(REPO_ROOT, "examples", "**", "*.mx")
        all_mx_files = sorted(glob.glob(pattern, recursive=True))

        for path in all_mx_files:
            rel = os.path.relpath(path, os.path.join(REPO_ROOT, "examples"))
            fname = os.path.basename(path)

            # Skip files already shown in featured
            if any(rel_p in path for _, rel_p, _ in featured):
                continue

            parts = rel.split(os.sep)
            cat_label = parts[0].replace("_", " ").title() if len(parts) > 1 else "Root"
            title_text = f"📄 {fname}"
            desc_text = f"{cat_label} • {rel}"

            row = Adw.ActionRow(title=title_text)
            row.set_subtitle(GLib.markup_escape_text(desc_text))
            btn_load = Gtk.Button(label="تحميل", icon_name="document-open-symbolic")
            btn_load.add_css_class("flat")
            btn_load.connect("clicked", lambda b, p=path: self._load_file_into_editor(p))
            row.add_suffix(btn_load)
            listbox.append(row)
            rows.append((fname, desc_text, row))

        # Real-time search filter
        def on_search_changed(entry):
            q = entry.get_text().lower().strip()
            for name, desc, r in rows:
                match = (q in name.lower()) or (q in desc.lower())
                r.set_visible(match)

        search_entry.connect("search-changed", on_search_changed)
        return box

    def _create_code_studio(self):
        # Vertical Paned: Top (Editor) vs Bottom (Console Output)
        studio_paned = Gtk.Paned(orientation=Gtk.Orientation.VERTICAL)
        studio_paned.set_vexpand(True)
        studio_paned.set_hexpand(True)

        # --- Top Half: Code Editor ---
        editor_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        editor_box.set_margin_start(4)
        editor_box.set_margin_end(4)
        editor_box.set_margin_top(4)

        # Editor Toolbar
        tb_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)
        self.lbl_buffer_name = Gtk.Label(label="📄 untitled.mx")
        self.lbl_buffer_name.add_css_class("dim-label")
        tb_box.append(self.lbl_buffer_name)

        tb_box.append(Gtk.Separator(orientation=Gtk.Orientation.VERTICAL))

        self.lbl_editor_stats = Gtk.Label(label="سطور: 0 | أحرف: 0")
        self.lbl_editor_stats.add_css_class("dim-label")
        tb_box.append(self.lbl_editor_stats)

        # Push action buttons to the right
        spacer = Gtk.Box()
        spacer.set_hexpand(True)
        tb_box.append(spacer)

        btn_ai_fix = Gtk.Button(label="🛠️ تصحيح ذكي (Fix with AI)")
        btn_ai_fix.add_css_class("flat")
        btn_ai_fix.set_tooltip_text("إصلاح الكود المفتوح حالياً تلقائياً بناءً على آخر خطأ ظهر في الكونسول")
        btn_ai_fix.connect("clicked", lambda b: self._fix_current_code_with_ai())
        tb_box.append(btn_ai_fix)

        btn_explain = Gtk.Button(label="💡 شرح الكود")
        btn_explain.add_css_class("flat")
        btn_explain.set_tooltip_text("تحليل الكود وشرح منطقه البرمجي في منصة الذكاء الاصطناعي")
        btn_explain.connect("clicked", lambda b: self._explain_current_code())
        tb_box.append(btn_explain)

        btn_copy_code = Gtk.Button(icon_name="edit-copy-symbolic", tooltip_text="نسخ الكود")
        btn_copy_code.add_css_class("flat")
        btn_copy_code.connect("clicked", self._copy_editor_code)
        tb_box.append(btn_copy_code)

        btn_clear_editor = Gtk.Button(label="🧹 تفريغ المحرر", tooltip_text="مسح كل محتوى المحرر والبدء من جديد")
        btn_clear_editor.add_css_class("flat")
        btn_clear_editor.connect("clicked", lambda b: self._clear_editor())
        tb_box.append(btn_clear_editor)

        editor_box.append(tb_box)

        # GtkSourceView
        scrolled_editor = Gtk.ScrolledWindow()
        scrolled_editor.set_vexpand(True)

        self.editor_buffer = GtkSource.Buffer()
        self.editor_view = GtkSource.View.new_with_buffer(self.editor_buffer)
        self.editor_view.set_show_line_numbers(True)
        self.editor_view.set_highlight_current_line(True)
        self.editor_view.set_auto_indent(True)
        self.editor_view.set_tab_width(4)
        self.editor_view.set_insert_spaces_instead_of_tabs(True)
        self.editor_view.add_css_class("code-studio-view")

        # Update stats on change
        self.editor_buffer.connect("changed", self._on_buffer_changed)

        scrolled_editor.set_child(self.editor_view)
        editor_box.append(scrolled_editor)

        studio_paned.set_start_child(editor_box)

        # --- Bottom Half: Output Terminal Console ---
        console_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        console_box.set_size_request(-1, 190)

        # Console Header
        con_header = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        con_header.add_css_class("terminal-titlebar")

        lbl_con_title = Gtk.Label(label="💻 مخرجات المترجم الأصلي (Stdout / Stderr):")
        lbl_con_title.add_css_class("dim-label")
        con_header.append(lbl_con_title)

        self.lbl_exec_status = Gtk.Label(label="● جاهز")
        self.lbl_exec_status.add_css_class("badge-pill")
        self.lbl_exec_status.add_css_class("badge-ready")
        con_header.append(self.lbl_exec_status)

        self.lbl_duration = Gtk.Label(label="⏱️ 0 ms")
        self.lbl_duration.add_css_class("dim-label")
        con_header.append(self.lbl_duration)

        con_spacer = Gtk.Box()
        con_spacer.set_hexpand(True)
        con_header.append(con_spacer)

        btn_clear_con = Gtk.Button(label="🧹 تفريغ", tooltip_text="مسح المخرجات")
        btn_clear_con.add_css_class("flat")
        btn_clear_con.connect("clicked", lambda b: self._clear_console())
        con_header.append(btn_clear_con)

        btn_copy_con = Gtk.Button(icon_name="edit-copy-symbolic", tooltip_text="نسخ المخرجات")
        btn_copy_con.add_css_class("flat")
        btn_copy_con.connect("clicked", self._copy_console_output)
        con_header.append(btn_copy_con)

        console_box.append(con_header)

        # Terminal TextView
        scrolled_con = Gtk.ScrolledWindow()
        scrolled_con.set_vexpand(True)

        self.console_buffer = Gtk.TextBuffer()
        self.console_view = Gtk.TextView.new_with_buffer(self.console_buffer)
        self.console_view.set_editable(False)
        self.console_view.set_cursor_visible(False)
        self.console_view.set_monospace(True)
        self.console_view.add_css_class("terminal-console")

        scrolled_con.set_child(self.console_view)
        console_box.append(scrolled_con)

        studio_paned.set_end_child(console_box)
        studio_paned.set_position(480)

        return studio_paned

    def _create_ai_deck(self):
        deck_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        deck_box.add_css_class("sidebar-frame")
        deck_box.set_margin_start(6)
        deck_box.set_margin_end(6)
        deck_box.set_margin_top(6)
        deck_box.set_margin_bottom(6)

        # Header: Mode Selector
        header_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        lbl_deck_title = Gtk.Label(label="🤖 MountX AI Autonomous Deck")
        lbl_deck_title.add_css_class("heading")
        header_box.append(lbl_deck_title)

        # Mode Selector (Dropdown)
        modes = [
            "⚡ وكيل الكود الذاتي (Autonomous Agent)",
            "💬 محادثة وسؤال (Ask & Chat)",
            "🔧 تصحيح الكود المفتوح (Fix Editor Code)",
            "💡 شرح الكود الحالي (Explain Code)"
        ]
        self.mode_dropdown = Gtk.DropDown.new_from_strings(modes)
        self.mode_dropdown.set_selected(0)
        header_box.append(self.mode_dropdown)

        # Quick Suggestion Chips (Horizontally scrolled)
        chips_scrolled = Gtk.ScrolledWindow()
        chips_scrolled.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.NEVER)
        chips_scrolled.set_min_content_height(42)
        chips_scrolled.set_propagate_natural_height(True)
        chips_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)

        suggestions = [
            ("🌐 خادم HTTP", "أنشئ خادم HTTP كامل في mountx يستقبل طلبات GET ويرد بـ JSON"),
            ("🔌 مقبس TCP", "اكتب برنامج socket tcp يفتح منفذاً محلياً ويرسل رسالة ترحيبية"),
            ("📊 موارد النظام", "اعرض نظام التشغيل وحجم الرام والمعالج بتنسيق أنيق"),
            ("🔢 الأعداد الأولية", "احسب جميع الأعداد الأولية حتى 20000 بأعلى أداء"),
            ("💾 قاعدة بيانات", "أنشئ قاعدة بيانات sqlite وجدول مستخدمين ونفذ عمليات CRUD")
        ]

        for chip_label, prompt_text in suggestions:
            btn_chip = Gtk.Button(label=chip_label)
            btn_chip.add_css_class("chip-btn")
            btn_chip.connect("clicked", lambda b, p=prompt_text: self._set_prompt_text(p))
            chips_box.append(btn_chip)

        chips_scrolled.set_child(chips_box)
        header_box.append(chips_scrolled)

        deck_box.append(header_box)

        # Chat / Activity Stream
        self.chat_scrolled = Gtk.ScrolledWindow()
        self.chat_scrolled.set_vexpand(True)

        self.chat_container = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        self.chat_container.set_margin_start(4)
        self.chat_container.set_margin_end(4)
        self.chat_scrolled.set_child(self.chat_container)

        # Initial Welcome Bubble
        self._add_chat_bubble(
            "assistant",
            "أهلاً بك! أنا وكيل MountX الذكي المدعوم بـ DeepSeek.\n"
            "يمكنني كتابة الأكواد وتشغيلها ذاتياً عبر `./mountx-native` وإصلاح الأخطاء ذاتياً، "
            "أو الإجابة عن أي استفسار مفاهيمي حول اللغة ومكتباتها القياسية الـ 41."
        )

        deck_box.append(self.chat_scrolled)

        # Bottom Input Box
        input_container = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)

        self.input_entry = Gtk.Entry()
        self.input_entry.set_placeholder_text("اكتب طلبك أو سؤالك هنا... (Enter للإرسال)")
        self.input_entry.connect("activate", lambda e: self._on_submit_prompt())
        input_container.append(self.input_entry)

        actions_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=6)

        # Model badge
        model_pill = Gtk.Label(label=f"🧠 {DEFAULT_MODEL}")
        model_pill.add_css_class("dim-label")
        actions_box.append(model_pill)

        actions_spacer = Gtk.Box()
        actions_spacer.set_hexpand(True)
        actions_box.append(actions_spacer)

        self.ai_spinner = Gtk.Spinner()
        self.ai_spinner.set_visible(False)
        actions_box.append(self.ai_spinner)

        self.btn_send = Gtk.Button(label="إرسال 🚀")
        self.btn_send.add_css_class("suggested-action")
        self.btn_send.connect("clicked", lambda b: self._on_submit_prompt())
        actions_box.append(self.btn_send)

        input_container.append(actions_box)
        deck_box.append(input_container)

        return deck_box

    def _setup_editor_syntax(self):
        # Register custom mountx language
        lm = GtkSource.LanguageManager.get_default()
        paths = list(lm.get_search_path())
        local_mountxai = os.path.join(REPO_ROOT, "mountxai")
        if local_mountxai not in paths:
            paths.append(local_mountxai)
            lm.set_search_path(paths)

        lang = lm.get_language("mountx")
        if lang:
            self.editor_buffer.set_language(lang)

        # Set Adwaita-dark or similar dark style scheme
        sm = GtkSource.StyleSchemeManager.get_default()
        for scheme_id in ["Adwaita-dark", "Yaru-dark", "solarized-dark", "oblivion", "classic-dark"]:
            scheme = sm.get_scheme(scheme_id)
            if scheme:
                self.editor_buffer.set_style_scheme(scheme)
                break

    # ==========================================================
    #  Actions & Event Handlers
    # ==========================================================

    def _on_key_pressed(self, controller, keyval, keycode, state):
        # Ctrl + Enter / Return -> Execute Code
        if (state & Gdk.ModifierType.CONTROL_MASK) and (keyval in (Gdk.KEY_Return, Gdk.KEY_KP_Enter)):
            self._execute_code()
            return True
        # Ctrl + S -> Save
        if (state & Gdk.ModifierType.CONTROL_MASK) and (keyval in (Gdk.KEY_s, Gdk.KEY_S)):
            self._on_save_file(None)
            return True
        # Ctrl + N -> New
        if (state & Gdk.ModifierType.CONTROL_MASK) and (keyval in (Gdk.KEY_n, Gdk.KEY_N)):
            self._on_new_file(None)
            return True
        return False

    def _on_toggle_sidebar(self, btn):
        visible = btn.get_active()
        self.sidebar_widget.set_visible(visible)

    def _on_toggle_ai_deck(self, btn):
        visible = btn.get_active()
        self.ai_deck_widget.set_visible(visible)

    def _on_buffer_changed(self, buf):
        text = buf.get_text(buf.get_start_iter(), buf.get_end_iter(), True)
        lines = buf.get_line_count()
        chars = len(text)
        self.lbl_editor_stats.set_text(f"سطور: {lines} | أحرف: {chars}")

    def _set_prompt_text(self, text):
        self.input_entry.set_text(text)
        self.input_entry.grab_focus()

    def _insert_import(self, mod_name):
        stmt = f'importx {mod_name}\n'
        self.editor_buffer.insert_at_cursor(stmt)
        self._show_toast(f"تم إدراج {stmt.strip()}")

    def _open_module_content(self, mod_info):
        content = mod_info.get("content", "")
        self.editor_buffer.set_text(content)
        self.lbl_buffer_name.set_text(f"📦 {mod_info['name']}.mx (Standard Library)")
        self._show_toast(f"تم فتح موديول {mod_info['name']} في المحرر")

    def _load_file_into_editor(self, filepath):
        if not os.path.isfile(filepath):
            self._show_toast(f"الملف غير موجود: {filepath}")
            return
        try:
            with open(filepath, "r", encoding="utf-8") as f:
                code = f.read()
            self.editor_buffer.set_text(code)
            self.current_filepath = filepath
            self.lbl_buffer_name.set_text(f"📄 {os.path.basename(filepath)}")
            self._show_toast(f"تم تحميل: {os.path.basename(filepath)}")
        except Exception as e:
            self._show_toast(f"خطأ في قراءة الملف: {e}")

    def _clear_editor(self):
        self.editor_buffer.set_text("")
        self.current_filepath = None
        self.lbl_buffer_name.set_text("📄 untitled.mx")
        self._show_toast("تم تفريغ المحرر بنجاح")

    def _on_new_file(self, btn):
        self._clear_editor()
        self._show_toast("تم فتح ملف جديد فارغ")

    def _on_open_file(self, btn):
        if hasattr(Gtk, "FileDialog"):
            dialog = Gtk.FileDialog()
            dialog.set_title("فتح ملف MountX")
            filt = Gtk.FileFilter()
            filt.set_name("MountX Source Files (*.mx)")
            filt.add_pattern("*.mx")
            store = Gio.ListStore.new(Gtk.FileFilter)
            store.append(filt)
            dialog.set_filters(store)
            dialog.set_default_filter(filt)

            def on_open_done(d, result):
                try:
                    gfile = d.open_finish(result)
                    if gfile:
                        self._load_file_into_editor(gfile.get_path())
                except GLib.GError:
                    pass

            dialog.open(self, None, on_open_done)
        else:
            dialog = Gtk.FileChooserNative(
                title="فتح ملف MountX",
                transient_for=self,
                action=Gtk.FileChooserAction.OPEN,
                accept_label="فتح",
                cancel_label="إلغاء"
            )
            filt = Gtk.FileFilter()
            filt.set_name("MountX Source Files (*.mx)")
            filt.add_pattern("*.mx")
            dialog.add_filter(filt)

            def on_response(d, response_id):
                if response_id == Gtk.ResponseType.ACCEPT:
                    gfile = d.get_file()
                    if gfile:
                        path = gfile.get_path()
                        self._load_file_into_editor(path)
                d.destroy()

            dialog.connect("response", on_response)
            dialog.show()

    def _on_save_file(self, btn):
        code = self._get_editor_code()
        if self.current_filepath:
            try:
                with open(self.current_filepath, "w", encoding="utf-8") as f:
                    f.write(code)
                self._show_toast(f"تم حفظ: {os.path.basename(self.current_filepath)}")
            except Exception as e:
                self._show_toast(f"فشل الحفظ: {e}")
        else:
            if hasattr(Gtk, "FileDialog"):
                dialog = Gtk.FileDialog()
                dialog.set_title("حفظ ملف MountX")
                dialog.set_initial_name("program.mx")

                def on_save_done(d, result):
                    try:
                        gfile = d.save_finish(result)
                        if gfile:
                            path = gfile.get_path()
                            try:
                                with open(path, "w", encoding="utf-8") as f:
                                    f.write(code)
                                self.current_filepath = path
                                self.lbl_buffer_name.set_text(f"📄 {os.path.basename(path)}")
                                self._show_toast(f"تم الحفظ في: {os.path.basename(path)}")
                            except Exception as e:
                                self._show_toast(f"فشل الحفظ: {e}")
                    except GLib.GError:
                        pass

                dialog.save(self, None, on_save_done)
            else:
                dialog = Gtk.FileChooserNative(
                    title="حفظ ملف MountX",
                    transient_for=self,
                    action=Gtk.FileChooserAction.SAVE,
                    accept_label="حفظ",
                    cancel_label="إلغاء"
                )
                dialog.set_current_name("program.mx")

                def on_response(d, response_id):
                    if response_id == Gtk.ResponseType.ACCEPT:
                        gfile = d.get_file()
                        if gfile:
                            path = gfile.get_path()
                            try:
                                with open(path, "w", encoding="utf-8") as f:
                                    f.write(code)
                                self.current_filepath = path
                                self.lbl_buffer_name.set_text(f"📄 {os.path.basename(path)}")
                                self._show_toast(f"تم الحفظ في: {os.path.basename(path)}")
                            except Exception as e:
                                self._show_toast(f"فشل الحفظ: {e}")
                    d.destroy()

                dialog.connect("response", on_response)
                dialog.show()

    def _get_editor_code(self):
        start = self.editor_buffer.get_start_iter()
        end = self.editor_buffer.get_end_iter()
        return self.editor_buffer.get_text(start, end, True)

    def _clear_console(self):
        self.console_buffer.set_text("")
        self.lbl_exec_status.set_text("● جاهز")
        self.lbl_exec_status.remove_css_class("badge-running")
        self.lbl_exec_status.remove_css_class("badge-success")
        self.lbl_exec_status.remove_css_class("badge-error")
        self.lbl_exec_status.add_css_class("badge-ready")

    def _copy_editor_code(self, btn):
        code = self._get_editor_code()
        display = Gdk.Display.get_default()
        if display:
            clipboard = display.get_clipboard()
            clipboard.set(code)
            self._show_toast("تم نسخ الكود إلى الحافظة")

    def _copy_console_output(self, btn):
        start = self.console_buffer.get_start_iter()
        end = self.console_buffer.get_end_iter()
        out = self.console_buffer.get_text(start, end, True)
        display = Gdk.Display.get_default()
        if display:
            clipboard = display.get_clipboard()
            clipboard.set(out)
            self._show_toast("تم نسخ مخرجات الكونسول إلى الحافظة")

    def _show_toast(self, message):
        toast = Adw.Toast.new(message)
        self.toast_overlay.add_toast(toast)

    # ==========================================================
    #  Code Execution (Non-Blocking / Threaded)
    # ==========================================================

    def _execute_code(self):
        if self.is_executing:
            return

        code = self._get_editor_code()
        if not code.strip():
            self._show_toast("المحرر فارغ!")
            return

        self.is_executing = True
        self.lbl_exec_status.set_text("⚡ قيد التشغيل...")
        self.lbl_exec_status.remove_css_class("badge-ready")
        self.lbl_exec_status.remove_css_class("badge-success")
        self.lbl_exec_status.remove_css_class("badge-error")
        self.lbl_exec_status.add_css_class("badge-running")
        self.btn_run.set_sensitive(False)

        def worker():
            res = run_mountx_code(code)
            GLib.idle_add(self._on_execution_finished, res)

        threading.Thread(target=worker, daemon=True).start()

    def _on_execution_finished(self, res):
        self.is_executing = False
        self.btn_run.set_sensitive(True)

        duration = res.get("duration_ms", 0.0)
        self.lbl_duration.set_text(f"⏱️ {duration:.1f} ms")

        exit_code = res.get("exit_code", 0)
        stdout = res.get("stdout", "")
        stderr = res.get("stderr", "")

        self.lbl_exec_status.remove_css_class("badge-running")

        if exit_code == 0:
            self.lbl_exec_status.set_text("✓ نجاح (0)")
            self.lbl_exec_status.add_css_class("badge-success")
            output_text = stdout if stdout else "(تم التنفيذ بنجاح بدون مخرجات نصية)"
            self.console_buffer.set_text(output_text)
            self.last_error = ""
        else:
            self.lbl_exec_status.set_text(f"❌ خطأ ({exit_code})")
            self.lbl_exec_status.add_css_class("badge-error")
            err_log = stderr if stderr else stdout
            self.last_error = err_log
            self.console_buffer.set_text(f"--- تقرير الخطأ (Exit Code: {exit_code}) ---\n" + err_log)

        return False

    # ==========================================================
    #  AI Autonomous Agent & Chat Deck Integration
    # ==========================================================

    def _add_chat_bubble(self, sender, text, code_to_load=None, is_healing=False):
        card = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        card.add_css_class("chat-card")

        if sender == "user":
            card.add_css_class("chat-user")
            badge = Gtk.Label(label="👤 أنت:")
            badge.set_halign(Gtk.Align.START)
            badge.add_css_class("heading")
            card.append(badge)
        elif is_healing:
            card.add_css_class("chat-healing")
            badge = Gtk.Label(label="🔧 تصحيح ذاتي تلقائي:")
            badge.set_halign(Gtk.Align.START)
            badge.add_css_class("heading")
            card.append(badge)
        else:
            card.add_css_class("chat-agent")
            badge = Gtk.Label(label="🤖 MountX Agent:")
            badge.set_halign(Gtk.Align.START)
            badge.add_css_class("heading")
            card.append(badge)

        lbl_content = Gtk.Label(label=text)
        lbl_content.set_wrap(True)
        lbl_content.set_xalign(0)
        lbl_content.set_selectable(True)
        card.append(lbl_content)

        if code_to_load:
            btn_load = Gtk.Button(label="📥 فتح الكود المولد في المحرر")
            btn_load.add_css_class("suggested-action")
            btn_load.set_halign(Gtk.Align.START)
            btn_load.connect("clicked", lambda b, c=code_to_load: self._load_generated_code(c))
            card.append(btn_load)

        self.chat_container.append(card)

        # Scroll to bottom
        GLib.idle_add(self._scroll_chat_to_bottom)

    def _scroll_chat_to_bottom(self):
        adj = self.chat_scrolled.get_vadjustment()
        adj.set_value(adj.get_upper() - adj.get_page_size())
        return False

    def _load_generated_code(self, code):
        self.editor_buffer.set_text(code)
        self.lbl_buffer_name.set_text("📄 ai_generated.mx")
        self._show_toast("تم وضع الكود المولد في المحرر بنجاح!")

    def _on_submit_prompt(self):
        prompt = self.input_entry.get_text().strip()
        if not prompt or self.is_agent_busy:
            return

        self.input_entry.set_text("")
        self._add_chat_bubble("user", prompt)

        # Check slash command or selected mode
        selected_mode_idx = self.mode_dropdown.get_selected()
        parts = prompt.split(maxsplit=1)
        first_token = parts[0].lower()

        if first_token in ["/ask", "/chat"] or selected_mode_idx == 1:
            q = parts[1] if (len(parts) > 1 and first_token in ["/ask", "/chat"]) else prompt
            self._run_chat_question(q)
        elif first_token in ["/explain"] or selected_mode_idx == 3:
            self._explain_current_code()
        elif first_token in ["/fix", "/correct"] or selected_mode_idx == 2:
            self._fix_current_code_with_ai()
        else:
            task = parts[1] if (len(parts) > 1 and first_token in ["/code", "/codegen"]) else prompt
            self._run_autonomous_agent(task)

    def _run_chat_question(self, question):
        self._set_ai_busy(True)

        def worker():
            try:
                reply = ask_chat(question)
                GLib.idle_add(self._on_chat_reply, reply)
            except Exception as e:
                GLib.idle_add(self._on_chat_reply, f"❌ خطأ: {e}")

        threading.Thread(target=worker, daemon=True).start()

    def _on_chat_reply(self, reply):
        self._set_ai_busy(False)
        self._add_chat_bubble("assistant", reply)
        return False

    def _run_autonomous_agent(self, task):
        self._set_ai_busy(True)

        def ui_callback(event, data):
            GLib.idle_add(self._on_agent_event, event, data)

        def worker():
            res = solve_task(task, callback=ui_callback, verbose=False)
            GLib.idle_add(self._on_agent_finished, res)

        threading.Thread(target=worker, daemon=True).start()

    def _on_agent_event(self, event, data):
        if event == "attempt_start":
            att = data.get("attempt", 1)
            mx = data.get("max", 3)
            self._add_chat_bubble("assistant", f"🔄 [المحاولة {att}/{mx}] جاري توليد كود MountX واختباره...")
        elif event == "self_heal_needed":
            err = data.get("error", "")
            if len(err) > 160:
                err = err[:157] + "..."
            self._add_chat_bubble("assistant", f"⚠️ حدث خطأ في التنفيذ:\n{err}\nجاري التصحيح الذاتي التلقائي...", is_healing=True)
        return False

    def _on_agent_finished(self, res):
        self._set_ai_busy(False)
        if res.get("success"):
            code = res.get("code", "")
            output = res.get("output", "")
            duration = res.get("duration_ms", 0.0)
            msg = f"✅ تم إنجاز المهمة بنجاح باهر في المحاولة {res.get('attempts', 1)}!\n⏱️ زمن التنفيذ: {duration:.1f} ms\n\n💻 المخرجات:\n{output}"
            self._add_chat_bubble("assistant", msg, code_to_load=code)
            self._show_toast("نجح توليد وتشغيل الكود!")
        else:
            err = res.get("error", "تعذر حل المهمة")
            self._add_chat_bubble("assistant", f"❌ لم يتم حل المهمة بعد المحاولات القصوى:\n{err}")
        return False

    def _fix_current_code_with_ai(self):
        code = self._get_editor_code()
        if not code.strip():
            self._show_toast("المحرر فارغ!")
            return

        self._set_ai_busy(True)
        self._add_chat_bubble("assistant", "🔧 جاري تحليل الكود والخطأ الأخير وتصحيحه تلقائياً...")

        def worker():
            try:
                fixed = correct_code(code, self.last_error)
                GLib.idle_add(self._on_fix_completed, fixed)
            except Exception as e:
                GLib.idle_add(self._add_chat_bubble, "assistant", f"❌ فشل التصحيح: {e}")
                GLib.idle_add(self._set_ai_busy, False)

        threading.Thread(target=worker, daemon=True).start()

    def _on_fix_completed(self, fixed_code):
        self._set_ai_busy(False)
        self.editor_buffer.set_text(fixed_code)
        self._add_chat_bubble("assistant", "🎉 تم تصحيح الكود ووضعه في المحرر! اضغط تشغيل للتحقق.")
        self._show_toast("تم تطبيق الكود المصحح في المحرر")
        return False

    def _explain_current_code(self):
        code = self._get_editor_code()
        if not code.strip():
            self._show_toast("المحرر فارغ!")
            return

        self._set_ai_busy(True)
        self._add_chat_bubble("assistant", "💡 جاري تحليل الكود وشرح منطقه...")

        def worker():
            try:
                explanation = explain_code(code)
                GLib.idle_add(self._on_explain_completed, explanation)
            except Exception as e:
                GLib.idle_add(self._add_chat_bubble, "assistant", f"❌ فشل الشرح: {e}")
                GLib.idle_add(self._set_ai_busy, False)

        threading.Thread(target=worker, daemon=True).start()

    def _on_explain_completed(self, explanation):
        self._set_ai_busy(False)
        self._add_chat_bubble("assistant", explanation)
        return False

    def _set_ai_busy(self, busy):
        self.is_agent_busy = busy
        self.btn_send.set_sensitive(not busy)
        if busy:
            self.ai_spinner.set_visible(True)
            self.ai_spinner.start()
        else:
            self.ai_spinner.stop()
            self.ai_spinner.set_visible(False)


class MountXWorkbenchApp(Adw.Application):
    def __init__(self):
        super().__init__(
            application_id="org.mountx.ai.workbench",
            flags=Gio.ApplicationFlags.DEFAULT_FLAGS
        )

    def do_activate(self):
        # Force Dark Mode for Modern Aesthetics
        Adw.StyleManager.get_default().set_color_scheme(Adw.ColorScheme.FORCE_DARK)
        win = self.props.active_window
        if not win:
            win = MountXWorkbenchWindow(application=self)
        win.present()

def main():
    app = MountXWorkbenchApp()
    return app.run(sys.argv)

if __name__ == "__main__":
    sys.exit(main())
