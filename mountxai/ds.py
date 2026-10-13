#!/usr/bin/env python3
# test_deepseek.py — أول اختبار لـ DeepSeek API

import json
import urllib.request
import urllib.error

API_KEY = "sk-0897d377e2dd4b3884c60a2a69a81d1f"          # platform.deepseek.com
BASE_URL = "https://api.deepseek.com/chat/completions"

def ask_deepseek(prompt, model="deepseek-chat"):
    """يرسل سؤال لـ DeepSeek ويرجع الرد."""
    body = json.dumps({
        "model": model,
        "messages": [
            {"role": "system", "content": "أنت مساعد MountX — لغة برمجة بسيطة."},
            {"role": "user", "content": prompt}
        ]
    }).encode("utf-8")

    req = urllib.request.Request(
        BASE_URL,
        data=body,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {API_KEY}"
        },
        method="POST"
    )

    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            return data["choices"][0]["message"]["content"]
    except urllib.error.HTTPError as e:
        return f"❌ HTTP {e.code}: {e.read().decode()[:200]}"
    except Exception as e:
        return f"❌ {type(e).__name__}: {e}"

# ═══ الاختبار ═══
if __name__ == "__main__":
    print("🧪 Testing DeepSeek API...\n")

    # اختبار 1: سؤال بسيط
    r1 = ask_deepseek("قل مرحبا بالعربي في جملة قصيرة")
    print("Test 1:", r1[:150], "\n")

    # اختبار 2: سؤال تقني
    r2 = ask_deepseek("Write a one-line Python function that reverses a string")
    print("Test 2:", r2[:150], "\n")

    print("✅ API working!" if not r1.startswith("❌") else "❌ Check your key")
    
    print("─" * 40)
    print("🧠 Testing deepseek-reasoner (R1 — deep thinking):\n")
    r3 = ask_deepseek(
        "If MountX interpreter is written in MountX itself, "
        "what does that prove? Answer in one sentence.",
        model="deepseek-reasoner"          # ← R1 التفكير العميق
    )
    print(r3[:300])
