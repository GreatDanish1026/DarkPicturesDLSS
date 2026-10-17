"""Analyses a QuarryDLSS / DarkPicturesDLSS texture capture (Ctrl+F8 / Ctrl+Shift+F8 in the overlay).

A capture holds two consecutive DLSS frames (suffix 0 and 1; 0.5.18+) or one (no suffix).
For the last frame it recomputes what dlss_prepare.hlsl does per render pixel: the game's
decoded velocity and the camera-only reprojection, and compares them per screen region.

With two frames it also tests which motion is right: each candidate motion field (game
velocity, camera only, inverted game velocity, zero, and what DLSS actually got) maps
frame 1's pixels to where they were in frame 0; the closer frame 0's colour there is to
frame 1's, the better the motion. Reported over all pixels and over the pixels where the
game's velocity disagrees with camera motion by more than 1 px.

PNGs (prefix <frame>): _color, _object (game velocity present), _diff (|decoded - camera|,
white >= 8 px), _motion (hue = direction, brightness = length, 16 px full), and with two
frames _err_<candidate> (reprojection error, white >= 0.5 of the frame's mean luma).

Usage: python analyze_capture.py <capture .txt> --out <dir>   (needs numpy and Pillow)
"""
import argparse
import colorsys
import os
import re

import numpy as np
from PIL import Image


def read_header(path):
    h = {"textures": {}}
    for line in open(path):
        parts = line.split()
        if not parts:
            continue
        key = parts[0]
        sfx = key[-1] if key[-1] in "01" and key not in ("texture",) else ""
        base = key[:-1] if sfx else key
        if base == "frame":
            h["frame" + sfx], h["age" + sfx] = int(parts[1]), int(parts[3])
        elif base == "render":
            h["render"] = tuple(int(v) for v in parts[1].split("x"))
            h["output"] = tuple(int(v) for v in parts[4].split("x"))
        elif base == "preexposure":
            h["pre" + sfx] = float(parts[1])
        elif base == "clip_to_prev_clip":
            h["c2p" + sfx] = np.array([float(v) for v in parts[1:17]]).reshape(4, 4)
    # Texture lines: add-ons before 0.1.2 / 1.1.1 cut them off (and their newline) when the
    # game path is long, so find them anywhere and derive each file name from the header's.
    base = os.path.splitext(path)[0]
    for name, w, hh, fmt in re.findall(r"texture (\w+) (\d+)x(\d+) dxgi_format (\d+)", open(path).read()):
        h["textures"][name] = (int(w), int(hh), int(fmt), f"{base}_{name}.bin")
    return h


def load(h, name, dtype, channels, size=None):
    w, hh, fmt, path = h["textures"][name]
    a = np.fromfile(path, dtype=dtype)
    rw, rh = size or h["render"]
    a = a.reshape(hh, w, channels) if channels > 1 else a.reshape(hh, w)
    return a[:rh, :rw]


def load_color(h, name, size=None):
    """RGB as float64, from RGBA16F (DXGI 10) or R11G11B10F (DXGI 26, The Devil in Me)."""
    fmt = h["textures"][name][2]
    if fmt == 10:
        return load(h, name, np.float16, 4, size)[..., :3].astype(np.float64)
    if fmt != 26:
        raise SystemExit(f"{name}: colour format DXGI {fmt} not supported")
    v = load(h, name, np.uint32, 1, size).astype(np.uint64)

    def small_float(bits, mbits):
        m = (bits & ((1 << mbits) - 1)).astype(np.float64)
        e = ((bits >> mbits) & 31).astype(np.float64)
        return np.where(e == 0, m / (1 << mbits) * 2.0 ** -14, 2.0 ** (e - 15) * (1.0 + m / (1 << mbits)))

    return np.stack([small_float(v & 0x7FF, 6), small_float((v >> 11) & 0x7FF, 6), small_float(v >> 22, 5)], axis=-1)


def motions(h, sfx):
    """Decoded game velocity, camera reprojection (both DLSS pixels: current -> previous),
    the object mask, and the motion DLSS got, for frame <sfx>."""
    rw, rh = h["render"]
    vel = load(h, "velocity" + sfx, np.uint16, 4).astype(np.float64) / 65535.0
    depth = load(h, "depth" + sfx, np.float32, 1).astype(np.float64)
    got = load(h, "motion" + sfx, np.float16, 2).astype(np.float64)
    ys, xs = np.mgrid[0:rh, 0:rw]
    sx = (xs + 0.5) / rw * 2.0 - 1.0
    sy = 1.0 - (ys + 0.5) / rh * 2.0
    prev = np.stack([sx, sy, depth, np.ones_like(depth)], axis=-1) @ h["c2p" + sfx]
    to_px = lambda dx, dy: np.stack((-dx * 0.5 * rw, dy * 0.5 * rh), axis=-1)
    cam = to_px(sx - prev[..., 0] / prev[..., 3], sy - prev[..., 1] / prev[..., 3])
    inv_div = 1.0 / (0.499 * 0.5)
    has_obj = vel[..., 0] > 0.0
    dec = to_px(vel[..., 0] * inv_div - 32767.0 / 65535.0 * inv_div,
                vel[..., 1] * inv_div - 32767.0 / 65535.0 * inv_div)
    dec[~has_obj] = 0.0
    return dec, cam, has_obj, got


def bilinear(img, x, y):
    h, w = img.shape[:2]
    x = np.clip(x - 0.5, 0, w - 1.001)
    y = np.clip(y - 0.5, 0, h - 1.001)
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = (x - x0)[..., None], (y - y0)[..., None]
    a, b = img[y0, x0], img[y0, x0 + 1]
    c, d = img[y0 + 1, x0], img[y0 + 1, x0 + 1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def save_gray(path, v):
    Image.fromarray((np.clip(v, 0, 1) * 255).astype(np.uint8)).save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("header")
    ap.add_argument("--out", default=".")
    args = ap.parse_args()
    h = read_header(args.header)
    pair = "color1" in h["textures"]
    s = "1" if pair else ""
    rw, rh = h["render"]
    tag = os.path.join(args.out, str(h["frame" + s]))

    dec, cam, has_obj, got = motions(h, s)
    diff = np.linalg.norm(dec - cam, axis=-1)
    diff[~has_obj] = 0.0
    print(f"frame {h['frame' + s]} age {h['age' + s]} render {rw}x{rh} pre {h.get('pre' + s)}"
          + (f" (pair with frame {h['frame0']})" if pair else ""))
    print(f"object-velocity pixels: {has_obj.mean() * 100:.1f}%")
    gx, gy = 4, 3
    print(f"per region ({gx}x{gy}, rows top to bottom), over object pixels: |decoded|, |camera|, "
          "|decoded - camera|, mean decoded and camera vectors")
    for j in range(gy):
        cells = []
        for i in range(gx):
            sl = (slice(j * rh // gy, (j + 1) * rh // gy), slice(i * rw // gx, (i + 1) * rw // gx))
            m = has_obj[sl]
            if m.sum() == 0:
                cells.append(f"obj 0% cam {np.linalg.norm(cam[sl], axis=-1).mean():.2f}")
                continue
            d, c = dec[sl][m], cam[sl][m]
            cells.append(f"obj {m.mean() * 100:.0f}% |d| {np.linalg.norm(d, axis=-1).mean():.2f} "
                         f"|c| {np.linalg.norm(c, axis=-1).mean():.2f} diff {diff[sl][m].mean():.2f} "
                         f"d({d[:, 0].mean():+.2f},{d[:, 1].mean():+.2f}) c({c[:, 0].mean():+.2f},{c[:, 1].mean():+.2f})")
        print("  " + " | ".join(cells))

    color = load_color(h, "color" + s)
    tone = lambda c: np.clip(c / (1.0 + c), 0, 1) ** (1 / 2.2)
    Image.fromarray((tone(color) * 255).astype(np.uint8)).save(tag + "_color.png")
    save_gray(tag + "_object.png", has_obj.astype(float))
    save_gray(tag + "_diff.png", diff / 8.0)
    mlen = np.linalg.norm(got, axis=-1)
    ang = (np.arctan2(got[..., 1], got[..., 0]) / (2 * np.pi)) % 1.0
    rgb = np.vectorize(colorsys.hsv_to_rgb, otypes=[float] * 3)(ang, np.ones_like(ang), np.clip(mlen / 16.0, 0, 1))
    Image.fromarray((np.stack(rgb, axis=-1) * 255).astype(np.uint8)).save(tag + "_motion.png")

    if pair:
        # Compare in absolute luminance: undo each frame's pre-exposure.
        c1 = color / h["pre1"]
        c0 = load_color(h, "color0") / h["pre0"]
        lum = lambda c: c @ np.array([0.2126, 0.7152, 0.0722])
        l1, mean = lum(c1), max(lum(c1).mean(), 1e-6)
        ys, xs = np.mgrid[0:rh, 0:rw]
        game = np.where(has_obj[..., None], dec, cam)
        candidates = {"game": game, "camera": cam, "inverted": np.where(has_obj[..., None], -dec, cam),
                      "zero": np.zeros_like(cam), "dlss": got}
        disagree = has_obj & (diff > 1.0)
        print(f"reprojection error, mean |luma(frame1) - luma(frame0 at p + motion)| / mean luma "
              f"(lower is better); disagreeing pixels: {disagree.mean() * 100:.1f}%")
        for name, m in candidates.items():
            err = np.abs(l1 - lum(bilinear(c0, xs + 0.5 + m[..., 0], ys + 0.5 + m[..., 1]))) / mean
            line = f"  {name:9s} all {err.mean():.4f}"
            if disagree.any():
                line += f"  disagreeing {err[disagree].mean():.4f}"
            for j in range(gy):
                line += " |"
                for i in range(gx):
                    sl = (slice(j * rh // gy, (j + 1) * rh // gy), slice(i * rw // gx, (i + 1) * rw // gx))
                    line += f" {err[sl].mean():.3f}"
            print(line)
            save_gray(f"{tag}_err_{name}.png", err / 0.5)
    if "output" + s in h["textures"]:
        # DLSS output vs its input: downsample the output to render size and compare luma.
        ow, oh = h["output"]
        out = load(h, "output" + s, np.float16, 4, (ow, oh))[..., :3].astype(np.float32)
        Image.fromarray((tone(out) * 255).astype(np.uint8)).save(tag + "_output.png")
        small = np.stack([np.asarray(Image.fromarray(out[..., c]).resize((rw, rh), Image.BILINEAR)) for c in range(3)],
                         axis=-1).astype(np.float64)
        lum = lambda c: c @ np.array([0.2126, 0.7152, 0.0722])
        li, lo = lum(color), lum(small)
        rel = np.abs(lo - li) / max(li.mean(), 1e-6)
        print(f"DLSS output vs input (luma, output downsampled): input mean {li.mean():.4f}, output mean {lo.mean():.4f}, "
              f"mean |diff| / mean input {rel.mean():.3f}")
        for j in range(gy):
            line = "  "
            for i in range(gx):
                sl = (slice(j * rh // gy, (j + 1) * rh // gy), slice(i * rw // gx, (i + 1) * rw // gx))
                line += f" in {li[sl].mean():.3f} out {lo[sl].mean():.3f} |d| {rel[sl].mean():.3f} |"
            print(line)
        save_gray(tag + "_outdiff.png", rel / 0.5)
    print("wrote PNGs to " + tag + "_*.png")


if __name__ == "__main__":
    main()
