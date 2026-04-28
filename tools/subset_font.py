#!/usr/bin/env python3
"""
Build a single-file CJK font (NotoSansJP-Medium-subset.ttf) covering all
characters used in main/i18n.cpp's translation tables.

Pipeline:
  1. Extract every non-ASCII char from i18n.cpp's per-language tables.
  2. Download Noto Sans {JP, KR, SC} Bold from Google Fonts CDN.
  3. Subset each font to only the chars it should provide:
       - JP font: JP chars + ASCII + LV_SYMBOL fallback (via Montserrat anyway)
       - KR font: only Hangul syllables used in KR translations
       - SC font: only chars used in ZH_CN translations not already in JP
  4. Merge them into one TTF with fontTools.merge.

Usage:
    python3 tools/subset_font.py
"""
import re
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
I18N_PATH = ROOT / "main" / "i18n.cpp"
OUT_PATH = ROOT / "components/scratch_core/gfx/ingame/fonts/NotoSansJP-Medium-subset.ttf"
CACHE = ROOT / "build" / "fontcache"
CACHE.mkdir(parents=True, exist_ok=True)

# Google Fonts CDN URLs for Noto Sans {JP, KR, SC} weight 700 (Bold).
# Google Fonts CDN URLs change with version bumps. If a download 404s, refresh
# them by querying:
#   curl -sL -A "Mozilla/5.0" \
#     "https://fonts.googleapis.com/css2?family=Noto+Sans+JP:wght@700&subset=japanese"
# (and the equivalent for korean / chinese-simplified) and copy the src URL.
FONTS = {
    "jp": "https://fonts.gstatic.com/s/notosansjp/v56/-F6jfjtqLzI2JPCgQBnw7HFyzSD-AsregP8VFPYk75s.ttf",
    "kr": "https://fonts.gstatic.com/s/notosanskr/v39/PbyxFmXiEBPT4ITbgNA5Cgms3VYcOA-vvnIzzg01eLQ.ttf",
    "sc": "https://fonts.gstatic.com/s/notosanssc/v40/k3kCo84MPvpLmixcA63oeAL7Iqp5IZJF9bmaGzjCnYw.ttf",
}


def download(url: str, dest: Path) -> Path:
    if dest.exists() and dest.stat().st_size > 1000:
        return dest
    print(f"Downloading {url} -> {dest.name}")
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req) as r, open(dest, "wb") as f:
        f.write(r.read())
    return dest


def extract_chars_per_lang(i18n_text: str) -> dict[str, set[int]]:
    """Pull characters out of each `static const char *<NAME>[STR_COUNT] = {...};`."""
    out: dict[str, set[int]] = {}
    for m in re.finditer(
        r"static const char \*(\w+)\[STR_COUNT\]\s*=\s*\{(.*?)\n\};",
        i18n_text,
        re.S,
    ):
        name, body = m.group(1), m.group(2)
        out[name] = set(ord(c) for c in body if ord(c) > 0x80)
    return out


def common_extras() -> set[int]:
    """Punctuation/whitespace useful across all CJK languages."""
    extras = set()
    extras.update(range(0x20, 0x7F))      # ASCII printable (small Latin coverage)
    for c in "「」、。・…？！（）：；〜～":
        extras.add(ord(c))
    return extras


# Native-script language names that appear in the Settings dropdown
# (ui_menu.cpp::LANG_DD_TEXT). Keep in sync if you add a new language.
LANGUAGE_NAMES = "日本語한국어简体中文"


# Baseline JP coverage so user content (Scratch project titles, SSIDs, speech
# bubbles) still renders even if a character isn't in the i18n.cpp tables.
# Hiragana + Katakana full ranges + 223 common kanji from the original subset.
JP_BASELINE_KANJI = (
    "一七万三上下不世中九事二五人今仕会低体何作使供信像兄先光入全八六内円写冬出分初"
    "前力動北十千半南去友口古可史右合同名問喜四回図国土在地夏外多夜夢大天女好妹姉始"
    "嫌子学実家寝小少届山川左希帰年座弟待後心必忘怒思悲想愛憶手技教数新方族日春昼時"
    "書最月望朝木未本村来東校楽次止正歩歴母気水法泣海火父現理生用男町界白百目知短社"
    "秋科秒空立笑第答終緑美習考耳聞育能色花苦英行術西要見覚言計記話語読負質赤走起足"
    "車送過道達違部金長閉開間際雨雪電青音頭頼題風食飲駅高黄黒"
)


def jp_baseline() -> set[int]:
    extras = set()
    extras.update(range(0x3040, 0x30A0))  # Hiragana
    extras.update(range(0x30A0, 0x3100))  # Katakana
    extras.update(ord(c) for c in JP_BASELINE_KANJI)
    return extras


def subset_to_tempfile(src: Path, unicodes: set[int], tag: str) -> Path:
    from fontTools.subset import Subsetter, Options
    from fontTools.ttLib import TTFont

    if not unicodes:
        return None
    out = CACHE / f"{tag}-subset.ttf"
    font = TTFont(str(src))
    opts = Options()
    opts.layout_features = ["*"]
    opts.name_IDs = ["*"]
    opts.notdef_outline = True
    opts.recommended_glyphs = True
    opts.drop_tables = ["DSIG"]  # avoid signature warnings on merge
    sub = Subsetter(options=opts)
    sub.populate(unicodes=list(unicodes))
    sub.subset(font)
    font.save(str(out))
    print(f"  {tag}: {len(unicodes)} chars -> {out.stat().st_size:,} bytes")
    return out


def merge(parts: list[Path], dest: Path) -> None:
    from fontTools.merge import Merger
    merger = Merger()
    merged = merger.merge([str(p) for p in parts])
    merged.save(str(dest))


def main() -> None:
    text = I18N_PATH.read_text(encoding="utf-8")
    chars = extract_chars_per_lang(text)
    extras = common_extras()
    print("Extracted chars per language:")
    for name, s in chars.items():
        print(f"  {name}: {len(s)} chars")

    # The JP font should cover JP chars + every CJK Unified Ideograph that
    # both JP and ZH_CN need, so glyph style stays consistent. KR font only
    # provides Hangul. SC font fills any chars JP doesn't have for ZH_CN.
    jp_chars   = chars.get("JP", set()) | extras | jp_baseline()
    kr_chars   = chars.get("KR", set()) - extras
    zh_chars   = chars.get("ZH_CN", set())

    # Native language names for the Settings dropdown — split by Unicode range
    # and route to the appropriate per-language subset.
    for c in LANGUAGE_NAMES:
        cp = ord(c)
        if 0xAC00 <= cp <= 0xD7A3:           # Hangul Syllables
            kr_chars.add(cp)
        elif 0x4E00 <= cp <= 0x9FFF:         # CJK Unified Ideographs
            zh_chars.add(cp)                 # JP coverage is checked below
        else:                                # Hiragana / Katakana / punct
            jp_chars.add(cp)

    # Try to satisfy ZH_CN from the JP font first (CJK Unified Ideographs
    # overlap heavily). Anything not in JP we pull from SC.
    jp_src = download(FONTS["jp"], CACHE / "NotoSansJP-Bold.ttf")
    kr_src = download(FONTS["kr"], CACHE / "NotoSansKR-Bold.ttf")
    sc_src = download(FONTS["sc"], CACHE / "NotoSansSC-Bold.ttf")

    # Probe JP font's coverage to figure out which ZH chars need SC.
    from fontTools.ttLib import TTFont
    jp_cmap = set(TTFont(str(jp_src)).getBestCmap().keys())

    jp_chars |= (zh_chars & jp_cmap)              # add ZH chars JP can render
    sc_chars  = (zh_chars - jp_cmap)              # remaining ZH chars from SC

    print("Subsetting:")
    parts = []
    p = subset_to_tempfile(jp_src, jp_chars, "jp");
    if p: parts.append(p)
    p = subset_to_tempfile(kr_src, kr_chars, "kr")
    if p: parts.append(p)
    p = subset_to_tempfile(sc_src, sc_chars, "sc")
    if p: parts.append(p)

    print(f"Merging {len(parts)} subsets -> {OUT_PATH.name}")
    merge(parts, OUT_PATH)

    final = TTFont(str(OUT_PATH))
    cmap = set(final.getBestCmap().keys())
    print(f"Final size: {OUT_PATH.stat().st_size:,} bytes, {len(cmap)} glyphs")

    # Sanity check: every char from every translation table + language names
    # is covered.
    missing = []
    for name, s in chars.items():
        miss = [chr(c) for c in s if c not in cmap]
        if miss:
            missing.append((name, miss))
    lang_miss = [c for c in LANGUAGE_NAMES if ord(c) not in cmap]
    if lang_miss:
        missing.append(("LANGUAGE_NAMES", lang_miss))
    if missing:
        print("MISSING GLYPHS:")
        for name, m in missing:
            print(f"  {name}: {''.join(m)}")
        sys.exit(1)
    print("All translation chars covered.")


if __name__ == "__main__":
    main()
