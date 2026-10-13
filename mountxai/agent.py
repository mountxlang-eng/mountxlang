#!/usr/bin/env python3
# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
"""
mountxai/agent.py — CLI Entrypoint for the MountX Autonomous AI Agent.
Modularized architecture: imports from mountxai.engine, config, and catalog.
"""

import os
import sys

# Ensure repo root is on sys.path so mountxai is always importable
_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _REPO_ROOT not in sys.path:
    sys.path.insert(0, _REPO_ROOT)

from mountxai.config import API_KEY, NATIVE_BIN
from mountxai.engine import solve_task, ask_chat, correct_code, explain_code
from mountxai.runner import run_mountx_code

def print_help():
    print("""
📌 الأوامر المتاحة (Slash Commands):
  /code <task>     - توليد وتشغيل كود MountX ذاتياً مع التصحيح التلقائي (الوضع الافتراضي)
  /ask <question>  - مناقشة وسؤال نظري ومفاهيمي مع خبير MountX (بدون تشغيل كود)
  /explain <file>  - شرح ملف أو كود MountX بالتفصيل
  /fix <file>      - فحص ملف MountX وتشغيله وتصحيح أي أخطاء تظهر فيه تلقائياً
  /gui, --gui, -g  - تشغيل استوديو الواجهة الرسومية MountX AI Workbench
  /help            - عرض قائمة المساعدة
  exit / quit      - إنهاء الجلسة
""")

def handle_command(cmd_line):
    cmd_line = cmd_line.strip()
    if not cmd_line:
        return

    parts = cmd_line.split(maxsplit=1)
    cmd = parts[0].lower()
    arg = parts[1].strip() if len(parts) > 1 else ""

    if cmd in ["/gui", "--gui", "-g", "gui", "واجهة"]:
        print("🚀 جاري إطلاق MountX AI Workbench (GTK 4 + Libadwaita)...")
        from mountxai.workbench import MountXWorkbenchApp
        app = MountXWorkbenchApp()
        sys.exit(app.run([]))
    elif cmd in ["/help", "-h", "--help", "مساعدة"]:
        print_help()
    elif cmd in ["/ask", "/chat", "اسأل"]:
        if not arg:
            print("⚠️  يرجى كتابة السؤال بعد الأمر، مثال: /ask ما هي مصفوفات MountX؟")
            return
        print(f"\n💬 السؤال: {arg}")
        print("⏳ جاري استشارة خبير MountX...")
        reply = ask_chat(arg)
        print("\n" + reply + "\n")
    elif cmd in ["/explain", "اشرح"]:
        if not arg:
            print("⚠️  يرجى تحديد الملف أو الكود للشرح، مثال: /explain prog.mx")
            return
        code_to_explain = arg
        if os.path.isfile(arg):
            with open(arg, "r", encoding="utf-8") as f:
                code_to_explain = f.read()
            print(f"📖 تم قراءة الملف: {arg}")
        print("⏳ جاري تحليل وشرح الكود...")
        explanation = explain_code(code_to_explain)
        print("\n" + explanation + "\n")
    elif cmd in ["/fix", "/correct", "صلح"]:
        if not arg:
            print("⚠️  يرجى تحديد الملف لإصلاحه، مثال: /fix buggy.mx")
            return
        target_file = arg
        if os.path.isfile(target_file):
            with open(target_file, "r", encoding="utf-8") as f:
                code = f.read()
            print(f"🧪 جاري اختبار الملف '{target_file}' عبر mountx-native...")
            res = run_mountx_code(code)
            if res["exit_code"] == 0:
                print("✅ الملف سليم تماماً ويعمل بدون أخطاء!")
                if res["stdout"]:
                    print("المخرجات:\n" + res["stdout"])
                return
            err = res["stderr"] if res["stderr"] else res["stdout"]
            print(f"❌ تم رصد خطأ:\n{err}")
            print("🔧 جاري التصحيح الذاتي عبر الذكاء الاصطناعي...")
            fixed = correct_code(code, err)
            test_res = run_mountx_code(fixed)
            if test_res["exit_code"] == 0:
                print("🎉 تم إصلاح الكود بنجاح واختباره بنجاح باهر!")
                with open(target_file, "w", encoding="utf-8") as f:
                    f.write(fixed)
                print(f"💾 تم حفظ النسخة المصححة في: {target_file}")
            else:
                print("⚠️  الكود المصحح لا يزال يواجه مشكلة:")
                print(test_res["stderr"] if test_res["stderr"] else test_res["stdout"])
        else:
            print(f"❌ لم يتم العثور على الملف: {target_file}")
    elif cmd in ["/code", "/codegen", "كود"]:
        if not arg:
            print("⚠️  يرجى كتابة المهمة بعد الأمر، مثال: /code أنشئ خادم HTTP")
            return
        solve_task(arg)
    else:
        # Default behavior: run as autonomous task
        solve_task(cmd_line)

def main():
    print("=" * 60)
    print("   🚀 MountX Autonomous AI Agent (DeepSeek-Powered)   ")
    print("=" * 60)

    if not API_KEY:
        print("❌ خطأ: لم يتم العثور على مفتاح DeepSeek API!")
        print("يرجى التأكد من وضع المفتاح في mountxai/deepseek.txt أو ضبط DEEPSEEK_API_KEY")
        sys.exit(1)

    print(f"🔑 API Key: {API_KEY[:8]}...{API_KEY[-4:]} (Active)")
    print(f"⚙️  Substrate: {NATIVE_BIN}")

    if len(sys.argv) > 1:
        task = " ".join(sys.argv[1:])
        handle_command(task)
    else:
        print("\n💡 الوضع التفاعلي: اكتب أي مهمة برمجية أو استعمل /ask أو /help (اكتب 'exit' للخروج):")
        while True:
            try:
                task = input("\n💬 طلبك (MountX) > ").strip()
                if not task:
                    continue
                if task.lower() in ["exit", "quit", "خروج"]:
                    print("مع السلامة! 👋")
                    break
                handle_command(task)
            except (KeyboardInterrupt, EOFError):
                print("\nتم الإنهاء.")
                break

if __name__ == "__main__":
    main()

