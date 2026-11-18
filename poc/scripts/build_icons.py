"""
PlatformIO pre-build hook: reads the icon manifest, fetches each icon's source
PNG from Icons8 if it isn't already present on disk, converts it into an LVGL C
array via LVGL's own LVGLImage.py, and generates the icon registry header
(including which icon the welcome screen uses).

Scoped-down version of the real pipeline in ../../docs/ARCHITECTURE.md §12 —
one runtime size (this POC only ever renders one icon on one screen, so the
real pipeline's two-runtime-size scheme would be unused complexity here).
Reuses the same manifest schema and the same "fail loudly if the manifest and
what's referenced in code disagree" philosophy.

LVGLImage.py's exact CLI flags have changed across LVGL releases; the
invocation below is verified against the lvgl 9.x actually fetched into
.pio/libdeps/esp32-c6/. If a version bump breaks it, run
`python <path-to-LVGLImage.py> --help` and adjust.

--------------------------------------------------------------------------
DEVIATION FROM ARCHITECTURE.md §12.2 — READ THIS
--------------------------------------------------------------------------
§12.2 specifies that source acquisition is a deliberate MANUAL per-icon step,
because Icons8's free tier (chosen in §12.1) has no API access, and their ToS
does not permit automated download outside their official paid API. It says
the pipeline is "deliberately **not** a script that crawls icons8.com", and
that the only sanctioned automation is "a thin script calling Icons8's
**official API** with a valid key".

This script automates that step against img.icons8.com, which is the website's
own CDN, not the official API. That was an explicit product decision — but it
means one of the following needs to be true before this ships:

  * the project is on a paid Icons8 tier whose licence permits this, or
  * §12.1/§12.2 are revised to record the new decision, or
  * this is swapped for the official-API script §12.2 describes (only
    resolve_download_url() below would need to change).

Independently of which: §12.1's free-tier attribution requirement still
applies. The manifest carries an `attribution` string for that purpose, and
it needs to reach a user-visible surface (the §5.2 "showAbout" screen), not
just this file.
--------------------------------------------------------------------------
"""

import json
import re
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

Import("env")  # noqa: F821 — provided by PlatformIO's SCons environment

PROJECT_DIR = Path(env["PROJECT_DIR"])  # noqa: F821
MANIFEST_PATH = PROJECT_DIR / "data" / "icons_manifest.json"
OUTPUT_DIR = PROJECT_DIR / "src" / "generated"
DEFAULT_SRC_DIR = Path("data/icons_src/icons8")
# Deliberately under .pio/ and NOT under data/: `pio run -t uploadfs` builds the
# LittleFS image from data/, so anything left there gets flashed to the device.

DOWNLOAD_TIMEOUT_S = 20
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"

# https://icons8.com/icon/17950/google-logo -> id 17950
ICONS8_PAGE_RE = re.compile(r"icons8\.com/icon/(\d+)")
# A bare icon name as it appears in an img.icons8.com path, e.g. "google".
ICONS8_NAME_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")


def fail(message: str):
    print(f"build_icons.py: {message}", file=sys.stderr)
    sys.exit(1)


def find_lvgl_image_converter() -> Path:
    candidates = list(PROJECT_DIR.glob(".pio/libdeps/*/lvgl/scripts/LVGLImage.py"))
    if not candidates:
        fail(
            "LVGLImage.py not found yet under .pio/libdeps — run `pio run` once "
            "to fetch the lvgl library, then re-run."
        )
    return candidates[0]


def check_converter_deps():
    """LVGLImage.py imports pypng and lz4 at module scope and raises a bare
    ImportError if either is absent. They are not PlatformIO dependencies, so
    on a fresh checkout they must be installed into PlatformIO's own Python
    (sys.executable here) — catch that up front with an actionable message
    instead of surfacing the converter's traceback."""
    missing = [
        pkg
        for pkg, mod in (("pypng", "png"), ("lz4", "lz4.block"))
        if subprocess.run(
            [sys.executable, "-c", f"import {mod}"], capture_output=True
        ).returncode
        != 0
    ]
    if missing:
        fail(
            f"LVGLImage.py needs {' and '.join(missing)} — install into "
            f"PlatformIO's Python with:\n"
            f"    {sys.executable} -m pip install {' '.join(missing)}"
        )


def load_manifest() -> dict:
    if not MANIFEST_PATH.exists():
        fail(f"manifest not found at {MANIFEST_PATH}")
    try:
        return json.loads(MANIFEST_PATH.read_text())
    except json.JSONDecodeError as exc:
        fail(f"manifest is not valid JSON ({MANIFEST_PATH}): {exc}")


def normalise_hex_color(value, where: str) -> str:
    color = str(value).strip().lstrip("#").upper()
    if not re.fullmatch(r"[0-9A-F]{6}", color):
        fail(f'{where} must be a 6-digit hex colour like "FFFFFF", got: {value!r}')
    return color


def icon_color(icon: dict, default: str, icon_id: str) -> str:
    """Per-icon `color` overrides the manifest-wide `iconColor`."""
    if "color" in icon:
        return normalise_hex_color(icon["color"], f"icon '{icon_id}' \"color\"")
    return default


def resolve_download_url(icon: dict, style: str, size: int, color: str) -> str:
    """Map a manifest `source` onto an img.icons8.com PNG URL.

    Two accepted forms:
      * an Icons8 page URL — "https://icons8.com/icon/17950/google-logo".
        The numeric ID is what identifies the icon; the trailing slug is
        decorative. Tinted via the `color` query parameter.
      * a bare name — "google". Fetched by name within the manifest's `style`,
        tinted via a colour path segment.

    Both forms return DIFFERENT artwork for the same word (id 17950 and
    ios-filled/"google" are distinct glyph designs), and — this is the part
    that is easy to get wrong — **both default to solid black**. The Icons8
    "logo" catalogue entries are monochrome silhouettes, not the brand's
    colour marks. Against this project's dark theme an untinted icon is a
    black shape on a near-black screen, so tinting applies to BOTH forms.

    Per-icon `preserveColors: true` opts out, for the genuinely multi-colour
    artwork where flattening to one colour would destroy the icon.
    """
    icon_id = icon["id"]
    source = str(icon.get("source", "")).strip()
    if not source:
        fail(f"icon '{icon_id}' has no \"source\" (an Icons8 page URL or a bare name)")

    tint = None if icon.get("preserveColors") else icon_color(icon, color, icon_id)

    if source.startswith("http://") or source.startswith("https://"):
        match = ICONS8_PAGE_RE.search(source)
        if not match:
            fail(
                f"icon '{icon_id}': source looks like a URL but not an Icons8 icon "
                f"page — expected .../icons8.com/icon/<numeric-id>/<slug>, got:\n"
                f"    {source}"
            )
        url = f"https://img.icons8.com/?id={match.group(1)}&format=png&size={size}"
        return url if tint is None else f"{url}&color={tint}"

    if not ICONS8_NAME_RE.match(source):
        fail(
            f"icon '{icon_id}': source '{source}' is neither a URL nor a valid "
            "icon name (lowercase letters, digits and hyphens only)"
        )
    if tint is None:
        return f"https://img.icons8.com/{style}/{size}/{source}.png"
    return f"https://img.icons8.com/{style}/{size}/{tint}/{source}.png"


def download_png(url: str, dest: Path, icon_id: str):
    request = urllib.request.Request(
        url,
        # img.icons8.com serves an error page rather than an image to clients it
        # doesn't recognise, so identify as an ordinary browser.
        headers={"User-Agent": "Mozilla/5.0 (compatible; esp32-c6-control build)"},
    )
    try:
        with urllib.request.urlopen(request, timeout=DOWNLOAD_TIMEOUT_S) as response:
            payload = response.read()
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        # No "fall back to the existing file" branch here on purpose: the caller
        # only reaches this function when the file is absent, so there is
        # nothing to fall back to.
        fail(
            f"could not download icon '{icon_id}', and no file exists at "
            f"{dest.relative_to(PROJECT_DIR)}.\n"
            f"    url:   {url}\n"
            f"    error: {exc}\n"
            f"    Put a PNG at that path to build offline — an existing file is "
            "always used as-is."
        )

    # A 200 response is not proof of a PNG — Icons8 answers unknown names with
    # an HTML page, which would otherwise land on disk and fail much later
    # inside the converter with a far less obvious message.
    if not payload.startswith(PNG_MAGIC):
        fail(
            f"icon '{icon_id}': {url} did not return a PNG "
            f"({len(payload)} bytes, starts with {payload[:16]!r}).\n"
            "    Check the icon name/ID exists, and that the style and size are "
            "valid for it."
        )

    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(payload)
    print(
        f"build_icons.py: downloaded {icon_id} -> "
        f"{dest.relative_to(PROJECT_DIR)} ({len(payload)} bytes)"
    )


def ensure_source_png(icon: dict, style: str, size: int, color: str) -> Path:
    """The file on disk wins. If it exists, it is used as-is and never
    re-downloaded or overwritten; if it doesn't, it is fetched.

    That single rule replaces what used to be three overlapping mechanisms (a
    resolved-URL cache index, a `local: true` opt-out, and a force-download env
    var). Deleting the PNG is the one way to re-fetch it, which also means a
    hand-placed or hand-edited file is safe by default rather than by opt-in.

    The tradeoff, worth knowing before it surprises you: changing an appearance
    setting in the manifest — `iconColor`, a per-icon `color`,
    `preserveColors`, `style`, `sourceExportSize` — has NO effect on an icon
    whose PNG already exists. Delete the file to pick the change up. The
    "using existing" log line below is the reminder.
    """
    icon_id = icon["id"]
    rel = Path(icon.get("file") or (DEFAULT_SRC_DIR / f"{icon_id}.png"))
    dest = PROJECT_DIR / rel

    if dest.exists():
        print(f"build_icons.py: using existing {rel} (delete it to re-download)")
        return dest

    download_png(resolve_download_url(icon, style, size, color), dest, icon_id)
    if not dest.exists():
        fail(f"icon '{icon_id}': expected a source PNG at {rel} after download")
    return dest


def convert_icon(converter: Path, icon_id: str, src: Path):
    print(f"build_icons.py: converting {icon_id} ({src.name})")
    # RGB565 + 8-bit alpha plane, C-array output, one file per image.
    cmd = [
        sys.executable,
        str(converter),
        "--ofmt", "C",
        "--cf", "RGB565A8",
        "-o", str(OUTPUT_DIR),
        # Force the emitted C symbol to match the manifest id, rather than
        # letting it fall out of the source filename — the generated registry
        # below refers to icons by manifest id, so this keeps the two in step
        # even if a `file` path is renamed.
        "--name", icon_id,
        str(src),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr, file=sys.stderr)
        fail(f"conversion failed for '{icon_id}'")


def c_string_literal(value: str) -> str:
    """Escape a manifest string for embedding in generated C.

    Deliberately rejects rather than mangles anything outside printable
    ASCII: the About screen renders with LVGL's built-in Montserrat font,
    whose glyph set does not cover arbitrary Unicode, so a stray em dash
    would compile fine and then render as a blank box on the device.
    """
    if any(ord(ch) < 0x20 or ord(ch) > 0x7E for ch in value):
        offenders = sorted({ch for ch in value if ord(ch) < 0x20 or ord(ch) > 0x7E})
        fail(
            f'manifest "attribution" must be printable ASCII (it is rendered '
            f"with LVGL's built-in font, which has no glyphs for the rest).\n"
            f"    offending characters: {offenders}\n"
            f"    value: {value}"
        )
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_registry(icon_ids: list, welcome_icon: str, attribution: str):
    """Generated lookup table: icon ID string -> lv_image_dsc_t symbol, plus
    the welcome screen's icon. main.cpp refers to kWelcomeIcon rather than to
    any specific symbol, so changing the welcome icon is a manifest edit and
    needs no code change."""
    header_path = OUTPUT_DIR / "icon_registry_gen.h"
    lines = [
        "// Generated by scripts/build_icons.py — do not edit by hand.",
        "#pragma once",
        '#include "lvgl.h"',
        "",
    ]
    lines += [
        "// Icons8's free tier requires a visible credit wherever the icons are",
        "// used (ARCHITECTURE.md §12.1). This comes from the manifest's",
        '// "attribution" so the string the device shows and the string recorded',
        "// alongside the licence cannot drift apart. Rendered by the About",
        "// screen — see showAboutScreen() in main.cpp.",
        f"static const char *const kIconAttribution = {c_string_literal(attribution)};",
        "",
    ]
    lines += [f"extern const lv_image_dsc_t {icon_id};" for icon_id in icon_ids]
    lines += [
        "",
        "struct IconRegistryEntry { const char *id; const lv_image_dsc_t *img; };",
        "static const IconRegistryEntry kIconRegistry[] = {",
    ]
    lines += [f'    {{"{icon_id}", &{icon_id}}},' for icon_id in icon_ids]
    lines += [
        "};",
        f"static const size_t kIconRegistryCount = {len(icon_ids)};",
        "",
        '// Selected by icons_manifest.json\'s "welcomeIcon".',
        f"static const lv_image_dsc_t *const kWelcomeIcon = &{welcome_icon};",
    ]
    header_path.write_text("\n".join(lines) + "\n")
    print(f"build_icons.py: wrote {header_path.relative_to(PROJECT_DIR)}")


def main():
    manifest = load_manifest()
    icons = manifest.get("icons", [])
    if not icons:
        fail("manifest has no icons — at least the welcome icon is required")

    style = manifest.get("style")
    if not style:
        fail('manifest has no "style" (e.g. "ios-filled")')

    size = manifest.get("sourceExportSize")
    if not isinstance(size, int) or size <= 0:
        fail('manifest "sourceExportSize" must be a positive integer')

    # Defaults to white because the UI is a permanent dark theme
    # (LV_THEME_DEFAULT_DARK in include/lv_conf.h). Only affects the name form
    # of `source` — see resolve_download_url().
    color = normalise_hex_color(manifest.get("iconColor", "FFFFFF"), 'manifest "iconColor"')

    icon_ids = [icon["id"] for icon in icons]
    duplicates = {i for i in icon_ids if icon_ids.count(i) > 1}
    if duplicates:
        fail(f"manifest has duplicate icon ids: {', '.join(sorted(duplicates))}")

    # Required, not optional: ARCHITECTURE.md §12.1 chose Icons8's free tier,
    # whose licence is conditional on a visible credit. Letting this default to
    # empty would let a build silently ship non-compliant.
    attribution = str(manifest.get("attribution", "")).strip()
    if not attribution:
        fail(
            'manifest has no "attribution" — Icons8\'s free tier (ARCHITECTURE.md '
            "§12.1) requires a visible credit, which the About screen renders from "
            "this field."
        )

    welcome_icon = manifest.get("welcomeIcon")
    if not welcome_icon:
        fail('manifest has no "welcomeIcon" naming which icon the welcome screen shows')
    if welcome_icon not in icon_ids:
        fail(
            f'manifest "welcomeIcon" is \'{welcome_icon}\', which is not one of the '
            f"defined icons ({', '.join(icon_ids)})"
        )

    converter = find_lvgl_image_converter()
    check_converter_deps()
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    for icon in icons:
        # Per-icon "size" overrides sourceExportSize — for small inline icons
        # (e.g. the Volume page's 28px target icons) that would otherwise be
        # scaled down from the big menu size at runtime and lose crispness.
        icon_size = icon.get("size", size)
        if not isinstance(icon_size, int) or icon_size <= 0:
            fail(f"icon '{icon['id']}': \"size\" must be a positive integer")
        src = ensure_source_png(icon, style, icon_size, color)
        convert_icon(converter, icon["id"], src)

    write_registry(icon_ids, welcome_icon, attribution)


main()
