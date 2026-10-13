# Copyright (C) 2026-2027 Ziad Mohsin. All Rights Reserved.
"""
mountxai/engine.py — Core orchestrator supporting Autonomous Codegen, Chat/Ask, In-Editor Fixing, and Explanation.
"""

import time
from mountxai.config import DEFAULT_MODEL
from mountxai.prompts import (
    SYSTEM_PROMPT,
    CHAT_SYSTEM_PROMPT,
    build_correction_prompt,
    build_refactor_prompt,
    build_explain_prompt
)
from mountxai.catalog import STDLIB_CATALOG, inspect_and_attach_files
from mountxai.client import call_deepseek
from mountxai.extractor import extract_mountx_code
from mountxai.runner import run_mountx_code

def solve_task(user_prompt, model=DEFAULT_MODEL, max_retries=3, verbose=True, callback=None):
    """
    سير عمل الوكيل الكامل مع حلقة التصحيح الذاتي التلقائي:
    1. إرسال الطلب مع الـ System Prompt ومواصفات الموديولات ذات الصلة.
    2. استخراج الكود وتشغيله عبر mountx-native.
    3. إذا نجح -> إرجاع الكود والمخرجات.
    4. إذا فشل -> إرسال الخطأ وموقع السهم ^ إلى النموذج للتصحيح التلقائي.
    """
    def emit(event, data=None):
        if callback:
            callback(event, data)

    dynamic_docs = STDLIB_CATALOG.get_relevant_docs(user_prompt)
    if verbose and dynamic_docs:
        print("📚 [محرك التوثيق التلقائي] تم اكتشاف وحقن مواصفات المكتبات القياسية المناسبة تلقائياً.")
    emit("docs_injected", {"docs": dynamic_docs})

    file_attachments = inspect_and_attach_files(user_prompt)
    if verbose and file_attachments:
        print("📂 [قارئ الملفات الذكي] تم اكتشاف وقراءة ملفات محلية مرتبطة بالطلب ودمجها تلقائياً.")
    emit("files_attached", {"files": file_attachments})

    effective_user_prompt = user_prompt + ("\n" + file_attachments if file_attachments else "")

    messages = [
        {"role": "system", "content": SYSTEM_PROMPT + dynamic_docs},
        {"role": "user", "content": effective_user_prompt}
    ]

    for attempt in range(1, max_retries + 1):
        if verbose:
            print(f"\n🔄 [المحاولة {attempt}/{max_retries}] جاري استشارة DeepSeek ({model})...")
        emit("attempt_start", {"attempt": attempt, "max": max_retries, "model": model})

        t0 = time.time()
        try:
            reply = call_deepseek(messages, model=model)
        except Exception as e:
            emit("api_error", {"attempt": attempt, "error": str(e)})
            return {"success": False, "attempts": attempt, "error": str(e)}

        elapsed = time.time() - t0
        code = extract_mountx_code(reply)

        if not code:
            if verbose:
                print("⚠️  لم يتم العثور على كود MountX صالح داخل الرد! جاري إعادة المحاولة...")
            emit("no_code_found", {"attempt": attempt, "reply": reply})
            messages.append({"role": "assistant", "content": reply})
            messages.append({"role": "user", "content": "You did not provide MountX code enclosed in mountx ... unmountx. Please provide ONLY executable MountX code enclosed in mountx / unmountx."})
            continue

        if verbose:
            print(f"⚡ تم استلام الكود في {elapsed:.2f} ثانية. جاري التشغيل عبر mountx-native...")
        emit("code_received", {"attempt": attempt, "elapsed": elapsed, "code": code})

        result = run_mountx_code(code)
        emit("execution_done", {"attempt": attempt, "result": result})

        if result["exit_code"] == 0:
            if verbose:
                print(f"✅ نجح التنفيذ من المحاولة {attempt} بنجاح باهر!")
                print("──────────────────────────────────────────────────")
                print("📄 الكود المولد:")
                print(code)
                print("──────────────────────────────────────────────────")
                print("💻 مخرجات البرنامج (Stdout):")
                print(result["stdout"] if result["stdout"] else "(تم التنفيذ بدون مخرجات نصية)")
                print("──────────────────────────────────────────────────")
            emit("success", {"attempt": attempt, "code": code, "output": result["stdout"], "duration_ms": result["duration_ms"]})
            return {
                "success": True,
                "attempts": attempt,
                "code": code,
                "output": result["stdout"],
                "duration_ms": result["duration_ms"],
                "raw_reply": reply
            }

        # فشل الكود -> تشغيل حلقة التصحيح الذاتي (Self-Healing)
        error_log = result["stderr"] if result["stderr"] else result["stdout"]
        if verbose:
            print(f"❌ فشل التنفيذ في المحاولة {attempt}!")
            print(f"🛑 تقرير المفسّر:\n{error_log}")
            print("🔧 جاري إرسال الخطأ للنموذج لتصحيحه ذاتياً...")
        emit("self_heal_needed", {"attempt": attempt, "error": error_log})

        correction_prompt = build_correction_prompt(code, error_log, result["exit_code"])
        messages.append({"role": "assistant", "content": reply})
        messages.append({"role": "user", "content": correction_prompt})

    if verbose:
        print(f"\n❌ تعذر تصحيح الكود بعد {max_retries} محاولات.")
    emit("failed", {"attempts": max_retries, "error": error_log, "code": code})
    return {
        "success": False,
        "attempts": max_retries,
        "code": code,
        "error": error_log
    }

def ask_chat(question, history=None, model=DEFAULT_MODEL):
    """
    وضع المحادثة والنقاش (Chat / Ask):
    إجابة أسئلة المستخدم المفاهيمية وشرح النظريات بدون محاولة إجبار الكود على التنفيذ.
    """
    messages = [{"role": "system", "content": CHAT_SYSTEM_PROMPT}]
    if history:
        for msg in history:
            messages.append(msg)
    messages.append({"role": "user", "content": question})

    reply = call_deepseek(messages, model=model, temperature=0.5)
    return reply

def correct_code(current_code, error_message=None, model=DEFAULT_MODEL):
    """
    وضع التصحيح المباشر (In-Editor Fix):
    يقرأ الكود الموجود في المحرر + رسالة الخطأ الظاهرة في الكونسول ويصلحه فوراً.
    """
    prompt = f"""Please fix the following MountX code.
Current Code:
```mountx
{current_code}
```
"""
    if error_message:
        prompt += f"\nCompiler/Runtime Error Reported:\n{error_message}\n"

    prompt += "\nRewrite the complete, corrected code enclosed inside mountx ... unmountx."

    messages = [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": prompt}
    ]

    reply = call_deepseek(messages, model=model, temperature=0.1)
    fixed_code = extract_mountx_code(reply)
    return fixed_code if fixed_code else current_code

def explain_code(code, model=DEFAULT_MODEL):
    """شرح كود MountX بالتفصيل للمستخدم."""
    prompt = build_explain_prompt(code)
    messages = [
        {"role": "system", "content": CHAT_SYSTEM_PROMPT},
        {"role": "user", "content": prompt}
    ]
    return call_deepseek(messages, model=model, temperature=0.3)
