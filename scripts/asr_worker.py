#!/usr/bin/env python3
"""
ASR Worker — 音频转录文字级时间戳生成器

协议:
  stdout — PROGRESS: <int>  实时进度 (0-100)
           {json}           最终 JSON 结果
  stderr — 所有日志、调试信息、错误详情

支持模型:
  - Qwen/Qwen3-ASR-0.6B (及同系列)
  - openai/whisper-* (small, medium, large-v3)
"""

import argparse
import json
import sys
import os
import time
import traceback

# ── 强制 stdout/stderr 使用 UTF-8 ──────────────────────────
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8")


def log(msg: str):
    """日志统一走 stderr"""
    print(f"[ASR] {msg}", file=sys.stderr, flush=True)


def progress(pct: int):
    """进度通过 stdout 输出，C++ 按行读取解析"""
    print(f"PROGRESS: {pct}", flush=True)


def align_punctuation(full_text, raw_words):
    """Align full-text (with punctuation) against raw timestamp words.

    Qwen-ASR's alignment only assigns timestamps to pronounceable characters,
    so the ``words`` array from ``time_stamps`` has no punctuation.  This
    function uses a two-pointer scan of ``full_text`` (which includes
    punctuation) to insert punctuation entries with timing borrowed from
    the preceding character.

    ``raw_words[i]`` may be single-char or multi-char (word-level alignment).
    The function matches by the first character and consumes the entire entry.
    """
    PUNCT_SET = set("，。？！、；：\n\r\t ")

    aligned = []
    wi = 0      # index into raw_words
    fi = 0      # index into full_text

    while fi < len(full_text) and wi < len(raw_words):
        ch = full_text[fi]

        if ch in PUNCT_SET:
            prev_end = aligned[-1]["end"] if aligned else 0.0
            aligned.append({
                "word":  ch,
                "start": prev_end,
                "end":   prev_end,
            })
            fi += 1
            continue

        raw_word = raw_words[wi]["word"]
        if raw_word and ch == raw_word[0]:
            # First character matches → consume entire raw_word entry
            aligned.append(dict(raw_words[wi]))
            fi += len(raw_word)  # skip all chars this word covers
            wi += 1
        else:
            # Mismatch — should not normally happen; skip char
            fi += 1

    # Trailing punctuation at end of text
    while fi < len(full_text):
        ch = full_text[fi]
        if ch in PUNCT_SET:
            prev_end = aligned[-1]["end"] if aligned else 0.0
            aligned.append({"word": ch, "start": prev_end, "end": prev_end})
        fi += 1

    return aligned


def postprocess_words(words):
    """Post-process words: split punctuation, detect paragraph boundaries.

    1. Splits trailing Chinese punctuation (。？！) from word text into its own entry,
       with start/end time equal to the preceding character's end time.
    2. Detects paragraph boundaries: if prev word ends with 。？！ or silence gap > 1.5 s.
    3. Marks the first word of each paragraph with ``is_paragraph_start: true``.
    """
    PUNCT = set("。？！")

    if not words:
        return words

    # ── Step 1: ensure punctuation has its own entry ──────────
    expanded = []
    for i, w in enumerate(words):
        text = w["word"]
        if not text:
            continue

        # Last character is CJK punctuation?  Split it off.
        if len(text) > 1 and text[-1] in PUNCT:
            expanded.append({
                "word": text[:-1],
                "start": w["start"],
                "end":   w["end"],
            })
            # punctuation entry: timestamps align with previous char
            prev_end = words[i - 1]["end"] if i > 0 else w["start"]
            expanded.append({
                "word": text[-1],
                "start": prev_end,
                "end":   w["end"],
            })
        else:
            expanded.append(dict(w))

    # ── Step 2: mark paragraph starts ─────────────────────────
    result = []
    for i, w in enumerate(expanded):
        entry = dict(w)

        if i == 0:
            entry["is_paragraph_start"] = True
        else:
            prev = expanded[i - 1]
            prev_text = prev.get("word", "")

            # Previous word ends with Chinese punctuation？
            if prev_text and prev_text[-1] in PUNCT:
                entry["is_paragraph_start"] = True
            # Silence gap > 1.5 seconds?
            elif (w["start"] - prev["end"]) > 1.5:
                entry["is_paragraph_start"] = True

        result.append(entry)

    return result


def run_asr_qwen(audio_path: str, device: str, model_name: str) -> dict:
    """使用 qwen-asr 包加载 Qwen3-ASR 模型并推理"""
    import torch
    from qwen_asr import Qwen3ASRModel

    progress(0)

    dtype = torch.bfloat16 if torch.cuda.is_available() else torch.float32
    device_map = device if torch.cuda.is_available() else "cpu"

    forced_aligner_model = model_name.replace("ASR", "ForcedAligner")

    # 加载模型
    log(f"加载模型: {model_name} device={device}")
    t0 = time.time()
    model = Qwen3ASRModel.from_pretrained(
        model_name,
        dtype=dtype,
        device_map=device_map,
        forced_aligner=forced_aligner_model,
        forced_aligner_kwargs={
            "dtype": dtype,
            "device_map": device_map,
        },
        max_new_tokens=256,
    )
    log(f"模型加载完成 ({time.time() - t0:.1f}s)")
    progress(15)

    # 推理
    log(f"开始识别: {audio_path}")
    t1 = time.time()
    progress(30)

    results = model.transcribe(audio_path, return_time_stamps=True)
    elapsed = time.time() - t1
    log(f"识别完成 ({elapsed:.1f}s)")
    progress(80)

    if not results:
        raise RuntimeError("ASR 返回了空结果")

    result = results[0]

    # 解析时间戳
    words = []
    if result.time_stamps is not None:
        for item in result.time_stamps:
            words.append({
                "word": item.text,
                "start": float(item.start_time),
                "end": float(item.end_time),
            })
    # ── 双指针对齐：将 result.text（含标点）和 raw_words（仅有发音字）对齐 ──
    if result.text is not None and words:
        words = align_punctuation(result.text, words)
    progress(90)

    # fallback: 没有时间戳 → 按时长等比分配
    if not words:
        full_text = result.text
        if not full_text:
            raise RuntimeError("ASR 返回了空文本")
        import soundfile as sf
        info = sf.info(audio_path)
        total_dur = float(info.duration)
        per_char = total_dur / max(1, len(full_text))
        for i, ch in enumerate(full_text):
            words.append({
                "word": ch,
                "start": i * per_char,
                "end": (i + 1) * per_char,
            })

    words = postprocess_words(words)
    full_text = "".join(w["word"] for w in words)
    progress(100)

    return {
        "success": True,
        "text": full_text,
        "words": words,
    }


def run_asr_whisper(audio_path: str, device: str, model_name: str) -> dict:
    """使用 transformers pipeline 加载 Whisper 模型并推理"""
    from transformers import pipeline

    progress(0)

    log(f"加载模型: {model_name} device={device}")
    t0 = time.time()
    asr = pipeline(
        "automatic-speech-recognition",
        model=model_name,
        device=device,
        chunk_length_s=30,
    )
    log(f"模型加载完成 ({time.time() - t0:.1f}s)")
    progress(20)

    log(f"开始识别: {audio_path}")
    t1 = time.time()
    progress(25)

    result = asr(
        audio_path,
        return_timestamps="word",
        generate_kwargs={"language": "zh", "task": "transcribe"},
    )
    elapsed = time.time() - t1
    log(f"识别完成 ({elapsed:.1f}s)")
    progress(85)

    # 解析结果
    words = []
    if "chunks" in result:
        for chunk in result["chunks"]:
            text = chunk.get("text", "").strip()
            ts = chunk.get("timestamp", (0.0, 0.0))

            if isinstance(ts, (list, tuple)) and len(ts) == 2:
                words.append({
                    "word": text,
                    "start": float(ts[0]),
                    "end": float(ts[1]),
                })
            else:
                chunk_start = float(ts[0]) if isinstance(ts, (list, tuple)) else 0.0
                chunk_end = float(ts[1]) if isinstance(ts, (list, tuple)) and len(ts) > 1 else chunk_start + 1.0
                dur = chunk_end - chunk_start
                if dur <= 0 or not text:
                    continue
                per_char = dur / max(1, len(text))
                for i, ch in enumerate(text):
                    words.append({
                        "word": ch,
                        "start": chunk_start + i * per_char,
                        "end": chunk_start + (i + 1) * per_char,
                    })

    # fallback
    if not words:
        full_text = result.get("text", "")
        if not full_text:
            raise RuntimeError("ASR 返回了空文本")
        import soundfile as sf
        info = sf.info(audio_path)
        total_dur = float(info.duration)
        per_char = total_dur / max(1, len(full_text))
        for i, ch in enumerate(full_text):
            words.append({
                "word": ch,
                "start": i * per_char,
                "end": (i + 1) * per_char,
            })

    words = postprocess_words(words)
    full_text = "".join(w["word"] for w in words)
    progress(100)

    return {
        "success": True,
        "text": full_text,
        "words": words,
    }


def run_asr(audio_path: str, device: str, model_name: str) -> dict:
    # CUDA 自动降级：没有 NVIDIA 显卡时默默切到 CPU
    if device == "cuda":
        try:
            import torch
            if not torch.cuda.is_available():
                log("CUDA 不可用，自动降级到 CPU（可以在无显卡电脑运行）")
                device = "cpu"
        except ImportError:
            log("PyTorch 未安装，自动降级到 CPU")
            device = "cpu"

    if "qwen" in model_name.lower():
        return run_asr_qwen(audio_path, device, model_name)
    else:
        return run_asr_whisper(audio_path, device, model_name)


def main():
    parser = argparse.ArgumentParser(description="ASR 语音识别字级时间戳")
    parser.add_argument("--audio", required=True, help="音频文件路径 (wav/mp3/flac)")
    parser.add_argument("--device", default="cuda", help="推理设备: cuda / cpu (默认 cuda)")
    parser.add_argument("--model", default="Qwen/Qwen3-ASR-0.6B",
                        help="HuggingFace 模型名 (默认 Qwen/Qwen3-ASR-0.6B)")
    args = parser.parse_args()

    if not os.path.isfile(args.audio):
        print(json.dumps({"success": False, "error": f"文件不存在: {args.audio}"}),
              file=sys.stderr)
        sys.exit(1)

    try:
        result = run_asr(args.audio, args.device, args.model)
        # 唯一 stdout JSON 输出
        print(json.dumps(result, ensure_ascii=False), flush=True)
        sys.exit(0)
    except Exception as e:
        tb = traceback.format_exc()
        print(json.dumps({"success": False, "error": str(e)}), file=sys.stderr)
        print(f"[ASR 错误] {tb}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
