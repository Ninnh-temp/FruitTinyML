#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
==============================================================================
 SMART SCALE POS — HYPRLAND / CATPPUCCIN LINUX RICE EDITION
==============================================================================
 Architecture:
   - Local Python Backend (`ThreadingHTTPServer` on http://127.0.0.1:8765)
   - Native Desktop App Window (`pywebview` if installed, or automatic
     frameless `msedge --app` / `chrome --app` native OS window mode)
   - Full CSS Spring Physics, 3D Mechanical Keycap Buttons, CRT Scanlines,
     Hyprland Tiling Borders (`╭─ ~/dev/ov2640`, `╭─ ~/pos/active-order`,
     `╭─ ~/pos/history-csv`), Reactive Fruit Glow, Mocha/Latte Theme Toggle
   - Order-Based POS Workflow (`Đơn #1` -> `Đơn #2` -> `Đơn #N`)
   - Direct PC Disk I/O to `data/pos_orders_history.csv` (`utf-8-sig`) and
     `data/pos_state.json` (never creates duplicate browser `(1).csv` files)
==============================================================================
"""

import os
import sys
import csv
import json
import time
import shutil
import socket
import argparse
import threading
import subprocess
import webbrowser
import urllib.request
import urllib.error
import urllib.parse
import http.client
from datetime import datetime
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn

if hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

# ----------------------------------------------------------------------------
# 1. PyInstaller-Safe Path Resolution & Dual-Directory Sync
# ----------------------------------------------------------------------------
if getattr(sys, "frozen", False):
    BASE_DIR = os.path.dirname(sys.executable)
else:
    BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DATA_DIR = os.path.join(BASE_DIR, "data")
os.makedirs(DATA_DIR, exist_ok=True)

CSV_FILE = os.path.join(DATA_DIR, "pos_orders_history.csv")
STATE_FILE = os.path.join(DATA_DIR, "pos_state.json")

# If running from workspace root vs FruitTinyML subfolder, keep both data/ folders synced
SECONDARY_DATA_DIR = None
if os.path.basename(BASE_DIR).lower() == "fruittinyml":
    parent_dir = os.path.dirname(BASE_DIR)
    if os.path.exists(parent_dir):
        SECONDARY_DATA_DIR = os.path.join(parent_dir, "data")
elif os.path.isdir(os.path.join(BASE_DIR, "FruitTinyML")):
    SECONDARY_DATA_DIR = os.path.join(BASE_DIR, "FruitTinyML", "data")

if SECONDARY_DATA_DIR:
    try:
        os.makedirs(SECONDARY_DATA_DIR, exist_ok=True)
    except Exception:
        SECONDARY_DATA_DIR = None

CSV_HEADER = [
    "Order_ID",
    "Order_No",
    "Timestamp",
    "Item_Count",
    "Items_Summary",
    "Total_Weight_g",
    "Total_Amount_VND",
]

DEFAULT_PRICES = {
    "apple": 60000,
    "orange": 40000,
    "pear": 70000,
}

FRUIT_LABELS_VI = {
    "apple": "Táo",
    "orange": "Cam",
    "pear": "Lê",
    "none": "Trống",
}


def _safe_float(val, default=0.0):
    if val is None or isinstance(val, bool):
        return default
    try:
        f = float(val)
        if f != f or f == float("inf") or f == float("-inf"):
            return default
        return f
    except (ValueError, TypeError):
        return default


def _safe_int(val, default=0):
    if val is None or isinstance(val, bool):
        return default
    try:
        return int(float(val))
    except (ValueError, TypeError):
        return default


class POSStorageManager:
    def __init__(self):
        self.lock = threading.Lock()
        self.next_order_no = 1
        self.prices = dict(DEFAULT_PRICES)
        self.esp_ip = "192.168.4.1"
        self.auto_add = True
        self.theme = "night"
        self.history = []
        self.csv_pending_sync = False
        self._cached_pending_history = None

        self.load_state()
        self.load_history()

    def _mirror_files_to_secondary(self):
        if not SECONDARY_DATA_DIR:
            return
        try:
            os.makedirs(SECONDARY_DATA_DIR, exist_ok=True)
            if os.path.exists(STATE_FILE):
                sec_state = os.path.join(SECONDARY_DATA_DIR, "pos_state.json")
                if os.path.abspath(STATE_FILE) != os.path.abspath(sec_state):
                    shutil.copy2(STATE_FILE, sec_state)
            if os.path.exists(CSV_FILE) and os.path.getsize(CSV_FILE) > 0:
                sec_csv = os.path.join(SECONDARY_DATA_DIR, "pos_orders_history.csv")
                if os.path.abspath(CSV_FILE) != os.path.abspath(sec_csv):
                    shutil.copy2(CSV_FILE, sec_csv)
        except Exception as e:
            print(f"[WARN] Secondary data sync warning: {e}")

    def load_state(self):
        with self.lock:
            target_state = STATE_FILE
            if not os.path.exists(target_state) and SECONDARY_DATA_DIR:
                alt = os.path.join(SECONDARY_DATA_DIR, "pos_state.json")
                if os.path.exists(alt):
                    target_state = alt

            if os.path.exists(target_state):
                try:
                    with open(target_state, "r", encoding="utf-8") as f:
                        data = json.load(f)
                    if isinstance(data, dict):
                        self.next_order_no = max(1, _safe_int(data.get("next_order_no", 1), 1))
                        loaded_prices = data.get("prices", {})
                        if isinstance(loaded_prices, dict):
                            for k in ("apple", "orange", "pear"):
                                if k in loaded_prices:
                                    p_val = _safe_float(loaded_prices[k], -1.0)
                                    if p_val >= 0:
                                        self.prices[k] = p_val
                        self.esp_ip = str(data.get("esp_ip", "192.168.4.1")).strip() or "192.168.4.1"
                        self.auto_add = bool(data.get("auto_add", True))
                        self.theme = str(data.get("theme", "night"))
                        self.csv_pending_sync = bool(data.get("csv_pending_sync", False))
                        if self.csv_pending_sync and isinstance(data.get("pending_history"), list):
                            self._cached_pending_history = data.get("pending_history")
                except Exception as e:
                    print(f"[WARN] Could not read {target_state}: {e}")
            self._save_state_unlocked()

    def _save_state_unlocked(self):
        data = {
            "next_order_no": int(self.next_order_no),
            "prices": {
                "apple": float(self.prices.get("apple", 60000)),
                "orange": float(self.prices.get("orange", 40000)),
                "pear": float(self.prices.get("pear", 70000)),
            },
            "esp_ip": self.esp_ip,
            "auto_add": bool(self.auto_add),
            "theme": self.theme,
            "csv_pending_sync": bool(self.csv_pending_sync),
            "last_saved": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        }
        if self.csv_pending_sync:
            data["pending_history"] = list(self.history)
        try:
            with open(STATE_FILE, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2, ensure_ascii=False)
            self._mirror_files_to_secondary()
        except Exception as e:
            print(f"[ERR] Failed saving state to {STATE_FILE}: {e}")

    def save_state(self):
        with self.lock:
            self._save_state_unlocked()

    def load_history(self):
        with self.lock:
            self.history = []
            loaded_from_secondary = False
            if self.csv_pending_sync and isinstance(self._cached_pending_history, list):
                self.history = list(self._cached_pending_history)
                self._cached_pending_history = None
                self._flush_pending_csv_unlocked()
            else:
                target_csv = CSV_FILE
                if (not os.path.exists(target_csv) or os.path.getsize(target_csv) == 0) and SECONDARY_DATA_DIR:
                    alt_csv = os.path.join(SECONDARY_DATA_DIR, "pos_orders_history.csv")
                    if os.path.exists(alt_csv) and os.path.getsize(alt_csv) > 0:
                        target_csv = alt_csv
                        loaded_from_secondary = True

                if os.path.exists(target_csv) and os.path.getsize(target_csv) > 0:
                    try:
                        with open(target_csv, "r", encoding="utf-8-sig", newline="") as f:
                            reader = csv.DictReader(f)
                            for raw_row in reader:
                                if not isinstance(raw_row, dict):
                                    continue
                                row = {
                                    (k.lstrip("\ufeff").strip() if k else ""): (
                                        v.lstrip("\ufeff").strip() if isinstance(v, str) else v
                                    )
                                    for k, v in raw_row.items()
                                }
                                order_id = str(row.get("Order_ID", "") or "").strip()
                                if not order_id or order_id == "Order_ID":
                                    continue
                                raw_no = row.get("Order_No", "")
                                if str(raw_no).isdigit():
                                    order_no = int(raw_no)
                                else:
                                    digits = "".join(ch for ch in order_id if ch.isdigit())
                                    order_no = int(digits) if digits else (len(self.history) + 1)

                                item_count = max(1, _safe_int(row.get("Item_Count", 1), 1))
                                total_weight_g = _safe_float(row.get("Total_Weight_g", 0.0), 0.0)
                                total_amount_vnd = _safe_float(row.get("Total_Amount_VND", 0.0), 0.0)

                                self.history.append({
                                    "order_id": order_id,
                                    "order_no": order_no,
                                    "timestamp": str(row.get("Timestamp", "") or ""),
                                    "item_count": item_count,
                                    "items_summary": str(row.get("Items_Summary", "") or ""),
                                    "total_weight_g": round(total_weight_g, 1),
                                    "total_amount_vnd": round(total_amount_vnd),
                                })
                    except Exception as e:
                        print(f"[WARN] Error loading CSV history from {target_csv}: {e}")

                # If history was loaded from secondary directory while primary CSV_FILE was missing, sync primary immediately
                if loaded_from_secondary and self.history:
                    self._rewrite_csv_unlocked()

            # Ensure next_order_no is at least max(existing order_no) + 1 if history has items
            if self.history:
                max_no = max(_safe_int(o.get("order_no", 0), 0) for o in self.history)
                if self.next_order_no <= max_no:
                    self.next_order_no = max_no + 1
                    self._save_state_unlocked()

    def _rewrite_csv_unlocked(self):
        """Rewrites data/pos_orders_history.csv with UTF-8-SIG BOM and current self.history."""
        try:
            with open(CSV_FILE, mode="w", encoding="utf-8-sig", newline="") as f:
                writer = csv.writer(f)
                writer.writerow(CSV_HEADER)
                for order in self.history:
                    writer.writerow([
                        order["order_id"],
                        order["order_no"],
                        order["timestamp"],
                        order["item_count"],
                        order["items_summary"],
                        f"{_safe_float(order.get('total_weight_g', 0.0)):.1f}",
                        f"{round(_safe_float(order.get('total_amount_vnd', 0.0)))}",
                    ])
            self.csv_pending_sync = False
            self._mirror_files_to_secondary()
            return True, None
        except (PermissionError, OSError) as e:
            self.csv_pending_sync = True
            self._save_state_unlocked()
            return False, str(e)

    def _flush_pending_csv_unlocked(self):
        if not self.csv_pending_sync:
            return True
        ok, _ = self._rewrite_csv_unlocked()
        if ok:
            self._save_state_unlocked()
        return ok

    def checkout_order(self, payload):
        """
        Finalizes the active order:
          1. Packages all items in Đơn #<N> into ORD-0001, timestamp, item_count,
             items_summary (e.g. "Táo (215g), Cam (180g)"), total_weight_g, total_amount_vnd.
          2. Appends directly to data/pos_orders_history.csv (mode="a", encoding="utf-8-sig",
             writing header only once if file doesn't exist).
          3. Advances next_order_no to N+1 and saves data/pos_state.json.
        """
        if not isinstance(payload, dict):
            return {"ok": False, "error": "Payload không hợp lệ."}

        with self.lock:
            items = payload.get("items", [])
            order_no = int(self.next_order_no)
            order_id = f"ORD-{order_no:04d}"
            timestamp = str(payload.get("timestamp") or datetime.now().strftime("%Y-%m-%d %H:%M:%S"))

            if items and isinstance(items, list):
                valid_items = [it for it in items if isinstance(it, dict)]
                if not valid_items:
                    return {"ok": False, "error": "Danh sách mặt hàng không hợp lệ."}
                item_count = len(valid_items)
                total_weight_g = round(
                    sum(max(0.0, _safe_float(it.get("weight_g", it.get("weightG", 0.0)), 0.0)) for it in valid_items),
                    1,
                )
                total_amount_vnd = round(
                    sum(max(0.0, _safe_float(it.get("total_vnd", it.get("totalVND", 0.0)), 0.0)) for it in valid_items)
                )
                summary_parts = []
                for it in valid_items:
                    label_key = str(it.get("label") or "").lower().strip()
                    short_name = str(
                        FRUIT_LABELS_VI.get(label_key) or it.get("short_name") or it.get("name") or "Món"
                    )
                    if "(" in short_name:
                        short_name = short_name.split("(")[0].strip()
                    w_val = max(0.0, _safe_float(it.get("weight_g", it.get("weightG", 0.0)), 0.0))
                    w_str = f"{w_val:.0f}g" if abs(w_val - round(w_val)) < 0.05 else f"{w_val:.1f}g"
                    summary_parts.append(f"{short_name} ({w_str})")
                items_summary = ", ".join(summary_parts)
            else:
                item_count = _safe_int(payload.get("item_count", 1), 0)
                total_weight_g = round(_safe_float(payload.get("total_weight_g", 0.0), 0.0), 1)
                total_amount_vnd = round(_safe_float(payload.get("total_amount_vnd", 0.0), 0.0))
                items_summary = str(payload.get("items_summary") or "").strip()

            if item_count <= 0 or total_weight_g <= 0:
                return {"ok": False, "error": "Giỏ hàng trống hoặc khối lượng bằng 0g."}

            order_record = {
                "order_id": order_id,
                "order_no": order_no,
                "timestamp": timestamp,
                "item_count": item_count,
                "items_summary": items_summary,
                "total_weight_g": total_weight_g,
                "total_amount_vnd": total_amount_vnd,
            }

            csv_warning = None
            file_exists = os.path.exists(CSV_FILE) and os.path.getsize(CSV_FILE) > 0

            # If CSV file was missing when history already had items, or pending sync was active, rewrite full history + new order
            if self.csv_pending_sync or (not file_exists and len(self.history) > 0):
                self.history.append(order_record)
                ok_csv, err_msg = self._rewrite_csv_unlocked()
                if not ok_csv:
                    csv_warning = f"File CSV đang mở trong ứng dụng khác ({err_msg}). Đơn đã lưu tạm và sẽ tự đồng bộ."
            else:
                try:
                    has_header = False
                    needs_leading_newline = False
                    if file_exists:
                        with open(CSV_FILE, "rb") as rf:
                            raw_bytes = rf.read()
                        has_header = b"Order_ID" in raw_bytes
                        if raw_bytes and not raw_bytes.endswith((b"\n", b"\r")):
                            needs_leading_newline = True

                    with open(CSV_FILE, mode="a", encoding="utf-8-sig", newline="") as f:
                        if needs_leading_newline:
                            f.write("\r\n")
                        writer = csv.writer(f)
                        if not file_exists or not has_header:
                            writer.writerow(CSV_HEADER)
                        writer.writerow([
                            order_record["order_id"],
                            order_record["order_no"],
                            order_record["timestamp"],
                            order_record["item_count"],
                            order_record["items_summary"],
                            f"{order_record['total_weight_g']:.1f}",
                            f"{order_record['total_amount_vnd']}",
                        ])
                    self.history.append(order_record)
                    self.csv_pending_sync = False
                except (PermissionError, OSError) as e:
                    self.history.append(order_record)
                    self.csv_pending_sync = True
                    csv_warning = f"File CSV đang khóa bởi Excel ({e}). Đơn đã được giữ an toàn và sẽ đồng bộ ngay khi đóng Excel."

            self.next_order_no = order_no + 1
            self._save_state_unlocked()
            self._mirror_files_to_secondary()

            resp = {
                "ok": True,
                "order": order_record,
                "state": self._get_state_dict_unlocked(),
            }
            if csv_warning:
                resp["csv_warning"] = csv_warning
            return resp

    def delete_order(self, order_id=None, order_no=None):
        """Deletes an order by order_id or order_no and immediately rewrites CSV on disk."""
        with self.lock:
            before_len = len(self.history)
            target_id = str(order_id or "").strip()
            target_no = int(order_no) if (order_no is not None and str(order_no).isdigit()) else None

            self.history = [
                o for o in self.history
                if not (
                    (target_id and o.get("order_id") == target_id)
                    or (target_no is not None and o.get("order_no") == target_no)
                )
            ]
            deleted = len(self.history) < before_len
            ok_csv, err_msg = self._rewrite_csv_unlocked()
            self._save_state_unlocked()
            resp = {
                "ok": True,
                "deleted": deleted,
                "state": self._get_state_dict_unlocked(),
            }
            if not ok_csv:
                resp["csv_warning"] = err_msg
            return resp

    def clear_history(self, reset_order_no=True):
        """Clears all finalized orders, rewrites CSV on disk, and resets to Đơn #1."""
        with self.lock:
            self.history = []
            if reset_order_no:
                self.next_order_no = 1
            ok_csv, err_msg = self._rewrite_csv_unlocked()
            self._save_state_unlocked()
            resp = {
                "ok": True,
                "state": self._get_state_dict_unlocked(),
            }
            if not ok_csv:
                resp["csv_warning"] = err_msg
            return resp

    def update_settings(self, payload):
        if not isinstance(payload, dict):
            return {"ok": False, "error": "Invalid settings payload"}
        with self.lock:
            if "prices" in payload and isinstance(payload["prices"], dict):
                for k in ("apple", "orange", "pear"):
                    if k in payload["prices"]:
                        val = _safe_float(payload["prices"][k], -1.0)
                        if val >= 0:
                            self.prices[k] = val
            if "esp_ip" in payload:
                ip_val = str(payload["esp_ip"] or "").strip()
                if ip_val and ip_val != self.esp_ip:
                    self.esp_ip = ip_val
                    if "ESP32_BRIDGE" in globals():
                        ESP32_BRIDGE.reset_target()
            if "auto_add" in payload:
                self.auto_add = bool(payload["auto_add"])
            if "theme" in payload and payload["theme"] in ("night", "day"):
                self.theme = payload["theme"]
            if "next_order_no" in payload:
                self.next_order_no = max(1, _safe_int(payload["next_order_no"], self.next_order_no))
            self._flush_pending_csv_unlocked()
            self._save_state_unlocked()
            return {
                "ok": True,
                "state": self._get_state_dict_unlocked(),
            }

    def open_csv_on_pc(self):
        with self.lock:
            self._flush_pending_csv_unlocked()
            if not os.path.exists(CSV_FILE) or os.path.getsize(CSV_FILE) == 0:
                self._rewrite_csv_unlocked()
        try:
            if sys.platform == "win32":
                os.startfile(CSV_FILE)
            elif sys.platform == "darwin":
                subprocess.Popen(["open", CSV_FILE])
            else:
                subprocess.Popen(["xdg-open", CSV_FILE])
            return {"ok": True, "csv_file": CSV_FILE}
        except Exception as e:
            return {"ok": False, "error": str(e), "csv_file": CSV_FILE}

    def _get_state_dict_unlocked(self):
        if self.csv_pending_sync:
            self._flush_pending_csv_unlocked()
        history_newest_first = list(reversed(self.history))
        grand_total_vnd = round(sum(_safe_float(o.get("total_amount_vnd", 0)) for o in self.history))
        total_weight_g = round(sum(_safe_float(o.get("total_weight_g", 0.0)) for o in self.history), 1)
        return {
            "ok": True,
            "next_order_no": self.next_order_no,
            "active_order_label": f"Đơn #{self.next_order_no}",
            "active_order_code": f"ORD-{self.next_order_no:04d}",
            "prices": dict(self.prices),
            "esp_ip": self.esp_ip,
            "auto_add": self.auto_add,
            "theme": self.theme,
            "csv_file": CSV_FILE,
            "state_file": STATE_FILE,
            "csv_pending_sync": self.csv_pending_sync,
            "history": history_newest_first,
            "total_orders": len(self.history),
            "grand_total_vnd": grand_total_vnd,
            "grand_total_weight_g": total_weight_g,
        }

    def get_state_dict(self):
        with self.lock:
            return self._get_state_dict_unlocked()


STORAGE = POSStorageManager()


# ----------------------------------------------------------------------------
# 3. Hyprland / Catppuccin Linux Rice POS HTML/CSS/JS Frontend
# ----------------------------------------------------------------------------
POS_RICE_HTML = r"""<!DOCTYPE html>
<html lang="vi">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Smart Scale POS — Hyprland Rice Edition</title>
  <style>
    /* ==========================================================================
       Catppuccin Theme Engine (Mocha 🌙 Night vs. Latte ☀️ Daylight)
       ========================================================================== */
    :root {
      /* Catppuccin Mocha (Night Default) */
      --bg: #11111b;            /* Crust */
      --mantle: #181825;        /* Mantle */
      --panel: #1e1e2e;         /* Base */
      --panel-tint: #25273a;
      --panel-surface: #313244; /* Surface0 */
      --border-color: rgba(137, 180, 250, 0.32);
      --text: #cdd6f4;          /* Text */
      --text-muted: #a6adc8;    /* Subtext0 */
      --text-dim: #6c7086;      /* Overlay0 */
      --key-shadow: #09090f;
      --dot-color: rgba(108, 112, 134, 0.24);

      /* Dynamic Reactive Fruit Glow Accents */
      --accent: #89b4fa;        /* Sky Blue (Standby / none) */
      --accent-glow: rgba(137, 180, 250, 0.24);
      --green: #a6e3a1;         /* Emerald (Pear / Success) */
      --green-glow: rgba(166, 227, 161, 0.24);
      --amber: #f9e2af;         /* Yellow */
      --peach: #fab387;         /* Tangerine (Orange) */
      --red: #f38ba8;           /* Crimson (Apple / Danger) */
      --mauve: #cba6f7;

      --font-mono: "JetBrains Mono", "Fira Code", "Cascadia Mono", Consolas, "Segoe UI Mono", monospace;
      --ease-spring: cubic-bezier(0.34, 1.56, 0.64, 1);
    }

    [data-theme="day"] {
      /* Catppuccin Latte (Daylight Dot-Grid) */
      --bg: #dce0e8;            /* Crust dot-grid bg */
      --mantle: #e6e9ef;        /* Mantle */
      --panel: #eff1f5;         /* Base */
      --panel-tint: #e6e9ef;
      --panel-surface: #ccd0da; /* Surface0 */
      --border-color: rgba(30, 102, 245, 0.28);
      --text: #4c4f69;          /* Text */
      --text-muted: #5c5f77;    /* Subtext0 */
      --text-dim: #8c8fa1;      /* Overlay0 */
      --key-shadow: #bcc0cc;
      --dot-color: rgba(140, 143, 161, 0.34);

      --accent: #1e66f5;
      --accent-glow: rgba(30, 102, 245, 0.18);
      --green: #40a02b;
      --green-glow: rgba(64, 160, 43, 0.20);
      --amber: #df8e1d;
      --peach: #fe640b;
      --red: #d20f39;
      --mauve: #8839ef;
    }

    * { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }

    body {
      font-family: var(--font-mono);
      background-color: var(--bg);
      background-image: radial-gradient(var(--dot-color) 1.5px, transparent 1.5px);
      background-size: 18px 18px;
      color: var(--text);
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      align-items: center;
      padding: 12px 16px 20px;
      transition: background-color 0.35s ease, color 0.35s ease;
    }

    .cockpit {
      width: 100%;
      max-width: 1240px;
      display: flex;
      flex-direction: column;
      gap: 14px;
    }

    /* ==========================================================================
       Waybar Top Status Bar
       ========================================================================== */
    .header-bar {
      width: 100%;
      background: var(--panel);
      border: 2px solid var(--accent);
      border-radius: 14px;
      padding: 8px 14px;
      display: flex;
      align-items: center;
      justify-content: space-between;
      flex-wrap: wrap;
      gap: 10px;
      box-shadow: 0 0 18px var(--accent-glow), 0 6px 20px rgba(0, 0, 0, 0.28);
      transition: border-color 0.35s ease, box-shadow 0.35s ease, background 0.35s ease;
    }
    .brand-left {
      display: flex;
      align-items: center;
      gap: 10px;
      flex-wrap: wrap;
    }
    .ws-pills {
      display: inline-flex;
      gap: 4px;
      background: var(--mantle);
      padding: 3px;
      border-radius: 9px;
      border: 1px solid var(--border-color);
    }
    .ws-pill {
      font-size: 0.68rem;
      font-weight: 800;
      padding: 2px 7px;
      border-radius: 6px;
      color: var(--text-dim);
      user-select: none;
    }
    .ws-pill.active {
      background: var(--accent);
      color: #11111b;
    }
    .brand-title {
      font-size: 1.02rem;
      font-weight: 800;
      letter-spacing: -0.3px;
      display: flex;
      align-items: center;
      gap: 8px;
    }
    .brand-badge {
      font-size: 0.64rem;
      background: rgba(137, 180, 250, 0.14);
      color: var(--accent);
      padding: 2px 8px;
      border-radius: 8px;
      font-weight: 800;
      text-transform: uppercase;
      letter-spacing: 0.4px;
      border: 1px solid var(--accent);
    }
    .header-right {
      display: flex;
      align-items: center;
      gap: 8px;
      flex-wrap: wrap;
    }

    /* ESP32 IP Quick Input */
    .ip-pill {
      display: inline-flex;
      align-items: center;
      gap: 5px;
      background: var(--mantle);
      border: 1.5px solid var(--border-color);
      border-radius: 9px;
      padding: 4px 9px;
      font-size: 0.72rem;
      font-weight: 700;
      color: var(--text-muted);
    }
    .ip-input {
      background: transparent;
      border: none;
      color: var(--accent);
      font-family: var(--font-mono);
      font-size: 0.74rem;
      font-weight: 800;
      width: 102px;
      outline: none;
    }

    /* 3D Mechanical Keycap Buttons */
    .pill-btn {
      position: relative;
      display: inline-flex;
      align-items: center;
      gap: 6px;
      padding: 6px 12px;
      border-radius: 9px;
      font-size: 0.75rem;
      font-family: var(--font-mono);
      font-weight: 700;
      background: var(--panel-surface);
      border: 1.5px solid var(--border-color);
      border-bottom: 4px solid var(--key-shadow);
      color: var(--text);
      cursor: pointer;
      user-select: none;
      transform: translateY(0);
      box-shadow: 0 3px 6px rgba(0, 0, 0, 0.25);
      transition: transform 0.12s var(--ease-spring), border-bottom-width 0.12s ease, background 0.2s ease, border-color 0.2s ease, color 0.2s ease;
    }
    .pill-btn:hover {
      transform: translateY(-2px);
      border-bottom-width: 5px;
      border-color: var(--accent);
      color: var(--accent);
    }
    .pill-btn:active {
      transform: translateY(2px);
      border-bottom-width: 2px;
      box-shadow: 0 1px 2px rgba(0, 0, 0, 0.4);
    }
    .pill-btn.active {
      background: rgba(137, 180, 250, 0.16);
      border-color: var(--accent);
      color: var(--accent);
    }
    .pill-btn.danger:hover {
      border-color: var(--red);
      color: var(--red);
    }
    .pill-btn.success {
      background: var(--green);
      color: #11111b;
      border-color: var(--green);
      border-bottom: 4px solid var(--key-shadow);
      font-weight: 800;
    }
    .pill-btn.success:hover {
      filter: brightness(1.08);
      color: #11111b;
      box-shadow: 0 6px 16px var(--green-glow);
    }

    .status-pill {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      padding: 6px 11px;
      border-radius: 9px;
      font-size: 0.74rem;
      font-weight: 700;
      background: var(--mantle);
      border: 1.5px solid var(--border-color);
      color: var(--text-muted);
    }
    .conn-dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
      background: var(--green);
      box-shadow: 0 0 8px var(--green);
      transition: all 0.3s ease;
    }
    .demo-toggle-label {
      cursor: pointer;
      display: inline-flex;
      align-items: center;
      gap: 6px;
      font-size: 0.75rem;
      font-weight: 700;
      color: var(--text-muted);
      user-select: none;
    }
    .demo-toggle-label input { cursor: pointer; accent-color: var(--accent); }

    /* ==========================================================================
       Hyprland Tiling Window Grid & Reactive Fruit Glow
       ========================================================================== */
    .cockpit-grid {
      display: grid;
      grid-template-columns: 1.08fr 1fr;
      gap: 16px;
      align-items: stretch;
    }
    @media (max-width: 900px) {
      .cockpit-grid { grid-template-columns: 1fr; }
    }

    .tiled-window {
      background: var(--panel);
      border: 2px solid var(--accent);
      border-radius: 14px;
      overflow: hidden;
      display: flex;
      flex-direction: column;
      box-shadow: 0 0 18px var(--accent-glow), 0 8px 24px rgba(0, 0, 0, 0.26);
      transition: transform 0.32s var(--ease-spring), border-color 0.35s ease, box-shadow 0.35s ease, background 0.35s ease;
    }
    .tiled-window:hover {
      transform: translateY(-2px);
    }

    /* Terminal Window Title Bar */
    .win-titlebar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 7px 12px;
      background: var(--mantle);
      border-bottom: 1.5px solid var(--border-color);
      font-size: 0.72rem;
      font-weight: 700;
      color: var(--text-muted);
      user-select: none;
    }
    .win-controls {
      display: flex;
      align-items: center;
      gap: 6px;
    }
    .win-dot {
      width: 9px;
      height: 9px;
      border-radius: 50%;
      display: inline-block;
    }
    .dot-close { background: var(--red); }
    .dot-min { background: var(--amber); }
    .dot-max { background: var(--green); }
    .win-term-path {
      color: var(--accent);
      font-weight: 800;
      margin-left: 4px;
      letter-spacing: 0.2px;
      transition: color 0.3s ease;
    }
    .win-title-text {
      letter-spacing: 0.3px;
      color: var(--text);
      font-weight: 700;
    }
    .win-tag {
      font-size: 0.65rem;
      background: var(--panel-surface);
      border: 1px solid var(--border-color);
      padding: 2px 7px;
      border-radius: 5px;
      color: var(--accent);
      font-weight: 800;
    }

    /* Left Column: Viewfinder, Action Keys, Simulator Bar & Telemetry */
    .monitor-col {
      display: flex;
      flex-direction: column;
      gap: 12px;
    }

    .viewfinder {
      position: relative;
      width: 100%;
      aspect-ratio: 4 / 3;
      background: #05070e;
      overflow: hidden;
      display: flex;
      align-items: center;
      justify-content: center;
      cursor: pointer;
    }
    #cam-feed {
      width: 100%;
      height: 100%;
      object-fit: contain;
      display: block;
    }

    /* CRT Scanlines */
    .crt-scanlines {
      position: absolute;
      inset: 0;
      background: linear-gradient(rgba(18, 16, 16, 0) 50%, rgba(0, 0, 0, 0.36) 50%);
      background-size: 100% 4px;
      z-index: 3;
      pointer-events: none;
      opacity: 0.55;
    }

    /* Viewfinder Corner Brackets */
    .bracket {
      position: absolute;
      width: 28px;
      height: 28px;
      border: 3.5px solid var(--accent);
      filter: drop-shadow(0 0 10px var(--accent-glow));
      pointer-events: none;
      z-index: 4;
      transition: all 0.35s var(--ease-spring);
    }
    .br-tl { top: 12px; left: 12px; border-right: none; border-bottom: none; border-top-left-radius: 6px; }
    .br-tr { top: 12px; right: 12px; border-left: none; border-bottom: none; border-top-right-radius: 6px; }
    .br-bl { bottom: 12px; left: 12px; border-right: none; border-top: none; border-bottom-left-radius: 6px; }
    .br-br { bottom: 12px; right: 12px; border-left: none; border-top: none; border-bottom-right-radius: 6px; }

    .viewfinder.scanning .bracket {
      border-color: #ffffff;
      filter: drop-shadow(0 0 12px var(--accent));
      animation: bracketPulse 0.8s ease-in-out;
    }
    @keyframes bracketPulse {
      0%, 100% { transform: scale(1); }
      50% { transform: scale(1.18); }
    }

    /* Animated AI Horizontal Laser Sweep */
    .ai-scan-laser {
      position: absolute;
      left: 0;
      right: 0;
      height: 3px;
      background: linear-gradient(90deg, transparent 0%, var(--accent) 25%, #ffffff 50%, var(--accent) 75%, transparent 100%);
      box-shadow: 0 0 16px var(--accent), 0 0 6px #ffffff;
      opacity: 0;
      pointer-events: none;
      top: 0;
      z-index: 5;
    }
    .ai-scan-laser.scanning {
      animation: laserSweep 0.8s cubic-bezier(0.4, 0, 0.2, 1) forwards;
    }
    @keyframes laserSweep {
      0% { top: 0%; opacity: 0; }
      15% { opacity: 1; }
      85% { opacity: 1; }
      100% { top: 100%; opacity: 0; }
    }

    .shutter-flash {
      position: absolute;
      inset: 0;
      background: white;
      opacity: 0;
      pointer-events: none;
      z-index: 6;
      transition: opacity 0.25s ease-out;
    }
    .shutter-flash.active { opacity: 0.85; transition: opacity 0.03s ease-in; }

    .hud-top {
      position: absolute;
      top: 16px;
      left: 50px;
      right: 50px;
      display: flex;
      justify-content: space-between;
      pointer-events: none;
      z-index: 5;
    }
    .hud-tag {
      background: rgba(17, 17, 27, 0.86);
      backdrop-filter: blur(8px);
      padding: 4px 10px;
      border-radius: 6px;
      font-size: 0.72rem;
      font-variant-numeric: tabular-nums;
      font-weight: 700;
      color: #cdd6f4;
      border: 1px solid rgba(255, 255, 255, 0.12);
    }
    .hud-bottom {
      position: absolute;
      bottom: 16px;
      left: 50px;
      right: 50px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      pointer-events: none;
      z-index: 5;
    }
    .live-target-badge {
      display: flex;
      align-items: center;
      gap: 6px;
      background: rgba(17, 17, 27, 0.9);
      backdrop-filter: blur(10px);
      padding: 6px 12px;
      border-radius: 8px;
      font-size: 0.84rem;
      font-weight: 700;
      color: #cdd6f4;
      border: 1.5px solid var(--accent);
    }

    /* 3D Mechanical Action Strip */
    .action-strip {
      display: grid;
      grid-template-columns: 1.4fr 1fr 1fr;
      gap: 10px;
    }
    .act-btn {
      position: relative;
      padding: 11px 12px;
      border-radius: 12px;
      border: 2px solid var(--border-color);
      border-bottom: 4px solid var(--key-shadow);
      background: var(--panel);
      color: var(--text);
      font-family: var(--font-mono);
      font-size: 0.82rem;
      font-weight: 800;
      display: inline-flex;
      align-items: center;
      justify-content: center;
      gap: 7px;
      cursor: pointer;
      user-select: none;
      transform: translateY(0);
      box-shadow: 0 4px 10px rgba(0, 0, 0, 0.25);
      transition: transform 0.12s var(--ease-spring), border-bottom-width 0.12s ease, background 0.15s ease, border-color 0.2s ease, box-shadow 0.2s ease;
    }
    .act-btn:hover {
      transform: translateY(-2px);
      border-bottom-width: 6px;
      border-color: var(--accent);
      color: var(--accent);
      box-shadow: 0 8px 18px var(--accent-glow);
    }
    .act-btn:active {
      transform: translateY(2px);
      border-bottom-width: 2px;
      box-shadow: 0 2px 4px rgba(0, 0, 0, 0.4);
    }
    .act-btn.primary {
      background: var(--accent);
      border-color: var(--accent);
      border-bottom: 4px solid var(--key-shadow);
      color: #11111b;
      box-shadow: 0 4px 14px var(--accent-glow);
    }
    .act-btn.primary:hover {
      filter: brightness(1.08);
      color: #11111b;
    }
    .act-btn.flash-active {
      background: var(--amber);
      border-color: var(--amber);
      color: #11111b;
    }

    /* Quick Simulator / Hardware Test Strip */
    .sim-strip {
      background: var(--panel);
      border: 1.5px solid var(--border-color);
      border-radius: 11px;
      padding: 8px 12px;
      display: flex;
      align-items: center;
      justify-content: space-between;
      flex-wrap: wrap;
      gap: 8px;
    }
    .sim-strip-title {
      font-size: 0.72rem;
      font-weight: 800;
      color: var(--text-muted);
    }
    .sim-btns {
      display: flex;
      align-items: center;
      gap: 6px;
      flex-wrap: wrap;
    }

    /* Right Column: Metrology, Price Tray, Active Order Cart */
    .console-col {
      display: flex;
      flex-direction: column;
      gap: 12px;
    }

    .metrology-block {
      padding: 13px 16px;
      display: flex;
      flex-direction: column;
      gap: 8px;
    }
    .metrology-row-top {
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    .metrology-title {
      font-size: 0.72rem;
      text-transform: uppercase;
      letter-spacing: 0.8px;
      font-weight: 800;
      color: var(--text-dim);
    }
    .source-pill {
      font-size: 0.65rem;
      background: var(--mantle);
      border: 1px solid var(--border-color);
      color: var(--green);
      padding: 2px 8px;
      border-radius: 6px;
      font-weight: 800;
    }
    .mass-hero-wrap {
      display: flex;
      align-items: baseline;
      justify-content: space-between;
      gap: 8px;
    }
    .mass-val {
      font-variant-numeric: tabular-nums;
      font-size: 3.1rem;
      font-weight: 800;
      line-height: 1;
      letter-spacing: -1px;
      color: var(--text);
    }
    .mass-unit {
      font-size: 1.1rem;
      font-weight: 800;
      color: var(--text-muted);
    }
    .mass-kg-sub {
      font-variant-numeric: tabular-nums;
      font-size: 0.95rem;
      font-weight: 800;
      color: var(--text-dim);
    }

    .receipt-divider {
      height: 1px;
      border: none;
      border-top: 1.5px dashed var(--border-color);
      margin: 3px 0;
    }

    .price-line {
      display: flex;
      align-items: baseline;
      justify-content: space-between;
      flex-wrap: wrap;
      gap: 8px;
    }
    .price-total-val {
      font-variant-numeric: tabular-nums;
      font-size: 2.1rem;
      font-weight: 800;
      letter-spacing: -0.5px;
      color: var(--accent);
      line-height: 1.05;
      transition: color 0.35s ease;
    }
    .price-rate-wrap {
      text-align: right;
      font-size: 0.82rem;
      color: var(--text-muted);
    }
    .edit-price-inline {
      color: var(--accent);
      font-size: 0.75rem;
      cursor: pointer;
      text-decoration: underline;
      margin-left: 4px;
      font-weight: 800;
    }

    @keyframes springPop {
      0% { transform: scale(0.65) rotate(-6deg); opacity: 0.5; }
      50% { transform: scale(1.25) rotate(4deg); opacity: 1; }
      75% { transform: scale(0.92) rotate(-2deg); }
      100% { transform: scale(1) rotate(0deg); opacity: 1; }
    }
    .spring-pop {
      display: inline-block;
      animation: springPop 0.48s var(--ease-spring) forwards;
    }

    /* Non-Blocking Price Editor Tray */
    .price-editor-tray {
      display: none;
      padding: 12px 14px;
      flex-direction: column;
      gap: 10px;
    }
    .price-editor-tray.open { display: flex; }
    .price-tray-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      font-size: 0.75rem;
      font-weight: 800;
      color: var(--text-muted);
    }
    .unit-switch-group {
      display: flex;
      gap: 4px;
      background: var(--mantle);
      padding: 2px;
      border-radius: 8px;
      border: 1px solid var(--border-color);
    }
    .unit-opt-btn {
      padding: 3px 8px;
      font-size: 0.7rem;
      font-family: var(--font-mono);
      font-weight: 800;
      border: none;
      background: none;
      color: var(--text-dim);
      border-radius: 6px;
      cursor: pointer;
    }
    .unit-opt-btn.selected {
      background: var(--accent);
      color: #11111b;
    }
    .price-inputs-grid {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: 8px;
    }
    .price-card {
      background: var(--mantle);
      border: 1px solid var(--border-color);
      border-radius: 9px;
      padding: 8px 10px;
      display: flex;
      flex-direction: column;
      gap: 6px;
    }
    .price-card-label {
      font-size: 0.74rem;
      font-weight: 800;
    }
    .price-stepper {
      display: flex;
      align-items: center;
      gap: 4px;
    }
    .step-btn {
      width: 28px;
      height: 28px;
      border-radius: 7px;
      border: 1.5px solid var(--border-color);
      border-bottom: 3px solid var(--key-shadow);
      background: var(--panel-surface);
      color: var(--text);
      font-family: var(--font-mono);
      font-size: 0.95rem;
      font-weight: 800;
      cursor: pointer;
      transition: transform 0.1s var(--ease-spring);
    }
    .step-btn:hover { transform: translateY(-1px); border-color: var(--accent); }
    .step-btn:active { transform: translateY(1.5px); }
    .price-input-box {
      flex: 1;
      width: 100%;
      min-width: 48px;
      background: var(--bg);
      border: 1.5px solid var(--border-color);
      border-radius: 7px;
      color: var(--text);
      font-family: var(--font-mono);
      font-size: 0.84rem;
      font-weight: 800;
      padding: 4px;
      text-align: center;
      outline: none;
    }
    .price-tray-footer {
      display: flex;
      justify-content: space-between;
      align-items: center;
      font-size: 0.72rem;
      color: var(--text-dim);
    }
    .reset-link {
      color: var(--accent);
      cursor: pointer;
      text-decoration: underline;
      font-weight: 700;
    }

    /* Fruit Variety Status Strip */
    .fruit-line {
      padding: 10px 14px;
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 10px;
    }
    .fruit-info-left {
      display: flex;
      align-items: center;
      gap: 12px;
      width: 100%;
    }
    .fruit-icon {
      font-size: 1.65rem;
      line-height: 1;
    }
    .fruit-name {
      font-size: 0.95rem;
      font-weight: 800;
      color: var(--text);
    }
    .fruit-consensus-txt {
      font-size: 0.73rem;
      color: var(--text-muted);
      margin-top: 2px;
    }
    .consensus-bar-wrap {
      width: 100%;
      height: 4px;
      background: var(--mantle);
      border-radius: 2px;
      overflow: hidden;
      margin-top: 4px;
    }
    .consensus-bar-fill {
      height: 100%;
      background: var(--accent);
      width: 0%;
      transition: width 0.3s ease, background-color 0.35s ease;
    }

    /* ==========================================================================
       Active Order POS Cart (`╭─ ~/pos/active-order` • Đang mở: ĐƠN #N)
       ========================================================================== */
    .order-cart-block {
      padding: 12px 14px;
      display: flex;
      flex-direction: column;
      gap: 10px;
    }
    .cart-controls-row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      flex-wrap: wrap;
      gap: 8px;
    }
    .cart-btn-group {
      display: flex;
      align-items: center;
      gap: 6px;
    }
    .cart-items-list {
      max-height: 175px;
      overflow-y: auto;
      display: flex;
      flex-direction: column;
      gap: 6px;
      padding-right: 2px;
    }
    .cart-empty-msg {
      text-align: center;
      font-size: 0.75rem;
      color: var(--text-dim);
      padding: 14px 8px;
      background: var(--mantle);
      border-radius: 8px;
      border: 1.5px dashed var(--border-color);
    }
    .cart-item-row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 8px;
      background: var(--mantle);
      border: 1px solid var(--border-color);
      border-radius: 8px;
      padding: 6px 10px;
      font-size: 0.76rem;
    }
    .cart-item-info {
      display: flex;
      align-items: center;
      gap: 6px;
      flex-wrap: wrap;
    }
    .cart-item-right {
      display: flex;
      align-items: center;
      gap: 8px;
    }
    .cart-item-price {
      font-weight: 800;
      color: var(--accent);
      font-variant-numeric: tabular-nums;
    }
    .del-item-btn {
      background: var(--panel-surface);
      border: 1px solid rgba(243, 139, 168, 0.4);
      border-bottom: 2.5px solid var(--key-shadow);
      border-radius: 6px;
      padding: 2px 7px;
      font-family: var(--font-mono);
      font-size: 0.7rem;
      font-weight: 800;
      cursor: pointer;
      color: var(--red);
      transition: transform 0.1s var(--ease-spring), background 0.15s ease;
    }
    .del-item-btn:hover {
      background: rgba(243, 139, 168, 0.2);
      border-color: var(--red);
      transform: translateY(-1px);
    }
    .del-item-btn:active {
      transform: translateY(1px);
    }
    .cart-summary-bar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      flex-wrap: wrap;
      gap: 10px;
      padding-top: 8px;
      border-top: 1.5px dashed var(--border-color);
    }
    .cart-totals {
      display: flex;
      flex-direction: column;
      gap: 2px;
    }
    .cart-total-money {
      font-size: 1.25rem;
      font-weight: 800;
      color: var(--green);
      font-variant-numeric: tabular-nums;
    }

    /* ==========================================================================
       Hardware Telemetry Monitor (`╭─ ~/sys/tinyml-telemetry`)
       ========================================================================== */
    .telem-inner {
      padding: 10px 14px;
      display: flex;
      flex-direction: column;
      gap: 8px;
    }
    .telem-grid {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: 8px;
    }
    .telem-cell {
      background: var(--mantle);
      border: 1px solid var(--border-color);
      border-radius: 8px;
      padding: 7px 9px;
      display: flex;
      flex-direction: column;
      gap: 4px;
      transition: background 0.3s ease, border-color 0.3s ease;
    }
    .telem-cell.flash-update {
      background: rgba(137, 180, 250, 0.22);
      border-color: var(--accent);
    }
    .telem-meta-row {
      display: flex;
      justify-content: space-between;
      align-items: baseline;
    }
    .telem-k {
      font-size: 0.63rem;
      color: var(--text-dim);
      font-weight: 800;
    }
    .telem-v {
      font-variant-numeric: tabular-nums;
      font-size: 0.8rem;
      font-weight: 800;
      color: var(--text);
    }
    .meter-bar {
      width: 100%;
      height: 3px;
      background: var(--panel-surface);
      border-radius: 2px;
      overflow: hidden;
    }
    .meter-fill {
      height: 100%;
      background: var(--accent);
      width: 50%;
      transition: width 0.35s ease, background 0.35s ease;
    }

    /* ==========================================================================
       Order History Manager (`╭─ ~/pos/history-csv`)
       ========================================================================== */
    .history-block {
      padding: 12px 14px;
      display: flex;
      flex-direction: column;
      gap: 10px;
    }
    .history-toolbar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      flex-wrap: wrap;
      gap: 10px;
    }
    .history-actions {
      display: flex;
      align-items: center;
      gap: 8px;
      flex-wrap: wrap;
    }
    .history-table-wrap {
      max-height: 260px;
      overflow-y: auto;
      border: 1px solid var(--border-color);
      border-radius: 9px;
      background: var(--mantle);
    }
    .history-table {
      width: 100%;
      border-collapse: collapse;
      font-size: 0.76rem;
      text-align: left;
    }
    .history-table th {
      position: sticky;
      top: 0;
      background: var(--panel-surface);
      color: var(--text-muted);
      font-weight: 800;
      text-transform: uppercase;
      font-size: 0.68rem;
      padding: 8px 10px;
      border-bottom: 1.5px solid var(--border-color);
      z-index: 1;
    }
    .history-table td {
      padding: 7px 10px;
      border-bottom: 1px solid rgba(137, 180, 250, 0.12);
      vertical-align: middle;
      font-variant-numeric: tabular-nums;
    }
    .history-table tr:hover td {
      background: rgba(137, 180, 250, 0.06);
    }
    .order-badge {
      display: inline-block;
      padding: 2px 7px;
      border-radius: 6px;
      background: rgba(137, 180, 250, 0.15);
      color: var(--accent);
      font-weight: 800;
      border: 1px solid var(--border-color);
    }

    /* Toast Notification */
    .rice-toast {
      position: fixed;
      bottom: 20px;
      right: 20px;
      background: var(--panel);
      border: 2px solid var(--accent);
      border-radius: 11px;
      padding: 10px 16px;
      color: var(--text);
      font-size: 0.78rem;
      font-weight: 800;
      box-shadow: 0 0 20px var(--accent-glow), 0 8px 24px rgba(0, 0, 0, 0.45);
      z-index: 999;
      opacity: 0;
      transform: translateY(12px);
      pointer-events: none;
      transition: opacity 0.25s ease, transform 0.25s var(--ease-spring);
    }
    .rice-toast.show {
      opacity: 1;
      transform: translateY(0);
    }

    footer {
      text-align: center;
      font-size: 0.71rem;
      color: var(--text-dim);
      padding: 4px 0;
    }
  </style>
</head>
<body>

  <div class="cockpit">
    <!-- Waybar Top Header -->
    <header class="header-bar">
      <div class="brand-left">
        <div class="ws-pills">
          <span class="ws-pill active">1: 📷 CAM</span>
          <span class="ws-pill active">2: 🛒 POS</span>
          <span class="ws-pill active">3: 📜 CSV</span>
        </div>
        <div class="brand-title">
          <span>Smart Scale POS</span>
          <span class="brand-badge">Hyprland Rice • Disk CSV</span>
        </div>
      </div>

      <div class="header-right">
        <div class="ip-pill" title="Địa chỉ IP của ESP32-CAM">
          <span>ESP32:</span>
          <input type="text" class="ip-input" id="esp-ip-input" value="192.168.4.1" onchange="onEspIpChanged(this.value)">
        </div>

        <button class="pill-btn" id="btn-price-menu" onclick="togglePriceMenu()">
          <span>🏷️</span>
          <span>Bảng Giá (VND)</span>
        </button>

        <button class="pill-btn" id="btn-theme-toggle" onclick="toggleRiceTheme()">
          <span id="lbl-theme-icon">🌙</span>
          <span id="lbl-theme-text">Mocha</span>
        </button>

        <div class="status-pill">
          <span class="conn-dot" id="conn-dot"></span>
          <span id="conn-status">SoftAP Live</span>
        </div>

        <label class="demo-toggle-label">
          <input type="checkbox" id="demo-check" onchange="toggleDemo(this.checked)">
          <span>Mô phỏng</span>
        </label>
      </div>
    </header>

    <!-- Main Hyprland Tiling Cockpit Grid -->
    <div class="cockpit-grid">
      <!-- Left Column: Camera Viewfinder, Action Keycaps, Quick Simulator & Telemetry -->
      <div class="monitor-col">
        <!-- Camera Tiled Window -->
        <div class="tiled-window">
          <div class="win-titlebar">
            <div class="win-controls">
              <span class="win-dot dot-close"></span>
              <span class="win-dot dot-min"></span>
              <span class="win-dot dot-max"></span>
              <span class="win-term-path">╭─ ~/dev/ov2640</span>
            </div>
            <div class="win-title-text">Camera Giám Sát (OV2640 • 320x240)</div>
            <span class="win-tag">CAMERA LIVE</span>
          </div>

          <div class="viewfinder" id="viewfinder-box" onclick="triggerInference()">
            <img id="cam-feed" src="/capture" alt="Camera Stream" onerror="handleImageError()">
            <div class="shutter-flash" id="shutter-fx"></div>
            <div class="crt-scanlines"></div>
            <div class="ai-scan-laser" id="ai-scan-laser"></div>

            <!-- Glowing Corner Brackets -->
            <div class="bracket br-tl"></div>
            <div class="bracket br-tr"></div>
            <div class="bracket br-bl"></div>
            <div class="bracket br-br"></div>

            <!-- Top HUD -->
            <div class="hud-top">
              <div class="hud-tag">FOMO INT8 • 96x96</div>
              <div class="hud-tag" id="cam-fps-hud">-- FPS</div>
            </div>

            <!-- Bottom HUD -->
            <div class="hud-bottom">
              <div class="live-target-badge">
                <span id="target-icon">⚖️</span>
                <span id="target-name">Đang quét đĩa cân...</span>
              </div>
              <div class="hud-tag" id="target-conf">-- %</div>
            </div>
          </div>
        </div>

        <!-- 3D Mechanical Action Keycaps -->
        <div class="action-strip">
          <button class="act-btn primary" onclick="triggerInference()">
            <span>⚡</span>
            <span>Quét Nhận Diện (AI)</span>
          </button>
          <button class="act-btn" id="btn-flash" onclick="toggleFlash()">
            <span>💡</span>
            <span id="lbl-flash">Đèn Flash</span>
          </button>
          <button class="act-btn" onclick="cycleWeight()">
            <span>⚖️</span>
            <span>Đổi Mức Cân</span>
          </button>
        </div>

        <!-- Quick Fruit Simulator Bar (For Instant Offline & Hardware Testing) -->
        <div class="sim-strip">
          <span class="sim-strip-title">🧪 Thử nhanh đĩa cân:</span>
          <div class="sim-btns">
            <button class="pill-btn" onclick="injectSampleFruit('apple', 215.0)">🍎 Táo (215g)</button>
            <button class="pill-btn" onclick="injectSampleFruit('orange', 180.0)">🍊 Cam (180g)</button>
            <button class="pill-btn" onclick="injectSampleFruit('pear', 245.0)">🍐 Lê (245g)</button>
            <button class="pill-btn" onclick="injectSampleFruit('none', 0.0)">🔄 Nhấc quả (0g)</button>
          </div>
        </div>

        <!-- Hardware & TinyML Telemetry Tiled Window -->
        <div class="tiled-window">
          <div class="win-titlebar">
            <div class="win-controls">
              <span class="win-dot dot-close"></span>
              <span class="win-dot dot-min"></span>
              <span class="win-dot dot-max"></span>
              <span class="win-term-path">╭─ ~/sys/tinyml-telemetry</span>
            </div>
            <div class="win-title-text">Thông Số Phần Cứng &amp; TinyML AI</div>
            <span class="win-tag" id="telem-live-pill">TRỰC TIẾP ●</span>
          </div>

          <div class="telem-inner">
            <div class="telem-grid">
              <div class="telem-cell" id="c-infer">
                <div class="telem-meta-row">
                  <span class="telem-k">AI_LATENCY</span>
                  <span class="telem-v" id="t-infer">-- ms</span>
                </div>
                <div class="meter-bar"><div class="meter-fill" id="bar-infer" style="width: 42%;"></div></div>
              </div>
              <div class="telem-cell" id="c-dsp">
                <div class="telem-meta-row">
                  <span class="telem-k">DSP_IMAGE</span>
                  <span class="telem-v" id="t-dsp">-- ms</span>
                </div>
                <div class="meter-bar"><div class="meter-fill" id="bar-dsp" style="width: 16%;"></div></div>
              </div>
              <div class="telem-cell" id="c-fps">
                <div class="telem-meta-row">
                  <span class="telem-k">FRAME_RATE</span>
                  <span class="telem-v" id="t-fps">-- FPS</span>
                </div>
                <div class="meter-bar"><div class="meter-fill" id="bar-fps" style="width: 66%;"></div></div>
              </div>
              <div class="telem-cell" id="c-consensus">
                <div class="telem-meta-row">
                  <span class="telem-k">VOTES_BURST</span>
                  <span class="telem-v" id="t-votes">--</span>
                </div>
                <div class="meter-bar"><div class="meter-fill" id="bar-votes" style="width: 100%;"></div></div>
              </div>
              <div class="telem-cell" id="c-temp">
                <div class="telem-meta-row">
                  <span class="telem-k">DIE_TEMP</span>
                  <span class="telem-v" id="t-temp">-- °C</span>
                </div>
                <div class="meter-bar"><div class="meter-fill" id="bar-temp" style="width: 60%;"></div></div>
              </div>
              <div class="telem-cell" id="c-psram">
                <div class="telem-meta-row">
                  <span class="telem-k">PSRAM_FREE</span>
                  <span class="telem-v" id="t-psram">-- KB</span>
                </div>
                <div class="meter-bar"><div class="meter-fill" id="bar-psram" style="width: 92%;"></div></div>
              </div>
            </div>
          </div>
        </div>
      </div>

      <!-- Right Column: Metrology Readout, Price Editor, Fruit Strip & Active Order Cart -->
      <div class="console-col">
        <!-- Metrology & Pricing Tiled Window -->
        <div class="tiled-window">
          <div class="win-titlebar">
            <div class="win-controls">
              <span class="win-dot dot-close"></span>
              <span class="win-dot dot-min"></span>
              <span class="win-dot dot-max"></span>
              <span class="win-term-path">╭─ ~/dev/hx711-pos</span>
            </div>
            <div class="win-title-text">Bảng Tính Tiền &amp; Cân Khối Lượng (HX711)</div>
            <span class="win-tag">POS THÀNH TIỀN</span>
          </div>

          <div class="metrology-block">
            <div class="metrology-row-top">
              <div style="display:flex; align-items:center; gap:8px;">
                <span class="metrology-title">Khối lượng cân</span>
                <span class="source-pill" id="val-source-pill">STM32 HX711</span>
              </div>
              <span class="mass-kg-sub" id="val-mass-kg">0,000 kg</span>
            </div>

            <div class="mass-hero-wrap">
              <div>
                <span class="mass-val" id="val-mass">0.0</span>
                <span class="mass-unit">g</span>
              </div>
            </div>

            <div class="receipt-divider"></div>

            <div class="price-line">
              <div>
                <div class="metrology-title">Thành tiền món đang cân</div>
                <div class="price-total-val" id="val-total">0 ₫</div>
              </div>
              <div class="price-rate-wrap">
                <div id="val-unit-rate">
                  Đơn giá: 0 ₫ / kg
                  <span class="edit-price-inline" onclick="togglePriceMenu()">[Sửa]</span>
                </div>
                <div style="font-size:0.73rem; color:var(--text-dim);" id="val-tare-state">Đã trừ bì (Tare 0g) • Cảm biến HX711 sẵn sàng</div>
              </div>
            </div>
          </div>
        </div>

        <!-- Non-Blocking Unit Price Tray -->
        <div class="tiled-window price-editor-tray" id="price-tray">
          <div class="win-titlebar" style="margin:-12px -14px 8px -14px;">
            <div class="win-controls">
              <span class="win-dot dot-close"></span>
              <span class="win-dot dot-min"></span>
              <span class="win-dot dot-max"></span>
              <span class="win-term-path">╭─ ~/cfg/unit-prices</span>
            </div>
            <div class="win-title-text">Cài Đặt Đơn Giá Nhanh (VND)</div>
            <span class="win-tag">SYNC ESP32 + DISK</span>
          </div>

          <div class="price-tray-header">
            <span>Đơn vị hiển thị giá:</span>
            <div class="unit-switch-group">
              <button class="unit-opt-btn selected" id="unit-kg-btn" onclick="setPriceUnit('kg')">/ kg</button>
              <button class="unit-opt-btn" id="unit-100g-btn" onclick="setPriceUnit('100g')">/ 100g</button>
              <button class="unit-opt-btn" id="unit-g-btn" onclick="setPriceUnit('g')">/ g</button>
            </div>
          </div>

          <div class="price-inputs-grid">
            <div class="price-card">
              <span class="price-card-label">🍎 Táo (Apple)</span>
              <div class="price-stepper">
                <button class="step-btn" onclick="stepPrice('apple', -1000)">−</button>
                <input type="number" step="100" min="0" class="price-input-box" id="inp-price-apple" oninput="onCustomPriceChange('apple', this.value)">
                <button class="step-btn" onclick="stepPrice('apple', 1000)">+</button>
              </div>
            </div>

            <div class="price-card">
              <span class="price-card-label">🍊 Cam (Orange)</span>
              <div class="price-stepper">
                <button class="step-btn" onclick="stepPrice('orange', -1000)">−</button>
                <input type="number" step="100" min="0" class="price-input-box" id="inp-price-orange" oninput="onCustomPriceChange('orange', this.value)">
                <button class="step-btn" onclick="stepPrice('orange', 1000)">+</button>
              </div>
            </div>

            <div class="price-card">
              <span class="price-card-label">🍐 Lê (Pear)</span>
              <div class="price-stepper">
                <button class="step-btn" onclick="stepPrice('pear', -1000)">−</button>
                <input type="number" step="100" min="0" class="price-input-box" id="inp-price-pear" oninput="onCustomPriceChange('pear', this.value)">
                <button class="step-btn" onclick="stepPrice('pear', 1000)">+</button>
              </div>
            </div>
          </div>

          <div class="price-tray-footer">
            <span>Tự động lưu `pos_state.json` &amp; đồng bộ `/set_prices`</span>
            <span class="reset-link" onclick="resetDefaultPrices()">Khôi phục giá mặc định</span>
          </div>
        </div>

        <!-- Detected Fruit Variety Strip -->
        <div class="tiled-window fruit-line">
          <div class="fruit-info-left">
            <span class="fruit-icon" id="fruit-emoji">⚖️</span>
            <div style="flex:1;">
              <div class="fruit-name" id="fruit-title">Cân trống</div>
              <div class="fruit-consensus-txt" id="fruit-sub">Đĩa cân trống • Sẵn sàng đặt quả</div>
              <div class="consensus-bar-wrap">
                <div class="consensus-bar-fill" id="consensus-bar"></div>
              </div>
            </div>
          </div>
        </div>

        <!-- Active Order POS Cart Window (`╭─ ~/pos/active-order` • Đơn #1 -> Đơn #2) -->
        <div class="tiled-window">
          <div class="win-titlebar">
            <div class="win-controls">
              <span class="win-dot dot-close"></span>
              <span class="win-dot dot-min"></span>
              <span class="win-dot dot-max"></span>
              <span class="win-term-path">╭─ ~/pos/active-order</span>
            </div>
            <div class="win-title-text">Đang mở: <span id="active-order-label" style="color:var(--accent);">ĐƠN #1</span> (<span id="active-order-code">ORD-0001</span>)</div>
            <span class="win-tag" id="cart-count-tag">0 MÓN</span>
          </div>

          <div class="order-cart-block">
            <div class="cart-controls-row">
              <label class="demo-toggle-label" style="color:var(--text);">
                <input type="checkbox" id="auto-add-check" checked onchange="onAutoAddToggle(this.checked)">
                <span>Tự động thêm vào đơn (≥ 50g)</span>
              </label>
              <div class="cart-btn-group">
                <button class="pill-btn" onclick="addCurrentToCart(true)">
                  <span>➕ Thêm món</span>
                </button>
                <button class="pill-btn danger" onclick="clearActiveCart()">
                  <span>🔄 Làm trống giỏ</span>
                </button>
              </div>
            </div>

            <div class="cart-items-list" id="cart-items-container">
              <div class="cart-empty-msg">Chưa có mặt hàng nào trong Đơn #1. Đặt quả lên cân hoặc nhấn "➕ Thêm món".</div>
            </div>

            <div class="cart-summary-bar">
              <div class="cart-totals">
                <span style="font-size:0.73rem; color:var(--text-dim);">Tổng đơn (<span id="cart-total-weight">0.0 g</span>):</span>
                <span class="cart-total-money" id="cart-total-money">0 ₫</span>
              </div>
              <button class="pill-btn success" id="btn-finalize-order" onclick="finalizeOrderAndNext()">
                <span>🧾 Xuất Đơn &amp; Sang Đơn Mới</span>
              </button>
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- Order History Manager (`╭─ ~/pos/history-csv`) -->
    <div class="tiled-window">
      <div class="win-titlebar">
        <div class="win-controls">
          <span class="win-dot dot-close"></span>
          <span class="win-dot dot-min"></span>
          <span class="win-dot dot-max"></span>
          <span class="win-term-path">╭─ ~/pos/history-csv</span>
        </div>
        <div class="win-title-text">📜 Lịch Sử Đơn Hàng (Lưu Trực Tiếp `data/pos_orders_history.csv`)</div>
        <span class="win-tag" id="history-count-tag">0 ĐƠN ĐÃ LƯU</span>
      </div>

      <div class="history-block">
        <div class="history-toolbar">
          <div style="font-size:0.8rem; color:var(--text-muted);">
            Tổng doanh thu tích lũy: <strong style="color:var(--green);" id="history-grand-total">0 ₫</strong>
            <span style="margin-left:10px; font-size:0.72rem; color:var(--text-dim);" id="csv-path-badge">📁 data/pos_orders_history.csv</span>
          </div>
          <div class="history-actions">
            <button class="pill-btn" onclick="openCsvOnPC()">
              <span>📂 Mở File CSV trên PC</span>
            </button>
            <button class="pill-btn danger" onclick="clearAllOrderHistory()">
              <span>🗑️ Xóa toàn bộ / Reset Đơn #1</span>
            </button>
          </div>
        </div>

        <div class="history-table-wrap">
          <table class="history-table">
            <thead>
              <tr>
                <th>Mã Đơn</th>
                <th>Thời gian</th>
                <th>Số món</th>
                <th>Chi tiết món</th>
                <th>Tổng KL (g)</th>
                <th>Tổng Tiền (VND)</th>
                <th style="text-align:center;">Thao tác</th>
              </tr>
            </thead>
            <tbody id="history-table-body">
              <tr>
                <td colspan="7" style="text-align:center; color:var(--text-dim); padding:16px;">Chưa có đơn hàng nào được xuất.</td>
              </tr>
            </tbody>
          </table>
        </div>
      </div>
    </div>

    <footer>
      Smart Scale POS — Hyprland / Catppuccin Rice Edition • Local Python Disk API (`127.0.0.1:8765`) &amp; ESP32-CAM TinyML
    </footer>
  </div>

  <div class="rice-toast" id="rice-toast"></div>

  <script>
    // ==========================================================================
    // State & Theme Palettes (Catppuccin Mocha 🌙 & Latte ☀️)
    // ==========================================================================
    let isLiveConnected = false;
    let isDemoMode = false;
    let flashActive = false;
    let demoIndex = 3; // Matches DEMO_SAMPLES[3] (empty scale at startup so first cycle selects Apple)
    let priceUnit = 'kg';
    let prevFruitLabel = '';
    let currentTheme = 'night';

    let activeOrderNumber = 1;
    let currentOrderItems = [];
    let orderHistory = [];
    let cartItemSeq = 0;
    let isCheckingOut = false;

    // Auto-add debounce state machine:
    // Resets scale_was_empty = true when weight_g < 15g so holding a fruit never duplicates rows
    let scale_was_empty = true;
    let stableReadingCount = 0;
    let lastAutoAddedWeight = 0.0;
    let lastAutoAddedLabel = '';

    const DEFAULT_PRICES = { apple: 60000, orange: 40000, pear: 70000 };
    let customPrices = { ...DEFAULT_PRICES };

    const PALETTES = {
      night: {
        apple:  { accent: '#f38ba8', glow: 'rgba(243, 139, 168, 0.28)' },
        orange: { accent: '#fab387', glow: 'rgba(250, 179, 135, 0.28)' },
        pear:   { accent: '#a6e3a1', glow: 'rgba(166, 227, 161, 0.28)' },
        none:   { accent: '#89b4fa', glow: 'rgba(137, 180, 250, 0.22)' }
      },
      day: {
        apple:  { accent: '#d20f39', glow: 'rgba(210, 15, 57, 0.24)' },
        orange: { accent: '#fe640b', glow: 'rgba(254, 100, 11, 0.24)' },
        pear:   { accent: '#40a02b', glow: 'rgba(64, 160, 43, 0.24)' },
        none:   { accent: '#1e66f5', glow: 'rgba(30, 102, 245, 0.20)' }
      }
    };

    const FRUITS = {
      apple:  { name: "Táo (Apple)", shortName: "Táo", emoji: "🍎" },
      orange: { name: "Cam (Orange)", shortName: "Cam", emoji: "🍊" },
      pear:   { name: "Lê (Pear)", shortName: "Lê", emoji: "🍐" },
      none:   { name: "Cân trống", shortName: "Trống", emoji: "⚖️" }
    };

    const DEMO_SAMPLES = [
      { label: "apple", detected: true, confidence: 0.95, weight: 215.0, votes: 3, burst_total: 3, dsp_ms: 8, infer_ms: 42, temp: 48.2, psram_kb: 3840, heap_kb: 172, stm32_live: true },
      { label: "orange", detected: true, confidence: 0.91, weight: 180.0, votes: 3, burst_total: 3, dsp_ms: 9, infer_ms: 41, temp: 48.6, psram_kb: 3840, heap_kb: 171, stm32_live: true },
      { label: "pear", detected: true, confidence: 0.96, weight: 245.0, votes: 3, burst_total: 3, dsp_ms: 8, infer_ms: 44, temp: 49.0, psram_kb: 3840, heap_kb: 170, stm32_live: true },
      { label: "none", detected: false, confidence: 0.0, weight: 0.0, votes: 0, burst_total: 3, dsp_ms: 7, infer_ms: 39, temp: 47.8, psram_kb: 3840, heap_kb: 174, stm32_live: true }
    ];

    let currentTelemetry = DEMO_SAMPLES[3]; // Start with empty scale (0g)

    let toastTimer = null;
    function showToast(msg) {
      const el = document.getElementById('rice-toast');
      if (!el) return;
      el.innerText = msg;
      el.classList.add('show');
      if (toastTimer) clearTimeout(toastTimer);
      toastTimer = setTimeout(() => el.classList.remove('show'), 3200);
    }

    function formatVND(val) {
      return Math.round(val || 0).toLocaleString('vi-VN') + ' ₫';
    }

    // ==========================================================================
    // Theme Switcher (🌙 Mocha / ☀️ Latte)
    // ==========================================================================
    function applyTheme(theme) {
      currentTheme = (theme === 'day') ? 'day' : 'night';
      if (currentTheme === 'day') {
        document.documentElement.dataset.theme = 'day';
      } else {
        delete document.documentElement.dataset.theme;
      }
      const icon = document.getElementById('lbl-theme-icon');
      const text = document.getElementById('lbl-theme-text');
      if (currentTheme === 'day') {
        if (icon) icon.innerText = '☀️';
        if (text) text.innerText = 'Latte';
      } else {
        if (icon) icon.innerText = '🌙';
        if (text) text.innerText = 'Mocha';
      }
      try { localStorage.setItem('scale_rice_theme', currentTheme); } catch (e) {}
      renderData(currentTelemetry, true);
    }

    function toggleRiceTheme() {
      const nextTheme = (currentTheme === 'night') ? 'day' : 'night';
      applyTheme(nextTheme);
      syncStateToBackend({ theme: nextTheme });
    }

    // ==========================================================================
    // Python Local Disk API Sync (`/api/state`, `/api/checkout`, `/api/delete_order`, `/api/clear_history`, `/api/open_csv`)
    // ==========================================================================
    async function fetchBackendState() {
      try {
        const res = await fetch('/api/state');
        const data = await res.json();
        if (data && data.ok) {
          applyBackendState(data);
        }
      } catch (e) {
        console.warn('Could not load backend state:', e);
      }
    }

    function applyBackendState(state) {
      if (state.next_order_no) {
        activeOrderNumber = state.next_order_no;
      }
      if (state.prices) {
        customPrices = {
          apple: Number(state.prices.apple || 60000),
          orange: Number(state.prices.orange || 40000),
          pear: Number(state.prices.pear || 70000)
        };
        syncPriceInputs();
      }
      if (state.esp_ip) {
        const ipInp = document.getElementById('esp-ip-input');
        if (ipInp && document.activeElement !== ipInp) {
          ipInp.value = state.esp_ip;
        }
      }
      if (typeof state.auto_add === 'boolean') {
        const chk = document.getElementById('auto-add-check');
        if (chk) chk.checked = state.auto_add;
      }
      if (state.theme && state.theme !== currentTheme) {
        applyTheme(state.theme);
      }
      if (Array.isArray(state.history)) {
        orderHistory = state.history;
      }
      renderCartUI();
      renderOrderHistoryUI();
    }

    async function syncStateToBackend(partial) {
      try {
        const res = await fetch('/api/state', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(partial)
        });
        const data = await res.json();
        if (data && data.state) {
          applyBackendState(data.state);
        }
      } catch (e) {}
    }

    function onEspIpChanged(val) {
      const ip = (val || '').trim() || '192.168.4.1';
      syncStateToBackend({ esp_ip: ip }).then(() => {
        if (!isDemoMode) {
          consecutiveFailCount = 0;
          cancelFrameInFlight();
          updateFrame();
          scheduleNextPoll(50);
        }
      });
      showToast(`🌐 Đã cập nhật địa chỉ ESP32: ${ip}`);
    }

    function onAutoAddToggle(checked) {
      syncStateToBackend({ auto_add: checked });
      if (checked && scale_was_empty && currentTelemetry) {
        renderData(currentTelemetry, false);
      }
    }

    function syncPostActionScaleLock() {
      const d = currentTelemetry || {};
      const massG = (d.weight !== undefined ? parseFloat(d.weight) : parseFloat(d.weight_g || 0)) || 0.0;
      const rawLabel = (d.label || 'none').toLowerCase();
      if (massG < 15.0 || rawLabel === 'none') {
        scale_was_empty = true;
      } else {
        // A fruit is still sitting on the scale: keep scale_was_empty = false until it is lifted (< 15g)
        scale_was_empty = false;
        lastAutoAddedWeight = massG;
        lastAutoAddedLabel = rawLabel;
      }
      stableReadingCount = 0;
    }

    // ==========================================================================
    // Active Order Cart (`Đơn #1` -> `Đơn #2`) & Auto-Add Debounce
    // ==========================================================================
    function addCurrentToCart(isManual) {
      const d = currentTelemetry || {};
      const rawLabel = (d.label || 'none').toLowerCase();
      const isFruit = (rawLabel === 'apple' || rawLabel === 'orange' || rawLabel === 'pear');
      const massG = d.weight > 0 ? parseFloat(d.weight) : 0.0;

      if (!isFruit || massG < 15.0) {
        if (isManual) {
          showToast('⚠️ Đĩa cân đang trống! Hãy đặt quả (Táo, Cam, Lê) lên cân trước.');
        }
        return;
      }
      if (!isManual && massG < 50.0) {
        return;
      }

      const meta = FRUITS[rawLabel] || FRUITS.none;
      const unitPriceKg = customPrices[rawLabel] || DEFAULT_PRICES[rawLabel] || 60000;
      const totalVND = Math.round((massG / 1000.0) * unitPriceKg);

      cartItemSeq += 1;
      currentOrderItems.push({
        id: cartItemSeq,
        label: rawLabel,
        name: meta.name,
        short_name: meta.shortName,
        emoji: meta.emoji,
        weight_g: Number(massG.toFixed(1)),
        unit_price_kg: unitPriceKg,
        total_vnd: totalVND,
        time: new Date().toLocaleTimeString('vi-VN')
      });

      scale_was_empty = false;
      lastAutoAddedWeight = massG;
      lastAutoAddedLabel = rawLabel;
      stableReadingCount = 0;

      renderCartUI();
      showToast(`➕ Đã thêm ${meta.emoji} ${meta.shortName} (${massG.toFixed(0)}g) vào Đơn #${activeOrderNumber}`);
    }

    function removeCartItem(itemId) {
      const numericId = Number(itemId);
      currentOrderItems = currentOrderItems.filter(item => item.id !== numericId);
      renderCartUI();
    }

    function clearActiveCart() {
      if (currentOrderItems.length === 0) return;
      currentOrderItems = [];
      syncPostActionScaleLock();
      renderCartUI();
      showToast(`🔄 Đã làm trống giỏ hàng Đơn #${activeOrderNumber}`);
    }

    function renderCartUI() {
      const orderCode = 'ORD-' + String(activeOrderNumber).padStart(4, '0');
      const lblEl = document.getElementById('active-order-label');
      if (lblEl) lblEl.innerText = `ĐƠN #${activeOrderNumber}`;

      const codeEl = document.getElementById('active-order-code');
      if (codeEl) codeEl.innerText = orderCode;

      const countTag = document.getElementById('cart-count-tag');
      if (countTag) countTag.innerText = `${currentOrderItems.length} MÓN`;

      const container = document.getElementById('cart-items-container');
      if (container) {
        if (currentOrderItems.length === 0) {
          container.innerHTML = `<div class="cart-empty-msg">Chưa có mặt hàng nào trong Đơn #${activeOrderNumber} (${orderCode}). Đặt quả lên cân (≥ 50g) hoặc nhấn "➕ Thêm món".</div>`;
        } else {
          container.innerHTML = currentOrderItems.map((item, idx) => `
            <div class="cart-item-row">
              <div class="cart-item-info">
                <span><strong>#${idx + 1}</strong> ${item.emoji} ${item.name}</span>
                <span style="color:var(--text-muted);">• ${item.weight_g.toFixed(1)} g</span>
              </div>
              <div class="cart-item-right">
                <span class="cart-item-price">${formatVND(item.total_vnd)}</span>
                <button class="del-item-btn" title="Xóa món này khỏi đơn" onclick="removeCartItem(${item.id})">❌</button>
              </div>
            </div>
          `).join('');
        }
      }

      const totalWeightG = currentOrderItems.reduce((acc, it) => acc + it.weight_g, 0);
      const totalMoneyVND = currentOrderItems.reduce((acc, it) => acc + it.total_vnd, 0);

      const weightEl = document.getElementById('cart-total-weight');
      if (weightEl) weightEl.innerText = `${totalWeightG.toFixed(1)} g`;

      const moneyEl = document.getElementById('cart-total-money');
      if (moneyEl) moneyEl.innerText = formatVND(totalMoneyVND);
    }

    async function finalizeOrderAndNext() {
      if (isCheckingOut) return;
      if (currentOrderItems.length === 0 && currentTelemetry) {
        const rawLabel = (currentTelemetry.label || 'none').toLowerCase();
        const isFruit = (rawLabel === 'apple' || rawLabel === 'orange' || rawLabel === 'pear');
        if (isFruit && currentTelemetry.weight >= 15.0) {
          addCurrentToCart(true);
        }
      }
      if (currentOrderItems.length === 0) {
        showToast(`⚠️ Đơn #${activeOrderNumber} đang trống! Hãy cân ít nhất 1 quả trước khi xuất đơn.`);
        return;
      }

      isCheckingOut = true;
      const itemsSnapshot = [...currentOrderItems];
      try {
        const res = await fetch('/api/checkout', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ items: itemsSnapshot })
        });
        const data = await res.json();
        if (data && data.ok) {
          const ord = data.order;
          currentOrderItems = [];
          syncPostActionScaleLock();
          applyBackendState(data.state);
          if (data.csv_warning) {
            showToast(`⚠️ Đã lưu ${ord.order_id} • ${data.csv_warning}`);
          } else {
            showToast(`✅ Đã ghi ${ord.order_id} (${formatVND(ord.total_amount_vnd)}) vào CSV • Chuyển sang ĐƠN #${activeOrderNumber}`);
          }
        } else {
          showToast(`❌ Lỗi xuất đơn: ${(data && data.error) || 'Không xác định'}`);
        }
      } catch (e) {
        showToast(`❌ Không thể kết nối Backend Python: ${e}`);
      } finally {
        isCheckingOut = false;
      }
    }

    async function deleteHistoryOrder(orderId) {
      try {
        const res = await fetch('/api/delete_order', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ order_id: orderId })
        });
        const data = await res.json();
        if (data && data.ok) {
          applyBackendState(data.state);
          showToast(`🗑️ Đã xóa đơn ${orderId} và cập nhật lại data/pos_orders_history.csv`);
        }
      } catch (e) {
        showToast(`❌ Lỗi xóa đơn: ${e}`);
      }
    }

    async function clearAllOrderHistory() {
      try {
        const res = await fetch('/api/clear_history', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ reset_order_no: true })
        });
        const data = await res.json();
        if (data && data.ok) {
          currentOrderItems = [];
          syncPostActionScaleLock();
          applyBackendState(data.state);
          showToast('🗑️ Đã xóa toàn bộ lịch sử CSV & Reset về ĐƠN #1 (ORD-0001)');
        }
      } catch (e) {
        showToast(`❌ Lỗi xóa lịch sử: ${e}`);
      }
    }

    async function openCsvOnPC() {
      try {
        const res = await fetch('/api/open_csv', { method: 'POST' });
        const data = await res.json();
        if (data && data.ok) {
          showToast('📂 Đã mở file data/pos_orders_history.csv trên PC!');
        } else {
          showToast(`⚠️ Không thể mở tự động: ${(data && data.error) || ''}`);
        }
      } catch (e) {
        showToast(`❌ Lỗi mở file CSV: ${e}`);
      }
    }

    function renderOrderHistoryUI() {
      const countTag = document.getElementById('history-count-tag');
      if (countTag) countTag.innerText = `${orderHistory.length} ĐƠN ĐÃ LƯU`;

      const grandTotal = orderHistory.reduce((acc, o) => acc + (Number(o.total_amount_vnd) || 0), 0);
      const grandEl = document.getElementById('history-grand-total');
      if (grandEl) grandEl.innerText = formatVND(grandTotal);

      const tbody = document.getElementById('history-table-body');
      if (!tbody) return;

      if (orderHistory.length === 0) {
        tbody.innerHTML = `<tr><td colspan="7" style="text-align:center; color:var(--text-dim); padding:16px;">Chưa có đơn hàng nào được xuất.</td></tr>`;
        return;
      }

      tbody.innerHTML = orderHistory.map(ord => `
        <tr>
          <td><span class="order-badge">${ord.order_id}</span></td>
          <td style="color:var(--text-muted);">${ord.timestamp}</td>
          <td><strong>${ord.item_count} món</strong></td>
          <td>${ord.items_summary}</td>
          <td><strong>${Number(ord.total_weight_g || 0).toFixed(1)} g</strong></td>
          <td><strong style="color:var(--green);">${formatVND(ord.total_amount_vnd)}</strong></td>
          <td style="text-align:center;">
            <button class="del-item-btn" title="Xóa đơn này khỏi CSV" onclick="deleteHistoryOrder('${ord.order_id}')">❌ Xóa đơn</button>
          </td>
        </tr>
      `).join('');
    }

    // ==========================================================================
    // Unit Price Editor & ESP32 Sync
    // ==========================================================================
    function convertRateForUnit(pricePerKg, unit) {
      if (unit === 'g') return pricePerKg / 1000.0;
      if (unit === '100g') return pricePerKg / 10.0;
      return pricePerKg;
    }

    function convertToPricePerKg(val, unit) {
      if (unit === 'g') return val * 1000.0;
      if (unit === '100g') return val * 10.0;
      return val;
    }

    function syncPriceInputs() {
      ['apple', 'orange', 'pear'].forEach(k => {
        const inp = document.getElementById('inp-price-' + k);
        if (inp) {
          inp.value = Math.round(convertRateForUnit(customPrices[k], priceUnit));
        }
      });
      ['kg', '100g', 'g'].forEach(u => {
        const btn = document.getElementById('unit-' + u + '-btn');
        if (btn) btn.classList.toggle('selected', u === priceUnit);
      });
    }

    function togglePriceMenu() {
      const tray = document.getElementById('price-tray');
      const btn = document.getElementById('btn-price-menu');
      const isOpen = tray.classList.toggle('open');
      btn.classList.toggle('active', isOpen);
      if (isOpen) syncPriceInputs();
    }

    function setPriceUnit(u) {
      priceUnit = u;
      syncPriceInputs();
      renderData(currentTelemetry, true);
    }

    function onCustomPriceChange(fruit, val) {
      const num = parseFloat(val);
      if (!isNaN(num) && num >= 0) {
        customPrices[fruit] = convertToPricePerKg(num, priceUnit);
        savePrices();
        renderData(currentTelemetry, true);
      }
    }

    function stepPrice(fruit, delta) {
      const currentRate = convertRateForUnit(customPrices[fruit], priceUnit);
      let stepDelta = delta;
      if (priceUnit === '100g') stepDelta = delta / 10.0;
      if (priceUnit === 'g') stepDelta = Math.max(1, delta / 1000.0);

      const newRate = Math.max(0, currentRate + stepDelta);
      customPrices[fruit] = convertToPricePerKg(newRate, priceUnit);
      savePrices();
      syncPriceInputs();
      renderData(currentTelemetry, true);
    }

    function resetDefaultPrices() {
      customPrices = { ...DEFAULT_PRICES };
      savePrices();
      syncPriceInputs();
      renderData(currentTelemetry, true);
      showToast('🏷️ Đã khôi phục bảng giá mặc định');
    }

    function savePrices() {
      syncStateToBackend({ prices: customPrices });
      if (!isDemoMode) {
        fetch(`/set_prices?apple=${Math.round(customPrices.apple)}&orange=${Math.round(customPrices.orange)}&pear=${Math.round(customPrices.pear)}`).catch(() => {});
      }
    }

    // ==========================================================================
    // Camera Viewfinder, Simulator & Telemetry Rendering
    // ==========================================================================
    function createSvgView(label, weightG) {
      const themeSet = PALETTES[currentTheme] || PALETTES.night;
      const c = (themeSet[label] ? themeSet[label].accent : '#89b4fa');
      const meta = FRUITS[label] || FRUITS.none;
      const bg = (currentTheme === 'day') ? "%23eff1f5" : "%2305070e";
      const txtColor = (currentTheme === 'day') ? "%234c4f69" : "%23cdd6f4";
      const subText = label !== 'none' ? `${meta.shortName.toUpperCase()} • ${Number(weightG || 0).toFixed(1)}g` : "READY • 0.0g";
      return `data:image/svg+xml;utf8,<svg xmlns="http://www.w3.org/2000/svg" width="320" height="240" viewBox="0 0 320 240"><rect width="320" height="240" fill="${bg}"/><circle cx="160" cy="112" r="56" fill="${encodeURIComponent(c)}" opacity="0.22"/><circle cx="160" cy="112" r="40" fill="${encodeURIComponent(c)}" opacity="0.72"/><text x="160" y="126" font-size="38" text-anchor="middle" font-family="sans-serif">${encodeURIComponent(meta.emoji)}</text><text x="160" y="196" font-size="12" font-weight="bold" fill="${txtColor}" text-anchor="middle" font-family="monospace">${encodeURIComponent(subText)}</text></svg>`;
    }

    let lastFrameTime = performance.now();
    let rollingStreamFps = 12.0;
    let frameTimerId = null;
    let frameInFlight = false;
    let frameFetchTimeoutId = null;
    let activeFrameImg = null;
    let pollTimerId = null;
    let pollInFlight = false;
    let consecutiveFailCount = 0;
    let hasReceivedLiveFrame = false;

    function cancelFrameInFlight() {
      if (frameFetchTimeoutId) {
        clearTimeout(frameFetchTimeoutId);
        frameFetchTimeoutId = null;
      }
      if (activeFrameImg) {
        activeFrameImg.onload = null;
        activeFrameImg.onerror = null;
        activeFrameImg = null;
      }
      frameInFlight = false;
    }

    function scheduleNextFrame(delayMs) {
      if (frameTimerId) {
        clearTimeout(frameTimerId);
        frameTimerId = null;
      }
      if (!isDemoMode) {
        frameTimerId = setTimeout(updateFrame, delayMs);
      }
    }

    function updateFrame() {
      if (isDemoMode || frameInFlight) return;
      frameInFlight = true;
      const view = document.getElementById('cam-feed');
      const img = new Image();
      activeFrameImg = img;
      frameFetchTimeoutId = setTimeout(() => {
        if (activeFrameImg === img) {
          cancelFrameInFlight();
          if (isDemoMode) return;
          handleImageError();
          scheduleNextFrame(consecutiveFailCount >= 3 ? 650 : 220);
        }
      }, 2200);
      img.onload = () => {
        if (activeFrameImg !== img) return;
        cancelFrameInFlight();
        if (isDemoMode) return;
        const now = performance.now();
        const dt = now - lastFrameTime;
        lastFrameTime = now;
        if (dt > 10 && dt < 2000) {
          rollingStreamFps = (rollingStreamFps * 0.75) + ((1000.0 / dt) * 0.25);
          document.getElementById('cam-fps-hud').innerText = Math.round(rollingStreamFps) + " FPS";
        }
        view.src = img.src;
        hasReceivedLiveFrame = true;
        consecutiveFailCount = 0;
        setConnectionState(true);
        scheduleNextFrame(65);
      };
      img.onerror = () => {
        if (activeFrameImg !== img) return;
        cancelFrameInFlight();
        if (isDemoMode) return;
        handleImageError();
        const retryDelay = consecutiveFailCount >= 3 ? 650 : 220;
        scheduleNextFrame(retryDelay);
      };
      img.src = '/capture?t=' + Date.now();
    }

    function handleImageError() {
      if (!isDemoMode) {
        consecutiveFailCount += 1;
        if (consecutiveFailCount >= 3 || !hasReceivedLiveFrame) {
          setConnectionState(false);
          if (!hasReceivedLiveFrame) {
            const rawLabel = (currentTelemetry && currentTelemetry.label) || 'none';
            document.getElementById('cam-feed').src = createSvgView(rawLabel, currentTelemetry ? currentTelemetry.weight : 0);
          }
        }
      }
    }

    function scheduleNextPoll(delayMs) {
      if (pollTimerId) {
        clearTimeout(pollTimerId);
        pollTimerId = null;
      }
      if (!isDemoMode) {
        pollTimerId = setTimeout(poll, delayMs);
      }
    }

    function poll() {
      if (isDemoMode) {
        return;
      }
      if (pollInFlight) {
        scheduleNextPoll(250);
        return;
      }
      pollInFlight = true;
      const ctrl = typeof AbortController !== 'undefined' ? new AbortController() : null;
      const timer = ctrl ? setTimeout(() => ctrl.abort(), 1800) : null;
      fetch('/status', ctrl ? { signal: ctrl.signal, cache: 'no-store' } : { cache: 'no-store' })
        .then(r => {
          if (timer) clearTimeout(timer);
          if (!r.ok) throw new Error('ESP32 offline');
          return r.json();
        })
        .then(d => {
          pollInFlight = false;
          if (isDemoMode) return;
          if (d && d.offline) {
            consecutiveFailCount += 1;
            if (consecutiveFailCount >= 3) {
              setConnectionState(false);
            }
            scheduleNextPoll(500);
            return;
          }
          consecutiveFailCount = 0;
          setConnectionState(true);
          renderData(d, false);
          scheduleNextPoll(350);
        })
        .catch(() => {
          if (timer) clearTimeout(timer);
          pollInFlight = false;
          if (isDemoMode) return;
          consecutiveFailCount += 1;
          if (consecutiveFailCount >= 3) {
            setConnectionState(false);
          }
          scheduleNextPoll(550);
        });
    }

    function triggerSpring(el) {
      if (!el) return;
      el.classList.remove('spring-pop');
      void el.offsetWidth;
      el.classList.add('spring-pop');
    }

    function renderData(d, skipAutoAdd) {
      if (!d) return;
      currentTelemetry = d;
      const rawLabel = (d.label || "none").toLowerCase();
      const isFruit = (rawLabel === "apple" || rawLabel === "orange" || rawLabel === "pear");
      const label = isFruit ? rawLabel : "none";
      const meta = FRUITS[label] || FRUITS.none;
      const massG = (d.weight !== undefined ? parseFloat(d.weight) : parseFloat(d.weight_g || 0)) || 0.0;
      const detected = (d.detected !== undefined) ? Boolean(d.detected) : (isFruit && massG >= 15.0);

      // Smart Auto-Add State Machine (`scale_was_empty` reset when weight_g < 15g)
      if (!skipAutoAdd) {
        if (massG < 15.0 || !isFruit || !detected) {
          scale_was_empty = true;
          stableReadingCount = 0;
        } else if (detected && massG >= 50.0) {
          const autoAddCheck = document.getElementById('auto-add-check');
          if (autoAddCheck && autoAddCheck.checked) {
            const threshold = (isDemoMode || !isLiveConnected) ? 1 : 2;
            if (scale_was_empty) {
              stableReadingCount += 1;
              if (stableReadingCount >= threshold) {
                addCurrentToCart(false);
              }
            } else if (Math.abs(massG - lastAutoAddedWeight) >= 35.0 || label !== lastAutoAddedLabel) {
              stableReadingCount += 1;
              if (stableReadingCount >= threshold) {
                addCurrentToCart(false);
              }
            }
          }
        }
      }

      // 1. Dynamic Reactive Fruit Glow Border & Accent
      const themeSet = PALETTES[currentTheme] || PALETTES.night;
      const fruitTheme = themeSet[label] || themeSet.none;
      document.documentElement.style.setProperty('--accent', fruitTheme.accent);
      document.documentElement.style.setProperty('--accent-glow', fruitTheme.glow);

      // 2. Spring Physics on Fruit Detection Change
      if (rawLabel !== prevFruitLabel) {
        prevFruitLabel = rawLabel;
        triggerSpring(document.getElementById('fruit-emoji'));
        triggerSpring(document.getElementById('target-icon'));
        triggerSpring(document.getElementById('val-total'));
      }

      // 3. Metrology & Pricing Calculation
      const basePricePerKg = isFruit ? (customPrices[label] || DEFAULT_PRICES[label] || 60000) : 0;
      const displayRate = convertRateForUnit(basePricePerKg, priceUnit);
      const totalDue = (massG / 1000.0) * basePricePerKg;

      document.getElementById('val-mass').innerText = massG.toFixed(1);
      document.getElementById('val-mass-kg').innerText = (massG / 1000.0).toFixed(3).replace('.', ',') + " kg";
      document.getElementById('val-total').innerText = totalDue > 0 ? formatVND(totalDue) : "0 ₫";

      const rateSuffix = priceUnit === 'g' ? '/ g' : (priceUnit === '100g' ? '/ 100g' : '/ kg');
      document.getElementById('val-unit-rate').innerHTML =
        `Đơn giá: ${formatVND(displayRate)} ${rateSuffix} <span class="edit-price-inline" onclick="togglePriceMenu()">[Sửa]</span>`;
      document.getElementById('val-tare-state').innerText = (isFruit && massG >= 15.0)
        ? `Đang cân ${meta.shortName} • Cảm biến HX711 ổn định`
        : "Đã trừ bì (Tare 0g) • Cảm biến HX711 sẵn sàng";

      const srcPill = document.getElementById('val-source-pill');
      if (srcPill) {
        srcPill.innerText = d.stm32_live ? "STM32 HX711 • Live" : "STM32 HX711 • Sim";
        srcPill.style.color = d.stm32_live ? "var(--green)" : "var(--amber)";
      }

      // 4. Viewfinder HUD
      const confVal = d.confidence !== undefined ? parseFloat(d.confidence) : (isFruit ? 0.94 : 0.0);
      document.getElementById('target-icon').innerText = meta.emoji;
      document.getElementById('target-name').innerText = isFruit ? meta.name : "Đang quét đĩa cân...";
      document.getElementById('target-conf').innerText = isFruit ? (confVal * 100).toFixed(0) + "%" : "-- %";

      const totalTime = (d.dsp_ms || 8) + (d.infer_ms || 42);
      const fps = totalTime > 0 ? (1000 / totalTime).toFixed(0) : "20";
      if (isDemoMode || (!isLiveConnected && !hasReceivedLiveFrame)) {
        document.getElementById('cam-fps-hud').innerText = fps + " FPS";
        document.getElementById('cam-feed').src = createSvgView(label, massG);
      }

      // 5. Fruit Variety Status Strip
      document.getElementById('fruit-emoji').innerText = meta.emoji;
      document.getElementById('fruit-title').innerText = isFruit ? meta.name : "Cân trống";
      const votes = d.votes !== undefined ? d.votes : (isFruit ? 3 : 0);
      const burstTotal = d.burst_total || 3;
      const pct = Math.min(100, Math.round((votes / Math.max(1, burstTotal)) * 100));
      document.getElementById('fruit-sub').innerText = isFruit
        ? `Đồng thuận ${votes}/${burstTotal} (${pct}%) • Đã nhận diện chuẩn xác ✨`
        : "Đĩa cân trống (< 15g) • Sẵn sàng đặt quả tiếp theo";
      document.getElementById('consensus-bar').style.width = isFruit ? pct + "%" : "0%";

      // 6. Hardware Telemetry Grid
      const inferVal = d.infer_ms || 42;
      const dspVal = d.dsp_ms || 8;
      const tempVal = d.temp ? Number(d.temp).toFixed(1) : "48.2";
      const psramVal = d.psram_kb || 3840;

      document.getElementById('t-infer').innerText = inferVal + " ms";
      document.getElementById('t-dsp').innerText = dspVal + " ms";
      document.getElementById('t-fps').innerText = fps + " FPS";
      document.getElementById('t-votes').innerText = `${votes}/${burstTotal}`;
      document.getElementById('t-temp').innerText = tempVal + " °C";
      document.getElementById('t-psram').innerText = psramVal + " KB";

      document.getElementById('bar-infer').style.width = Math.min(100, Math.round((inferVal / 80) * 100)) + "%";
      document.getElementById('bar-dsp').style.width = Math.min(100, Math.round((dspVal / 25) * 100)) + "%";
      document.getElementById('bar-fps').style.width = Math.min(100, Math.round((parseFloat(fps) / 25) * 100)) + "%";
      document.getElementById('bar-votes').style.width = pct + "%";
      document.getElementById('bar-temp').style.width = Math.min(100, Math.round((parseFloat(tempVal) / 75) * 100)) + "%";
      document.getElementById('bar-psram').style.width = Math.min(100, Math.round((psramVal / 4096) * 100)) + "%";
    }

    function setConnectionState(connected) {
      isLiveConnected = connected;
      const dot = document.getElementById('conn-dot');
      const text = document.getElementById('conn-status');
      if (connected) {
        dot.style.background = "var(--green)";
        dot.style.boxShadow = "0 0 8px var(--green)";
        text.innerText = "ESP32 Live";
      } else {
        dot.style.background = "var(--amber)";
        dot.style.boxShadow = "0 0 8px var(--amber)";
        text.innerText = isDemoMode ? "Mô phỏng" : "Offline • Sẵn sàng";
      }
    }

    function toggleDemo(enabled) {
      isDemoMode = enabled;
      if (enabled) {
        if (frameTimerId) { clearTimeout(frameTimerId); frameTimerId = null; }
        if (pollTimerId) { clearTimeout(pollTimerId); pollTimerId = null; }
        cancelFrameInFlight();
        hasReceivedLiveFrame = false;
        setConnectionState(false);
        if (demoIndex === 3) demoIndex = 0;
        scale_was_empty = true;
        renderData(DEMO_SAMPLES[demoIndex], false);
      } else {
        consecutiveFailCount = 0;
        updateFrame();
        poll();
      }
    }

    function injectSampleFruit(label, weightG) {
      isDemoMode = true;
      if (frameTimerId) { clearTimeout(frameTimerId); frameTimerId = null; }
      if (pollTimerId) { clearTimeout(pollTimerId); pollTimerId = null; }
      cancelFrameInFlight();
      hasReceivedLiveFrame = false;
      const chk = document.getElementById('demo-check');
      if (chk) chk.checked = true;
      setConnectionState(false);

      const idxMap = { apple: 0, orange: 1, pear: 2, none: 3 };
      if (idxMap[label] !== undefined) {
        demoIndex = idxMap[label];
      }

      // Play laser scan animation
      playLaserSweep();

      if (label === 'none' || weightG < 15.0) {
        renderData({
          label: 'none',
          detected: false,
          confidence: 0.0,
          weight: 0.0,
          votes: 0,
          burst_total: 3,
          dsp_ms: 7,
          infer_ms: 38,
          temp: 47.5,
          psram_kb: 3840,
          stm32_live: false
        }, false);
        showToast('⚖️ Đã nhấc quả khỏi bàn cân (0.0g) • Sẵn sàng cân món tiếp theo');
        return;
      }

      // Reset scale_was_empty so clicking sample fruit buttons always simulates placing a new fruit on the scale
      scale_was_empty = true;
      renderData({
        label: label,
        detected: true,
        confidence: 0.95,
        weight: weightG,
        votes: 3,
        burst_total: 3,
        dsp_ms: 8,
        infer_ms: 41,
        temp: 48.4,
        psram_kb: 3840,
        stm32_live: true
      }, false);
    }

    function playLaserSweep() {
      const fx = document.getElementById('shutter-fx');
      if (fx) {
        fx.classList.add('active');
        setTimeout(() => fx.classList.remove('active'), 110);
      }
      const vf = document.getElementById('viewfinder-box');
      const laser = document.getElementById('ai-scan-laser');
      if (laser) {
        laser.classList.remove('scanning');
        void laser.offsetWidth;
        laser.classList.add('scanning');
        setTimeout(() => laser.classList.remove('scanning'), 850);
      }
      if (vf) {
        vf.classList.add('scanning');
        setTimeout(() => vf.classList.remove('scanning'), 850);
      }
      const cells = document.querySelectorAll('.telem-cell');
      cells.forEach(c => c.classList.add('flash-update'));
      setTimeout(() => cells.forEach(c => c.classList.remove('flash-update')), 350);
    }

    function triggerInference() {
      playLaserSweep();
      if (isDemoMode || !isLiveConnected) {
        demoIndex = (demoIndex + 1) % DEMO_SAMPLES.length;
        scale_was_empty = true;
        renderData(DEMO_SAMPLES[demoIndex], false);
      } else {
        fetch('/trigger').catch(() => {});
        scheduleNextPoll(220);
      }
    }

    function cycleWeight() {
      if (isDemoMode || !isLiveConnected) {
        demoIndex = (demoIndex + 1) % DEMO_SAMPLES.length;
        scale_was_empty = true;
        renderData(DEMO_SAMPLES[demoIndex], false);
      } else {
        fetch('/cycle_weight').catch(() => {});
        scheduleNextPoll(200);
      }
    }

    function toggleFlash() {
      flashActive = !flashActive;
      const btn = document.getElementById('btn-flash');
      const lbl = document.getElementById('lbl-flash');
      btn.classList.toggle('flash-active', flashActive);
      lbl.innerText = flashActive ? "Đèn BẬT" : "Đèn Flash";
      if (!isDemoMode) fetch('/flash').catch(() => {});
    }

    // ==========================================================================
    // Initialization
    // ==========================================================================
    (function init() {
      try {
        const savedTheme = localStorage.getItem('scale_rice_theme');
        if (savedTheme === 'day' || savedTheme === 'night') {
          applyTheme(savedTheme);
        }
      } catch (e) {}
      syncPriceInputs();
      renderData(currentTelemetry, true);
      fetchBackendState();
      poll();
      updateFrame();
    })();
  </script>
</body>
</html>
"""


# ----------------------------------------------------------------------------
# 4. Serialized ESP32 Connection Pool & Local Python ThreadingHTTPServer
# ----------------------------------------------------------------------------
class ESP32BridgeManager:
    """
    Serializes and pools all HTTP requests to the single-core/single-socket-task
    ESP32-CAM `esp_http_server` so `/capture`, `/status`, and `/trigger` NEVER
    collide concurrently or exhaust lwIP sockets in `TIME_WAIT`.
    Also maintains a fast RAM cache of the latest JPEG frame and telemetry JSON
    so momentary Wi-Fi or AI-burst jitter never drops the POS camera stream.
    """

    def __init__(self):
        self.io_lock = threading.Lock()
        self.state_lock = threading.Lock()
        self._conn = None
        self._conn_host = None
        self._conn_port = 80

        # In-memory cache for zero-drop smooth streaming
        self.last_jpeg = None
        self.last_jpeg_ts = 0.0
        self.last_status_bytes = None
        self.last_status_ts = 0.0
        self.consecutive_errors = 0

    def _parse_host_port(self, raw_ip):
        ip_str = str(raw_ip or "192.168.4.1").strip()
        if ip_str.startswith("http://"):
            ip_str = ip_str[7:]
        elif ip_str.startswith("https://"):
            ip_str = ip_str[8:]
        ip_str = ip_str.split("/")[0].strip() or "192.168.4.1"
        if ":" in ip_str:
            host, port_s = ip_str.rsplit(":", 1)
            try:
                return host.strip() or "192.168.4.1", int(port_s)
            except ValueError:
                return host.strip() or "192.168.4.1", 80
        return ip_str, 80

    def _close_conn_unlocked(self):
        if self._conn is not None:
            try:
                self._conn.close()
            except Exception:
                pass
            self._conn = None

    def reset_target(self):
        with self.io_lock:
            self._close_conn_unlocked()
            self._conn_host = None
            self._conn_port = 80
        with self.state_lock:
            self.last_jpeg = None
            self.last_jpeg_ts = 0.0
            self.last_status_bytes = None
            self.last_status_ts = 0.0
            self.consecutive_errors = 0

    def _request_serialized(self, path_with_query, timeout=2.0):
        host, port = self._parse_host_port(STORAGE.esp_ip)
        headers = {
            "User-Agent": "SmartScalePOS-HyprlandRice/2.0",
            "Connection": "keep-alive",
            "Accept": "*/*",
        }

        with self.io_lock:
            for attempt in range(2):
                try:
                    if self._conn is None or self._conn_host != host or self._conn_port != port:
                        self._close_conn_unlocked()
                        self._conn_host = host
                        self._conn_port = port
                        self._conn = http.client.HTTPConnection(host, port, timeout=timeout)

                    self._conn.timeout = timeout
                    if getattr(self._conn, "sock", None) is not None:
                        try:
                            self._conn.sock.settimeout(timeout)
                            self._conn.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                        except Exception:
                            pass

                    self._conn.request("GET", path_with_query, headers=headers)
                    if getattr(self._conn, "sock", None) is not None:
                        try:
                            self._conn.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                        except Exception:
                            pass

                    resp = self._conn.getresponse()
                    data = resp.read()
                    status = resp.status
                    content_type = resp.getheader("Content-Type", "application/octet-stream")
                    conn_hdr = (resp.getheader("Connection") or "").lower()
                    if "close" in conn_hdr or resp.will_close:
                        self._close_conn_unlocked()

                    with self.state_lock:
                        self.consecutive_errors = 0
                    return status, content_type, data, None
                except Exception as e:
                    self._close_conn_unlocked()
                    if attempt == 0:
                        continue
                    with self.state_lock:
                        self.consecutive_errors += 1
                    return None, None, None, e

    def fetch_capture(self, path_with_query):
        now = time.monotonic()
        with self.state_lock:
            # Coalesce bursts if another request refreshed the frame within the last 35ms
            if self.last_jpeg and (now - self.last_jpeg_ts) < 0.035:
                return 200, "image/jpeg", self.last_jpeg

        status, ctype, data, err = self._request_serialized(path_with_query, timeout=2.2)
        if status == 200 and data and len(data) > 64:
            with self.state_lock:
                self.last_jpeg = data
                self.last_jpeg_ts = time.monotonic()
            return 200, ctype or "image/jpeg", data

        # Serve cached frame for up to 3.5 seconds during transient Wi-Fi / AI burst stalls
        with self.state_lock:
            if self.last_jpeg and (time.monotonic() - self.last_jpeg_ts) < 3.5 and self.consecutive_errors < 6:
                return 200, "image/jpeg", self.last_jpeg

        return 503, "text/plain; charset=utf-8", b"ESP32 Offline"

    def fetch_status(self, path_with_query):
        now = time.monotonic()
        with self.state_lock:
            # Coalesce rapid status polls within 150ms
            if self.last_status_bytes and (now - self.last_status_ts) < 0.15:
                return 200, "application/json; charset=utf-8", self.last_status_bytes

        status, ctype, data, err = self._request_serialized(path_with_query, timeout=1.6)
        if status == 200 and data:
            with self.state_lock:
                self.last_status_bytes = data
                self.last_status_ts = time.monotonic()
            return 200, ctype or "application/json; charset=utf-8", data

        # Serve cached status for up to 3.0 seconds during transient camera burst locks
        with self.state_lock:
            if self.last_status_bytes and (time.monotonic() - self.last_status_ts) < 3.0 and self.consecutive_errors < 5:
                return 200, "application/json; charset=utf-8", self.last_status_bytes

        err_payload = json.dumps(
            {"ok": False, "offline": True, "error": str(err or f"HTTP {status}")},
            ensure_ascii=False,
        ).encode("utf-8")
        return 503, "application/json; charset=utf-8", err_payload

    def fetch_command(self, path_with_query):
        status, ctype, data, err = self._request_serialized(path_with_query, timeout=2.0)
        if path_with_query.startswith("/cycle_weight") and status == 404:
            status, ctype, data, err = self._request_serialized("/trigger", timeout=2.0)
        if status and 200 <= status < 400 and data is not None:
            return status, ctype or "text/plain; charset=utf-8", data
        return 503, "text/plain; charset=utf-8", b"ESP32 Offline"


ESP32_BRIDGE = ESP32BridgeManager()


class ThreadingHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = (sys.platform != "win32")


class POSRequestHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        # Keep console output clean; suppress high-frequency polling logs
        pass

    def _safe_write(self, data):
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, socket.error):
            pass

    def _send_json(self, status_code, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        try:
            self.send_response(status_code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self._safe_write(body)
        except Exception:
            pass

    def _read_json_body(self):
        length = _safe_int(self.headers.get("Content-Length", 0), 0)
        if length <= 0:
            return {}
        raw = self.rfile.read(min(length, 5 * 1024 * 1024))
        try:
            parsed = json.loads(raw.decode("utf-8"))
            return parsed if isinstance(parsed, dict) else {}
        except Exception:
            return {}

    def _proxy_to_esp32(self, esp_path_with_query):
        if esp_path_with_query.startswith("/capture"):
            status, content_type, data = ESP32_BRIDGE.fetch_capture(esp_path_with_query)
        elif esp_path_with_query.startswith("/status"):
            status, content_type, data = ESP32_BRIDGE.fetch_status(esp_path_with_query)
        else:
            status, content_type, data = ESP32_BRIDGE.fetch_command(esp_path_with_query)

        try:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self._safe_write(data)
        except Exception:
            pass

    def do_OPTIONS(self):
        try:
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")
            self.end_headers()
        except Exception:
            pass

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        route = parsed.path

        # 1. Serve Hyprland / Catppuccin Linux Rice POS UI
        if route in ("/", "/index.html"):
            body = POS_RICE_HTML.encode("utf-8")
            try:
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self._safe_write(body)
            except Exception:
                pass
            return

        # 2. Local Disk State & History API
        if route == "/api/state":
            self._send_json(200, STORAGE.get_state_dict())
            return

        if route == "/api/open_csv":
            result = STORAGE.open_csv_on_pc()
            self._send_json(200, result)
            return

        # 3. Proxy ESP32-CAM Endpoints (/capture, /status, /trigger, /flash, /cycle_weight, /set_prices)
        if route in ("/capture", "/status", "/trigger", "/flash", "/cycle_weight", "/set_prices"):
            # If /set_prices is called, also update local prices in pos_state.json
            if route == "/set_prices" and parsed.query:
                qs = urllib.parse.parse_qs(parsed.query)
                new_prices = {}
                for k in ("apple", "orange", "pear"):
                    if k in qs and qs[k]:
                        val = _safe_float(qs[k][0], -1.0)
                        if val >= 0:
                            new_prices[k] = val
                if new_prices:
                    STORAGE.update_settings({"prices": new_prices})
            self._proxy_to_esp32(self.path)
            return

        self._send_json(404, {"ok": False, "error": f"Not found: {route}"})

    def do_POST(self):
        try:
            parsed = urllib.parse.urlparse(self.path)
            route = parsed.path
            payload = self._read_json_body()

            if route == "/api/state":
                res = STORAGE.update_settings(payload)
                self._send_json(200 if res.get("ok") else 400, res)
                return

            if route == "/api/checkout":
                res = STORAGE.checkout_order(payload)
                status = 200 if res.get("ok") else 400
                self._send_json(status, res)
                return

            if route == "/api/delete_order":
                order_id = payload.get("order_id")
                order_no = payload.get("order_no")
                res = STORAGE.delete_order(order_id=order_id, order_no=order_no)
                self._send_json(200, res)
                return

            if route == "/api/clear_history":
                reset_no = payload.get("reset_order_no", True)
                res = STORAGE.clear_history(reset_order_no=bool(reset_no))
                self._send_json(200, res)
                return

            if route == "/api/open_csv":
                res = STORAGE.open_csv_on_pc()
                self._send_json(200, res)
                return

            self._send_json(404, {"ok": False, "error": f"Unknown endpoint: {route}"})
        except Exception as e:
            self._send_json(500, {"ok": False, "error": str(e)})


# ----------------------------------------------------------------------------
# 5. Native Window Launcher (pywebview -> msedge --app -> chrome --app)
# ----------------------------------------------------------------------------
def find_chromium_app_browser():
    """Finds Microsoft Edge or Google Chrome executable for frameless --app mode."""
    candidates = [
        shutil.which("msedge"),
        shutil.which("msedge.exe"),
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
        os.path.expandvars(r"%LocalAppData%\Microsoft\Edge\Application\msedge.exe"),
        shutil.which("chrome"),
        shutil.which("chrome.exe"),
        shutil.which("google-chrome"),
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
        os.path.expandvars(r"%LocalAppData%\Google\Chrome\Application\chrome.exe"),
    ]
    for path in candidates:
        if path and os.path.isfile(path):
            return path
    return None


def is_port_in_use(host, port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(0.3)
        return s.connect_ex((host, port)) == 0


def start_background_server(host="127.0.0.1", port=8765):
    server = ThreadingHTTPServer((host, port), POSRequestHandler)
    thread = threading.Thread(target=server.serve_forever, name="POS-HTTP-Server", daemon=True)
    thread.start()
    return server


def launch_native_window(url, title="Smart Scale POS — Hyprland Rice Edition"):
    """
    1. First tries `pywebview` (`import webview`) to open a native OS window.
    2. Falls back to Microsoft Edge or Chrome in frameless App Mode (`--app=...`).
    3. Final fallback opens default system browser.
    """
    try:
        import webview  # type: ignore
        print("[INFO] Launching native desktop window via pywebview...")
        webview.create_window(title, url, width=1280, height=860, min_size=(960, 680))
        webview.start()
        return "pywebview"
    except ImportError:
        pass
    except Exception as e:
        print(f"[WARN] pywebview initialization failed ({e}), falling back to Chromium App Mode...")

    browser_exe = find_chromium_app_browser()
    if browser_exe:
        browser_name = os.path.basename(browser_exe)
        print(f"[INFO] Launching frameless native App window via {browser_name} (--app={url})...")
        try:
            subprocess.Popen([
                browser_exe,
                f"--app={url}",
                "--window-size=1280,860",
            ])
            return "app_browser"
        except Exception as e:
            print(f"[WARN] Failed launching {browser_exe}: {e}")

    print(f"[INFO] Opening {url} in default system browser...")
    webbrowser.open(url)
    return "webbrowser"


def main():
    parser = argparse.ArgumentParser(description="Smart Scale POS — Hyprland Rice Desktop App")
    parser.add_argument("--host", default="127.0.0.1", help="Local backend host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=8765, help="Local backend port (default: 8765)")
    parser.add_argument("--headless", action="store_true", help="Run HTTP backend server without launching GUI window")
    args = parser.parse_args()

    host = args.host
    port = args.port
    url = f"http://{host}:{port}"

    print("=" * 78)
    print("  SMART SCALE POS — HYPRLAND / CATPPUCCIN LINUX RICE EDITION")
    print("=" * 78)
    print(f"  [Backend URL]   {url}")
    print(f"  [ESP32 Target]  http://{STORAGE.esp_ip}")
    print(f"  [CSV History]   {CSV_FILE}")
    print(f"  [State Config]  {STATE_FILE}")
    print(f"  [Active Order]  Đơn #{STORAGE.next_order_no} (ORD-{STORAGE.next_order_no:04d})")
    print("=" * 78)

    if is_port_in_use(host, port):
        print(f"[INFO] Port {port} is already active! Connecting window to running server at {url}...")
        if not args.headless:
            launch_native_window(url)
        return

    server = start_background_server(host, port)

    if args.headless:
        print("[INFO] Running in --headless mode. Press Ctrl+C to stop.")
        try:
            while True:
                time.sleep(0.5)
        except KeyboardInterrupt:
            print("\n[INFO] Shutting down server...")
            server.shutdown()
        return

    mode = launch_native_window(url)
    if mode == "pywebview":
        # pywebview.start() blocks until the native window is closed
        server.shutdown()
        return

    print("\n  [POS SERVER ACTIVE] Keep this terminal open while using the POS window.")
    print("  Press Ctrl+C here at any time to stop the local POS server.\n")
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\n[INFO] Shutting down POS server...")
        server.shutdown()


if __name__ == "__main__":
    main()
