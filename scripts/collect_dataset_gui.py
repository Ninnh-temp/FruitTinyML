"""
Edge AI Smart Retail Scale - Dataset Collection Tool
-----------------------------------------------------
Connects wirelessly to the ESP32-CAM HTTP server to preview and capture
labeled training images for Edge Impulse / TinyML model training.

Runs on PC with Python (tkinter + Pillow). No external pip packages required.
"""

import os
import sys
import time
import threading
import urllib.request
import urllib.error
import io
import tkinter as tk
from tkinter import ttk, messagebox
from PIL import Image, ImageTk

# Default Settings
DEFAULT_ESP_IP = "192.168.4.1"  # Default SoftAP IP (use router IP if on home Wi-Fi)
DEFAULT_CLASSES = ["apple", "banana", "orange", "empty_scale"]
OUTPUT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "dataset"))


class DatasetCollectorApp:
    def __init__(self, root):
        self.root = root
        self.root.title("TinyFruitML - Dataset Collector (ESP32-CAM)")
        self.root.geometry("980x720")
        self.root.minsize(850, 600)
        self.root.configure(bg="#0f172a")

        self.esp_ip = tk.StringVar(value=DEFAULT_ESP_IP)
        self.current_label = tk.StringVar(value=DEFAULT_CLASSES[0])
        self.custom_label = tk.StringVar()
        self.status_text = tk.StringVar(value="Ready. Connect to ESP32 Wi-Fi.")
        self.is_streaming = False
        self.is_auto_capturing = False
        self.auto_interval_sec = tk.DoubleVar(value=1.5)
        self.capture_count = 0
        self.latest_image_bytes = None

        # Lock for stream thread
        self.stream_thread = None
        self.auto_thread = None
        self.stop_events = threading.Event()

        os.makedirs(OUTPUT_DIR, exist_ok=True)
        self.build_ui()
        self.refresh_stats()
        self.bind_shortcuts()

    def build_ui(self):
        style = ttk.Style()
        style.theme_use("clam")
        style.configure("TLabel", background="#0f172a", foreground="#f8fafc", font=("Segoe UI", 10))
        style.configure("Header.TLabel", font=("Segoe UI", 14, "bold"), foreground="#38bdf8")
        style.configure("SubHeader.TLabel", font=("Segoe UI", 9), foreground="#94a3b8")
        style.configure("TButton", font=("Segoe UI", 10, "bold"), padding=6)

        # Main Layout: Left = Stream & Controls, Right = Stats & Class Manager
        main_frame = tk.Frame(self.root, bg="#0f172a")
        main_frame.pack(fill=tk.BOTH, expand=True, padx=16, pady=16)

        # Top Bar: Connection & IP
        top_bar = tk.Frame(main_frame, bg="#1e293b", bd=1, relief=tk.SOLID, padx=12, pady=10)
        top_bar.pack(fill=tk.X, pady=(0, 12))

        ttk.Label(top_bar, text="ESP32 URL:", background="#1e293b").pack(side=tk.LEFT, padx=(0, 6))
        ip_entry = tk.Entry(top_bar, textvariable=self.esp_ip, width=16, font=("Segoe UI", 10), bg="#0f172a", fg="#38bdf8", insertbackground="white")
        ip_entry.pack(side=tk.LEFT, padx=(0, 10))

        self.btn_connect = tk.Button(
            top_bar, text="▶ Start Live Preview", command=self.toggle_stream,
            bg="#0284c7", fg="white", font=("Segoe UI", 9, "bold"), relief=tk.FLAT, padx=10
        )
        self.btn_connect.pack(side=tk.LEFT, padx=(0, 8))

        self.btn_flash = tk.Button(
            top_bar, text="💡 Toggle Flash LED", command=self.toggle_flash,
            bg="#475569", fg="white", font=("Segoe UI", 9), relief=tk.FLAT, padx=10
        )
        self.btn_flash.pack(side=tk.LEFT, padx=(0, 8))

        self.btn_open_folder = tk.Button(
            top_bar, text="📁 Open Dataset Folder", command=self.open_dataset_folder,
            bg="#334155", fg="#cbd5e1", font=("Segoe UI", 9), relief=tk.FLAT, padx=10
        )
        self.btn_open_folder.pack(side=tk.RIGHT)

        # Content Split: Viewport (Left) and Sidebar (Right)
        content_frame = tk.Frame(main_frame, bg="#0f172a")
        content_frame.pack(fill=tk.BOTH, expand=True)

        # Left Column: Camera View + Capture Buttons
        left_col = tk.Frame(content_frame, bg="#0f172a")
        left_col.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(0, 12))

        # Viewport Frame
        self.view_container = tk.Frame(left_col, bg="#000000", bd=2, relief=tk.SUNKEN)
        self.view_container.pack(fill=tk.BOTH, expand=True)

        self.lbl_viewport = tk.Label(
            self.view_container, text="Camera Offline\nClick 'Start Live Preview' above",
            bg="#000000", fg="#64748b", font=("Segoe UI", 12)
        )
        self.lbl_viewport.pack(fill=tk.BOTH, expand=True)

        # Bottom Action Bar
        action_bar = tk.Frame(left_col, bg="#1e293b", padx=12, pady=10)
        action_bar.pack(fill=tk.X, pady=(10, 0))

        # Big Capture Button
        self.btn_capture = tk.Button(
            action_bar, text="📸 CAPTURE PHOTO  [Spacebar]",
            command=self.capture_manual,
            bg="#16a34a", fg="white", font=("Segoe UI", 12, "bold"),
            relief=tk.FLAT, height=2, cursor="hand2"
        )
        self.btn_capture.pack(fill=tk.X, pady=(0, 8))

        # Auto-Capture Interval Bar
        auto_bar = tk.Frame(action_bar, bg="#1e293b")
        auto_bar.pack(fill=tk.X)

        self.chk_auto = tk.Checkbutton(
            auto_bar, text="Auto-Capture Interval:", variable=self.is_auto_capturing,
            command=self.toggle_auto_capture, bg="#1e293b", fg="#f8fafc",
            selectcolor="#0f172a", activebackground="#1e293b", activeforeground="#f8fafc",
            font=("Segoe UI", 9, "bold")
        )
        self.chk_auto.pack(side=tk.LEFT)

        interval_spin = tk.Spinbox(
            auto_bar, from_=0.5, to=10.0, increment=0.5, textvariable=self.auto_interval_sec,
            width=5, font=("Segoe UI", 9), bg="#0f172a", fg="white"
        )
        interval_spin.pack(side=tk.LEFT, padx=6)
        ttk.Label(auto_bar, text="sec (rotate fruit on scale)", background="#1e293b", foreground="#94a3b8").pack(side=tk.LEFT)

        # Right Column: Classes & Dataset Stats
        right_col = tk.Frame(content_frame, bg="#1e293b", width=300, padx=14, pady=12)
        right_col.pack(side=tk.RIGHT, fill=tk.Y)
        right_col.pack_propagate(False)

        ttk.Label(right_col, text="Active Class Label", style="Header.TLabel", background="#1e293b").pack(anchor=tk.W)
        ttk.Label(right_col, text="Select fruit variety to tag photos:", style="SubHeader.TLabel", background="#1e293b").pack(anchor=tk.W, pady=(0, 8))

        # Preset class radio buttons
        self.class_buttons_frame = tk.Frame(right_col, bg="#1e293b")
        self.class_buttons_frame.pack(fill=tk.X, pady=(0, 8))
        self.rebuild_class_radios()

        # Add Custom Class
        add_frame = tk.Frame(right_col, bg="#1e293b")
        add_frame.pack(fill=tk.X, pady=(0, 14))
        entry_custom = tk.Entry(add_frame, textvariable=self.custom_label, width=12, font=("Segoe UI", 9), bg="#0f172a", fg="white")
        entry_custom.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(0, 6))
        btn_add = tk.Button(add_frame, text="+ Add", command=self.add_custom_class, bg="#334155", fg="white", font=("Segoe UI", 8, "bold"), relief=tk.FLAT)
        btn_add.pack(side=tk.RIGHT)

        # Dataset Stats Box
        ttk.Label(right_col, text="Dataset Statistics", style="Header.TLabel", background="#1e293b").pack(anchor=tk.W, pady=(8, 4))
        self.stats_tree = ttk.Treeview(right_col, columns=("class", "count"), show="headings", height=8)
        self.stats_tree.heading("class", text="Class")
        self.stats_tree.heading("count", text="Samples")
        self.stats_tree.column("class", width=140, anchor=tk.W)
        self.stats_tree.column("count", width=70, anchor=tk.CENTER)
        self.stats_tree.pack(fill=tk.BOTH, expand=True, pady=(0, 8))

        # Refresh stats button
        btn_refresh = tk.Button(right_col, text="🔄 Refresh Counts", command=self.refresh_stats, bg="#334155", fg="#cbd5e1", font=("Segoe UI", 8), relief=tk.FLAT)
        btn_refresh.pack(fill=tk.X)

        # Status Bar at bottom
        self.lbl_status = tk.Label(self.root, textvariable=self.status_text, bg="#0284c7", fg="white", font=("Segoe UI", 9), anchor=tk.W, padx=12, pady=4)
        self.lbl_status.pack(side=tk.BOTTOM, fill=tk.X)

    def bind_shortcuts(self):
        self.root.bind("<space>", lambda e: self.capture_manual())

    def rebuild_class_radios(self):
        for widget in self.class_buttons_frame.winfo_children():
            widget.destroy()

        for c in DEFAULT_CLASSES:
            rb = tk.Radiobutton(
                self.class_buttons_frame, text=c.capitalize(), value=c,
                variable=self.current_label, bg="#1e293b", fg="#f8fafc",
                selectcolor="#0f172a", activebackground="#1e293b", activeforeground="#38bdf8",
                font=("Segoe UI", 10, "bold"), pady=3
            )
            rb.pack(anchor=tk.W)

    def add_custom_class(self):
        val = self.custom_label.get().strip().lower().replace(" ", "_")
        if val and val not in DEFAULT_CLASSES:
            DEFAULT_CLASSES.append(val)
            self.current_label.set(val)
            self.custom_label.set("")
            self.rebuild_class_radios()
            self.refresh_stats()

    def get_url(self, endpoint):
        ip = self.esp_ip.get().strip()
        if not ip.startswith("http"):
            ip = f"http://{ip}"
        return f"{ip.rstrip('/')}/{endpoint.lstrip('/')}"

    def toggle_stream(self):
        if not self.is_streaming:
            self.is_streaming = True
            self.stop_events.clear()
            self.btn_connect.config(text="⏹ Stop Preview", bg="#dc2626")
            self.status_text.set("Connecting to camera stream...")
            self.stream_thread = threading.Thread(target=self._stream_loop, daemon=True)
            self.stream_thread.start()
        else:
            self.is_streaming = False
            self.stop_events.set()
            self.btn_connect.config(text="▶ Start Live Preview", bg="#0284c7")
            self.status_text.set("Live preview stopped.")

    def _stream_loop(self):
        """Continuously pulls single frames from /capture for high reliability and zero lag."""
        fail_count = 0
        while self.is_streaming and not self.stop_events.is_set():
            url = self.get_url("capture?t=" + str(int(time.time() * 1000)))
            try:
                req = urllib.request.Request(url, headers={"User-Agent": "TinyFruitML-Collector/1.0"})
                with urllib.request.urlopen(req, timeout=3.0) as resp:
                    raw_data = resp.read()

                self.latest_image_bytes = raw_data
                fail_count = 0

                # Render to GUI in main thread
                img = Image.open(io.BytesIO(raw_data))

                # Resize image proportionally to fit viewport
                vp_width = max(self.view_container.winfo_width(), 320)
                vp_height = max(self.view_container.winfo_height(), 240)
                img.thumbnail((vp_width, vp_height), Image.Resampling.LANCZOS)
                tk_img = ImageTk.PhotoImage(img)

                def update_view(image_ref=tk_img):
                    if self.is_streaming:
                        self.lbl_viewport.config(image=image_ref, text="")
                        self.lbl_viewport.image = image_ref

                self.root.after(0, update_view)
                time.sleep(0.04)  # ~25 FPS max client throttle

            except Exception as e:
                fail_count += 1
                if fail_count > 5:
                    self.root.after(0, lambda: self.status_text.set(f"Stream error: Cannot reach {url}. Check Wi-Fi."))
                    time.sleep(1.0)
                else:
                    time.sleep(0.2)

    def capture_manual(self):
        """Captures the current frame and saves to dataset."""
        label = self.current_label.get().strip().lower()
        if not label:
            messagebox.showwarning("Warning", "Please select or type a class label first.")
            return

        # Visual feedback: flash viewport green briefly
        orig_bg = self.view_container.cget("bg")
        self.view_container.config(bg="#22c55e")
        self.root.after(100, lambda: self.view_container.config(bg=orig_bg))

        # Run capture in background thread so GUI doesn't freeze
        threading.Thread(target=self._save_capture_task, args=(label,), daemon=True).start()

    def _save_capture_task(self, label):
        class_dir = os.path.join(OUTPUT_DIR, label)
        os.makedirs(class_dir, exist_ok=True)

        img_bytes = None
        # If we have a fresh preview frame, use it or request a dedicated snapshot
        try:
            url = self.get_url("capture?t=" + str(int(time.time() * 1000)))
            req = urllib.request.Request(url, headers={"User-Agent": "TinyFruitML-Collector/1.0"})
            with urllib.request.urlopen(req, timeout=4.0) as resp:
                img_bytes = resp.read()
        except Exception:
            # Fall back to latest cached preview frame if direct fetch times out
            img_bytes = self.latest_image_bytes

        if not img_bytes:
            self.root.after(0, lambda: self.status_text.set("Error: Failed to capture image from ESP32."))
            return

        timestamp = time.strftime("%Y%m%d_%H%M%S")
        millis = int((time.time() % 1) * 1000)
        filename = f"{label}_{timestamp}_{millis:03d}.jpg"
        filepath = os.path.join(class_dir, filename)

        try:
            with open(filepath, "wb") as f:
                f.write(img_bytes)

            self.capture_count += 1
            self.root.after(0, lambda: self._on_save_success(label, filename))
        except Exception as e:
            self.root.after(0, lambda: self.status_text.set(f"Save error: {e}"))

    def _on_save_success(self, label, filename):
        self.status_text.set(f"Saved: {label}/{filename} (Total captured: {self.capture_count})")
        self.refresh_stats()

    def toggle_auto_capture(self):
        if self.is_auto_capturing:
            self.status_text.set(f"Auto-capture active: snapping every {self.auto_interval_sec.get()}s")
            self.auto_thread = threading.Thread(target=self._auto_capture_loop, daemon=True)
            self.auto_thread.start()
        else:
            self.status_text.set("Auto-capture disabled.")

    def _auto_capture_loop(self):
        while self.is_auto_capturing and not self.stop_events.is_set():
            self.capture_manual()
            interval = max(0.5, self.auto_interval_sec.get())
            time.sleep(interval)

    def toggle_flash(self):
        threading.Thread(target=self._flash_task, daemon=True).start()

    def _flash_task(self):
        url = self.get_url("flash")
        try:
            req = urllib.request.Request(url)
            with urllib.request.urlopen(req, timeout=2.0) as resp:
                txt = resp.read().decode("utf-8").strip()
            self.root.after(0, lambda: self.status_text.set(f"Flash LED state: {txt}"))
        except Exception as e:
            self.root.after(0, lambda: self.status_text.set(f"Flash toggle error: {e}"))

    def refresh_stats(self):
        for item in self.stats_tree.get_children():
            self.stats_tree.delete(item)

        total_samples = 0
        if os.path.exists(OUTPUT_DIR):
            classes = sorted(list(set(DEFAULT_CLASSES + os.listdir(OUTPUT_DIR))))
            for c in classes:
                c_path = os.path.join(OUTPUT_DIR, c)
                if os.path.isdir(c_path):
                    count = len([f for f in os.listdir(c_path) if f.lower().endswith(('.jpg', '.jpeg', '.png'))])
                else:
                    count = 0
                self.stats_tree.insert("", tk.END, values=(c, count))
                total_samples += count

        self.stats_tree.insert("", tk.END, values=("--- TOTAL ---", total_samples))

    def open_dataset_folder(self):
        if not os.path.exists(OUTPUT_DIR):
            os.makedirs(OUTPUT_DIR, exist_ok=True)
        if sys.platform == "win32":
            os.startfile(OUTPUT_DIR)
        elif sys.platform == "darwin":
            os.system(f"open '{OUTPUT_DIR}'")
        else:
            os.system(f"xdg-open '{OUTPUT_DIR}'")


if __name__ == "__main__":
    root = tk.Tk()
    app = DatasetCollectorApp(root)
    root.mainloop()
