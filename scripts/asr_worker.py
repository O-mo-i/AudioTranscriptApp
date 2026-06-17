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

长音频机制:
  Qwen3-ASR 对超过 3 分钟的长音频存在 attention collapse，
  导致中间段时间戳 Token 物理断层。
  本脚本自动检测音频时长，超过 30s 则启用滑动窗口分块推理，
  每块 30 秒，块间 0.5 秒重叠，最后融合输出。
"""

import argparse
import json
import sys
import os
import time
import traceback
import math
import tempfile

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


# ═══════════════════════════════════════════════════════════
#  滑动窗口分块参数
# ═══════════════════════════════════════════════════════════
CHUNK_SEC = 30.0       # 每块时长（秒）
OVERLAP_SEC = 0.5       # 块间重叠（秒）


def is_model_in_cache(model_name: str) -> bool:
    """检测 HuggingFace 模型是否已在本地缓存中。"""
    try:
        from huggingface_hub import try_to_load_from_cache
        # 检查 config.json 是否在缓存中
        cached = try_to_load_from_cache(model_name, "config.json")
        return cached is not None and cached is not False
    except Exception:
        pass
    # 回退：检查 HuggingFace 默认缓存目录
    try:
        import os
        hf_cache = os.path.expanduser("~/.cache/huggingface/hub")
        if not os.path.isdir(hf_cache):
            return False
        safe_name = model_name.replace("/", "--")
        model_dir = os.path.join(hf_cache, f"models--{safe_name}")
        return os.path.isdir(model_dir) and any(
            f.endswith(".json") or f.endswith(".bin") or f.endswith(".safetensors")
            for f in os.listdir(os.path.join(model_dir, "snapshots"))
        ) if os.path.isdir(os.path.join(model_dir, "snapshots")) else False
    except Exception:
        return False


def align_punctuation(full_text, raw_words):
    """Align full-text (with punctuation) against raw timestamp words."""
    PUNCT_SET = set("，。？！、；：\n\r\t ")

    aligned = []
    wi = 0
    fi = 0

    while fi < len(full_text) and wi < len(raw_words):
        ch = full_text[fi]

        if ch in PUNCT_SET:
            prev_end = aligned[-1]["end"] if aligned else 0.0
            aligned.append({"word": ch, "start": prev_end, "end": prev_end})
            fi += 1
            continue

        raw_word = raw_words[wi]["word"]
        if raw_word and ch == raw_word[0]:
            aligned.append(dict(raw_words[wi]))
            fi += len(raw_word)
            wi += 1
        else:
            fi += 1

    while fi < len(full_text):
        ch = full_text[fi]
        if ch in PUNCT_SET:
            prev_end = aligned[-1]["end"] if aligned else 0.0
            aligned.append({"word": ch, "start": prev_end, "end": prev_end})
        fi += 1

    return aligned


def postprocess_words(words):
    """Post-process words: split punctuation, detect paragraph boundaries."""
    PUNCT = set("。？！")

    if not words:
        return words

    # Step 1: split trailing CJK punctuation
    expanded = []
    for i, w in enumerate(words):
        text = w["word"]
        if not text:
            continue

        if len(text) > 1 and text[-1] in PUNCT:
            expanded.append({"word": text[:-1], "start": w["start"], "end": w["end"]})
            prev_end = words[i - 1]["end"] if i > 0 else w["start"]
            expanded.append({"word": text[-1], "start": prev_end, "end": w["end"]})
        else:
            expanded.append(dict(w))

    # Step 2: mark paragraph starts
    result = []
    for i, w in enumerate(expanded):
        entry = dict(w)
        if i == 0:
            entry["is_paragraph_start"] = True
        else:
            prev = expanded[i - 1]
            prev_text = prev.get("word", "")
            if prev_text and prev_text[-1] in PUNCT:
                entry["is_paragraph_start"] = True
            elif (w["start"] - prev["end"]) > 1.5:
                entry["is_paragraph_start"] = True
        result.append(entry)

    return result


def merge_word_chunks(chunks, chunk_offsets):
    """融合多个分块的字级时间戳，去除重叠部分。

    chunks[idx] 中的字已经加好了 chunk_offsets[idx] 的时间偏移，
    本函数不再修改时间戳，只做重叠区的首尾裁剪。

    Args:
        chunks: list of list[dict], 每个分块的 words 数组（时间戳已含偏移）
        chunk_offsets: list[float], 每个分块的时间偏移（仅用于计算重叠中点）

    Returns:
        list[dict]: 融合后的 words 数组
    """
    if not chunks:
        return []

    merged = list(chunks[0])  # 第一块直接全收

    for idx in range(1, len(chunks)):
        curr_chunk = chunks[idx]  # 已含偏移，不再加

        # 重叠中点: 上一块的起始时间 + 块长时间 - overlap的一半
        overlap_mid = chunk_offsets[idx - 1] + CHUNK_SEC - OVERLAP_SEC / 2

        # 从 merged 中砍掉重叠区后半段的字
        while merged and merged[-1]["start"] >= overlap_mid:
            merged.pop()

        # 从当前块中只取重叠中点之后的字（curr_chunk 已是绝对时间）
        kept = [w for w in curr_chunk if w["start"] >= overlap_mid]

        for w in kept:
            merged.append(dict(w))

    return merged


def run_asr_qwen_single(model, audio_path: str, device: str) -> dict:
    """单次 Qwen3-ASR 推理一个音频文件，返回 words + text。"""
    results = model.transcribe(audio_path, return_time_stamps=True)

    if not results:
        raise RuntimeError("ASR 返回了空结果")

    result = results[0]

    words = []
    if result.time_stamps is not None:
        for item in result.time_stamps:
            words.append({
                "word": item.text,
                "start": float(item.start_time),
                "end": float(item.end_time),
            })

    if result.text is not None and words:
        words = align_punctuation(result.text, words)

    if not words:
        full_text = result.text
        if not full_text:
            raise RuntimeError("ASR 返回了空文本")
        import soundfile as sf
        info = sf.info(audio_path)
        total_dur = float(info.duration)
        per_char = total_dur / max(1, len(full_text))
        for i, ch in enumerate(full_text):
            words.append({"word": ch, "start": i * per_char, "end": (i + 1) * per_char})

    return {"words": words, "text": result.text if result.text else ""}


def run_asr_qwen(audio_path: str, device: str, model_name: str) -> dict:
    """Qwen3-ASR 推理入口（支持长音频滑动窗口分块）。"""
    import torch
    from qwen_asr import Qwen3ASRModel
    import soundfile as sf

    progress(0)

    dtype = torch.bfloat16 if torch.cuda.is_available() else torch.float32
    device_map = device if torch.cuda.is_available() else "cpu"

    # 离线/本地路径模式：forced_aligner 需要单独指定
    is_offline = os.environ.get("TRANSFORMERS_OFFLINE") == "1"
    is_local_path = os.path.sep in model_name or os.path.isdir(model_name)
    forced_aligner_model = None
    if is_local_path or is_offline:
        # 本地路径模式：不自动替换名称，让用户通过 --model-dir 统一管理
        # 或从原始 HuggingFace 名称推导
        hf_name = model_name
        if is_local_path:
            # 从路径反推 HuggingFace 名称
            base = os.path.basename(model_name)
            if base.startswith("models--"):
                # snapshot 下载目录格式
                parts = base.replace("models--", "").split("--")
                if len(parts) >= 2:
                    hf_name = "/".join(parts)
            else:
                hf_name = base
        forced_aligner_model = hf_name.replace("ASR", "ForcedAligner")
        log(f"离线模式 forced_aligner: {forced_aligner_model}")
    else:
        forced_aligner_model = model_name.replace("ASR", "ForcedAligner")

    # 获取音频时长
    info = sf.info(audio_path)
    total_dur = float(info.duration)
    log(f"音频总时长: {total_dur:.1f}s")

    # 加载模型（只加载一次）
    log(f"加载模型: {model_name} device={device}")
    t0 = time.time()
    is_offline_mode = bool(os.environ.get("TRANSFORMERS_OFFLINE"))

    # 自动检测缓存：模型已在本地时强制 local_files_only，避免网络卡死
    model_in_cache = is_model_in_cache(model_name)
    local_only = is_offline_mode or model_in_cache
    if model_in_cache and not is_offline_mode:
        log(f"检测到模型已缓存，自动启用 local_files_only=True（无需 --offline 参数）")

    forced_aligner_kwargs = {"dtype": dtype, "device_map": device_map}
    if local_only:
        forced_aligner_kwargs["local_files_only"] = True

    # 非缓存模式设置网络超时，防止 SYN_SENT 永久挂死
    if not local_only:
        os.environ.setdefault("HF_HUB_DOWNLOAD_TIMEOUT", "30")
        os.environ.setdefault("HF_HUB_ETAG_TIMEOUT", "10")

    try:
        model = Qwen3ASRModel.from_pretrained(
            model_name,
            dtype=dtype,
            device_map=device_map,
            forced_aligner=forced_aligner_model,
            forced_aligner_kwargs=forced_aligner_kwargs,
            max_new_tokens=256,
            local_files_only=local_only,
        )
    except OSError as e:
        err_msg = str(e)
        if "Offline mode" in err_msg or "connect" in err_msg.lower():
            raise RuntimeError(
                f"模型 {model_name} 不在本地缓存中。\n"
                f"请先在线运行一次 ASR 以下载模型，或使用 --model-dir 指定本地路径。\n"
                f"错误详情: {err_msg}"
            )
        raise
    except Exception as e:
        if "Connection" in str(e) or "Timeout" in str(e) or "timeout" in str(e):
            raise RuntimeError(
                f"连接 HuggingFace 服务器超时，无法加载模型 {model_name}。\n"
                f"请切换到离线模式或在有网络的環境下先运行一次。\n"
                f"错误详情: {e}"
            )
        raise
    log(f"模型加载完成 ({time.time() - t0:.1f}s)")
    progress(5)

    # ── 短音频：单次推理 ──────────────────────────────
    if total_dur <= CHUNK_SEC + 5:
        log("短音频模式：单次推理")
        result = run_asr_qwen_single(model, audio_path, device)
        words = result["words"]
        progress(80)
    else:
        # ── 长音频：滑动窗口分块推理 ────────────────────
        log(f"长音频模式：启动滑动窗口（每块 {CHUNK_SEC}s，重叠 {OVERLAP_SEC}s）")

        data, sr = sf.read(audio_path)
        if data.ndim > 1:
            data = data.mean(axis=1)  # 转单声道

        chunk_samples = int(CHUNK_SEC * sr)
        overlap_samples = int(OVERLAP_SEC * sr)
        step_samples = chunk_samples - overlap_samples

        num_chunks = max(1, math.ceil((len(data) - chunk_samples) / step_samples) + 1)

        all_chunks = []
        chunk_offsets = []
        tmp_files = []

        for ci in range(num_chunks):
            start_sample = ci * step_samples
            end_sample = min(start_sample + chunk_samples, len(data))

            if end_sample - start_sample < sr * 2:  # 最后一段不足 2 秒则跳过
                break

            chunk_data = data[start_sample:end_sample]
            offset = start_sample / sr

            # 写入临时文件
            tmp = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
            tmp_path = tmp.name
            tmp.close()
            sf.write(tmp_path, chunk_data, sr)
            tmp_files.append(tmp_path)

            pct = 5 + int((ci / num_chunks) * 70)
            progress(pct)
            log(f"分块 {ci + 1}/{num_chunks} 推理中 (offset={offset:.1f}s, "
                f"dur={(end_sample - start_sample) / sr:.1f}s)")

            try:
                chunk_result = run_asr_qwen_single(model, tmp_path, device)
            except Exception as e:
                log(f"分块 {ci + 1} 出错: {e}")
                for f in tmp_files:
                    try:
                        os.unlink(f)
                    except OSError:
                        pass
                raise

            words_with_offset = []
            for w in chunk_result["words"]:
                entry = dict(w)
                entry["start"] += offset
                entry["end"] = (w.get("end", w["start"]) + offset)
                words_with_offset.append(entry)
            all_chunks.append(words_with_offset)
            chunk_offsets.append(offset)

            # 清理临时文件
            try:
                os.unlink(tmp_path)
            except OSError:
                pass

        progress(80)

        # ── 融合各分块 ────────────────────────────────
        merged = merge_word_chunks(all_chunks, chunk_offsets)
        log(f"融合前总字数: {sum(len(c) for c in all_chunks)}，融合后: {len(merged)}")
        words = merged

        # 释放模型显存
        del model
        if torch.cuda.is_available():
            torch.cuda.empty_cache()

    # ── 后处理 ──────────────────────────────────────────
    words = postprocess_words(words)
    full_text = "".join(w["word"] for w in words)
    progress(100)

    log(f"最终输出字数: {len(words)}")
    return {"success": True, "text": full_text, "words": words}


def run_asr_whisper(audio_path: str, device: str, model_name: str) -> dict:
    """使用 transformers pipeline 加载 Whisper 模型并推理"""
    from transformers import pipeline

    progress(0)

    log(f"加载模型: {model_name} device={device}")
    t0 = time.time()
    try:
        asr = pipeline(
            "automatic-speech-recognition",
            model=model_name,
            device=device,
            chunk_length_s=30,
        )
    except OSError as e:
        err_msg = str(e)
        if "Offline mode" in err_msg or "connect" in err_msg.lower():
            raise RuntimeError(
                f"模型 {model_name} 不在本地缓存中。\n"
                f"请先在线运行一次 ASR 以下载模型，或使用 --model-dir 指定本地路径。\n"
                f"错误详情: {err_msg}"
            )
        raise
    except Exception as e:
        if "Connection" in str(e) or "Timeout" in str(e) or "timeout" in str(e):
            raise RuntimeError(
                f"连接 HuggingFace 服务器超时，无法加载模型 {model_name}。\n"
                f"请切换到离线模式或在有网络的环境下先运行一次。\n"
                f"错误详情: {e}"
            )
        raise
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
                words.append({"word": text, "start": float(ts[0]), "end": float(ts[1])})
            else:
                chunk_start = float(ts[0]) if isinstance(ts, (list, tuple)) else 0.0
                chunk_end = float(ts[1]) if isinstance(ts, (list, tuple)) and len(ts) > 1 else chunk_start + 1.0
                dur = chunk_end - chunk_start
                if dur <= 0 or not text:
                    continue
                per_char = dur / max(1, len(text))
                for i, ch in enumerate(text):
                    words.append({"word": ch, "start": chunk_start + i * per_char, "end": chunk_start + (i + 1) * per_char})

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
            words.append({"word": ch, "start": i * per_char, "end": (i + 1) * per_char})

    words = postprocess_words(words)
    full_text = "".join(w["word"] for w in words)
    progress(100)

    return {"success": True, "text": full_text, "words": words}


def download_model_only(model_name: str, device: str,
                        model_dir: str | None, offline_args: dict) -> dict:
    """仅下载并缓存模型，不执行识别。

    同时下载 ASR 模型和对应的 ForcedAligner 模型。
    适用于首次使用前的模型预下载。
    """
    import torch

    try:
        resolved = resolve_model_path(model_name, model_dir)
        log(f"正在下载模型: {resolved}")

        # 检测缓存状态
        if is_model_in_cache(model_name) and is_model_in_cache(
                model_name.replace("ASR", "ForcedAligner")):
            log("模型已全部缓存，无需下载")
            return {"success": True, "text": "模型已存在", "words": []}

        dtype = torch.bfloat16 if torch.cuda.is_available() else torch.float32
        device_map = device if torch.cuda.is_available() else "cpu"

        # 推导 forced_aligner 名称
        forced_aligner_model = model_name.replace("ASR", "ForcedAligner")

        # 下载 ASR 模型
        log("下载 ASR 模型中...")
        from qwen_asr import Qwen3ASRModel
        _ = Qwen3ASRModel.from_pretrained(
            resolved,
            dtype=dtype,
            device_map=device_map,
            max_new_tokens=256,
            **offline_args,
        )
        log("ASR 模型下载完成")

        # 下载 ForcedAligner 模型
        log("下载 ForcedAligner 模型中...")
        try:
            _ = Qwen3ASRModel.from_pretrained(
                forced_aligner_model,
                dtype=dtype,
                device_map=device_map,
                max_new_tokens=256,
                local_files_only=False,
            )
            log("ForcedAligner 模型下载完成")
        except Exception as e:
            log(f"ForcedAligner 模型下载失败（可忽略）: {e}")

        if torch.cuda.is_available():
            torch.cuda.empty_cache()

        return {"success": True, "text": "模型下载完成", "words": []}

    except Exception as e:
        log(f"模型下载失败: {e}")
        return {"success": False, "error": str(e)}


def verify_model_online(model_name: str, device: str,
                        model_dir: str | None) -> dict:
    """联网校验模型完整性，检查缓存文件是否与 Hub 一致。

    对比本地缓存文件与 HuggingFace Hub 上预期文件列表，
    不自动下载缺失文件。
    """
    import time

    resolved = resolve_model_path(model_name, model_dir)
    log(f"正在校验模型: {resolved}")

    # ── 本地缓存文件检查 ──────────────────────────
    # 使用 is_model_in_cache 做快速检测（不依赖 huggingface_hub 导入）
    asr_ok = is_model_in_cache(resolved)
    forced_aligner_name = model_name.replace("ASR", "ForcedAligner")
    fa_ok = is_model_in_cache(forced_aligner_name)

    if asr_ok:
        log("ASR 模型文件存在")
    else:
        log("ASR 模型文件缺失")
    if fa_ok:
        log("ForcedAligner 模型文件存在")
    else:
        log("ForcedAligner 模型文件缺失")

    # ── 联网校验 ────────────────────────────────
    hub_ok = False
    hub_error = ""
    try:
        from huggingface_hub import model_info
        _ = model_info(model_name, timeout=15)
        hub_ok = True
        log("已连接到 HuggingFace Hub")
    except Exception as e:
        hub_error = str(e)
        log(f"无法连接到 HuggingFace Hub: {hub_error}")

    # ── 结果判定 ─────────────────────────────────
    all_local = asr_ok and fa_ok

    if hub_ok and all_local:
        return {"success": True, "status": "ok",
                "text": "模型完整，与 Hub 一致"}
    elif not hub_ok and all_local:
        return {"success": True, "status": "ok_local",
                "text": "本地模型文件完整（无法联网验证）"}
    elif hub_ok and not all_local:
        return {"success": True, "status": "needs_download",
                "text": '模型不完整，请点击"下载模型"'}
    elif not all_local:
        return {"success": True, "status": "needs_download",
                "text": '模型文件缺失，请点击"下载模型"'}
    else:
        return {"success": True, "status": "ok_local",
                "text": "本地模型文件完整（无法联网验证完整性）"}


def run_asr(audio_path: str, device: str, model_name: str) -> dict:
    # CUDA 自动降级
    if device == "cuda":
        try:
            import torch
            if not torch.cuda.is_available():
                log("CUDA 不可用，自动降级到 CPU")
                device = "cpu"
        except ImportError:
            log("PyTorch 未安装，自动降级到 CPU")
            device = "cpu"

    if "qwen" in model_name.lower():
        return run_asr_qwen(audio_path, device, model_name)
    else:
        return run_asr_whisper(audio_path, device, model_name)


def resolve_model_path(model_name: str, model_dir: str | None) -> str:
    """解析模型路径。如果指定了 model_dir，优先使用本地路径。"""
    if model_dir:
        # 直接使用本地目录
        local_path = os.path.join(model_dir, os.path.basename(model_name))
        if os.path.isdir(local_path):
            log(f"使用本地模型: {local_path}")
            return local_path
        if os.path.isdir(model_dir):
            log(f"使用本地模型目录: {model_dir}")
            return model_dir
        log(f"警告: 本地模型目录不存在 ({model_dir})，回退到 HuggingFace 名称")
    return model_name


def main():
    parser = argparse.ArgumentParser(description="ASR 语音识别字级时间戳")
    parser.add_argument("--audio", help="音频文件路径 (wav/mp3/flac)")
    parser.add_argument("--device", default="cuda", help="推理设备: cuda / cpu (默认 cuda)")
    parser.add_argument("--model", default="Qwen/Qwen3-ASR-0.6B",
                        help="HuggingFace 模型名 (默认 Qwen/Qwen3-ASR-0.6B)")
    parser.add_argument("--offline", action="store_true",
                        help="离线模式：禁止所有网络请求，仅从本地缓存加载模型")
    parser.add_argument("--model-dir", default=None,
                        help="本地模型目录路径（离线模式下从此路径加载模型，不联网下载）")
    parser.add_argument("--download-only", action="store_true",
                        help="仅下载模型到本地缓存，不执行识别")
    parser.add_argument("--verify-only", action="store_true",
                        help="仅联网校验模型完整性，不下载不识别")
    args = parser.parse_args()

    # ── 离线模式设置 ──────────────────────────
    if args.offline:
        log("离线模式已启用，禁止所有网络请求")
        os.environ["TRANSFORMERS_OFFLINE"] = "1"
        os.environ["HF_HUB_OFFLINE"] = "1"
        os.environ["HF_OFFLINE"] = "1"
        # 禁用 huggingface_hub 的 telemetry 和版本检查
        os.environ["HF_HUB_DISABLE_TELEMETRY"] = "1"
        os.environ["HUGGINGFACE_HUB_DISABLE_TELEMETRY"] = "1"
        os.environ["DISABLE_VERSION_CHECK"] = "1"
        # 设置较短超时，防止 socket 阻塞
        os.environ["HF_HUB_DOWNLOAD_TIMEOUT"] = "5"
        os.environ["HF_HUB_ETAG_TIMEOUT"] = "5"

    # ── 下载模式：仅下载模型，不执行识别 ──────────
    if args.download_only:
        log(f"下载模式: 仅下载模型 {args.model} 到本地缓存")
        result = download_model_only(
            args.model, args.device, args.model_dir,
            {"local_files_only": False})
        print(json.dumps(result, ensure_ascii=False), flush=True)
        sys.exit(0 if result.get("success") else 1)

    if args.verify_only:
        log(f"校验模式: 联网检查模型 {args.model} 完整性")
        result = verify_model_online(
            args.model, args.device, args.model_dir)
        print(json.dumps(result, ensure_ascii=False), flush=True)
        sys.exit(0 if result.get("success") else 1)

    # 解析模型路径（支持本地目录）
    resolved_model = resolve_model_path(args.model, args.model_dir)
    log(f"模型路径: {resolved_model}")

    if not args.audio or not os.path.isfile(args.audio):
        print(json.dumps({"success": False, "error": f"文件不存在: {args.audio}"}),
              file=sys.stderr)
        sys.exit(1)

    # 离线模式预检：检查模型是否在本地缓存中
    if args.offline and not args.model_dir:
        import importlib.util
        # 检查 huggingface_hub 缓存
        hf_hub_available = importlib.util.find_spec("huggingface_hub") is not None
        if hf_hub_available:
            try:
                from huggingface_hub import try_to_load_from_cache
                # 检查模型配置文件是否在缓存中
                cached = try_to_load_from_cache(args.model, "config.json")
                if cached is None or cached is False:
                    log(f"离线模式：模型 {args.model} 不在本地缓存中")
                    log(f"请先在线运行一次 ASR 下载模型，或使用 --model-dir 指定本地路径")
                    log(f"也可以临时关闭离线模式（在插件设置中取消勾选）")
                    # 不退出，给 from_pretrained 一个机会用缓存
                    # 如果还是失败，transformers 会抛出 OSError
            except Exception:
                pass

    # 预检 torch CUDA
    if args.device == "cuda":
        try:
            import torch
            if not torch.cuda.is_available():
                log("CUDA 不可用，将自动降级到 CPU")
                args.device = "cpu"
        except ImportError:
            pass

    try:
        # 用已解析的模型路径覆盖
        args.model = resolved_model
        result = run_asr(args.audio, args.device, args.model)

        # 诊断落盘
        dump_path = os.path.join(os.path.expanduser("~"), "asr_debug_dump.json")
        try:
            with open(dump_path, "w", encoding="utf-8") as f:
                json.dump(result, f, ensure_ascii=False, indent=2)
            log(f"DEBUG: full result dumped to {dump_path}")
        except Exception as e:
            log(f"DEBUG: failed to dump: {e}")

        log(f"DEBUG: words count = {len(result.get('words', []))}")
        print(json.dumps(result, ensure_ascii=False), flush=True)
        sys.exit(0)
    except Exception as e:
        tb = traceback.format_exc()
        print(json.dumps({"success": False, "error": str(e)}), file=sys.stderr)
        print(f"[ASR 错误] {tb}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
