#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PlatformIO pre-build hook to ensure dashboard_html_gz.h is always up to date.
Invoked automatically before compiling if configured in platformio.ini:
  extra_scripts = pre:scripts/pre_build_compress.py
"""

import os
import sys

# PlatformIO SCons does not provide __file__; use env or getcwd
try:
    Import("env")
    project_dir = env.get("PROJECT_DIR", os.getcwd())
except Exception:
    project_dir = os.getcwd()

scripts_dir = os.path.join(project_dir, "scripts")
if scripts_dir not in sys.path:
    sys.path.insert(0, scripts_dir)

try:
    import compress_dashboard

    main_cpp = os.path.join(project_dir, "src", "main.cpp")
    gz_header = os.path.join(project_dir, "include", "dashboard_html_gz.h")
    needs_update = False

    if not os.path.isfile(gz_header):
        needs_update = True
    else:
        # Check if header is in sync with main.cpp
        is_synced = compress_dashboard.check_sync(main_cpp, gz_header, verbose=False)
        if not is_synced:
            needs_update = True

    if needs_update:
        print("\n[PRE-BUILD] DASHBOARD_HTML in src/main.cpp changed or GZIP header missing.")
        print("[PRE-BUILD] Auto-regenerating include/dashboard_html_gz.h...")
        raw_html = compress_dashboard.extract_html_from_main(main_cpp)
        raw_sz, gz_sz = compress_dashboard.update_gz_header(raw_html, gz_header, verbose=False)
        print(f"[PRE-BUILD] Successfully updated {gz_header}: {raw_sz:,} raw -> {gz_sz:,} gz bytes ({gz_sz * 100.0 / raw_sz:.1f}%)\n")
    else:
        print("[PRE-BUILD] include/dashboard_html_gz.h is verified and in sync.")

except Exception as err:
    print(f"[PRE-BUILD WARNING] Could not auto-sync GZIP header: {err}")
    # Don't fail the build, src/main.cpp has #if __has_include fallback to raw HTML
