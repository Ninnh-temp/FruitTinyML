"""
Edge AI Smart Retail Scale - CLI Dataset Collection Script
------------------------------------------------------------
Lightweight command-line script to collect training images wirelessly
from the ESP32-CAM HTTP server.

Usage:
    python scripts/collect_dataset_cli.py --label apple
    python scripts/collect_dataset_cli.py --label banana --interval 1.5 --count 30
    python scripts/collect_dataset_cli.py --ip 192.168.1.100 --label orange
"""

import os
import sys
import time
import argparse
import urllib.request
import urllib.error

DEFAULT_ESP_IP = "192.168.4.1"
DEFAULT_OUTPUT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "dataset"))


def capture_frame(ip: str) -> bytes:
    url = f"http://{ip.strip('/')}/capture?t={int(time.time() * 1000)}"
    req = urllib.request.Request(url, headers={"User-Agent": "TinyFruitML-CLI/1.0"})
    with urllib.request.urlopen(req, timeout=4.0) as resp:
        return resp.read()


def save_image(img_bytes: bytes, output_dir: str, label: str) -> str:
    class_dir = os.path.join(output_dir, label)
    os.makedirs(class_dir, exist_ok=True)
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    millis = int((time.time() % 1) * 1000)
    filename = f"{label}_{timestamp}_{millis:03d}.jpg"
    filepath = os.path.join(class_dir, filename)
    with open(filepath, "wb") as f:
        f.write(img_bytes)
    return filepath


def main():
    parser = argparse.ArgumentParser(description="Collect training photos from ESP32-CAM over Wi-Fi.")
    parser.add_argument("--ip", default=DEFAULT_ESP_IP, help=f"ESP32 IP address (default: {DEFAULT_ESP_IP})")
    parser.add_argument("--label", default="apple", help="Class label for images (default: apple)")
    parser.add_argument("--interval", type=float, default=0.0, help="Auto-capture interval in seconds (0 = manual enter)")
    parser.add_argument("--count", type=int, default=0, help="Target number of images (0 = infinite)")
    parser.add_argument("--out", default=DEFAULT_OUTPUT_DIR, help="Dataset output directory")
    args = parser.parse_args()

    print("=" * 60)
    print("   TinyFruitML - Wireless Dataset Collector (CLI)")
    print("=" * 60)
    print(f"  ESP32 IP:      http://{args.ip}")
    print(f"  Active Label:  {args.label}")
    print(f"  Output Dir:    {args.out}")
    if args.interval > 0:
        print(f"  Mode:          Automatic burst (every {args.interval}s)")
    else:
        print("  Mode:          Manual (Press [Enter] to capture, 'c' to change label, 'q' to quit)")
    print("=" * 60)

    # Test connection
    print("Testing connection to ESP32-CAM... ", end="", flush=True)
    try:
        data = capture_frame(args.ip)
        print(f"[OK] Received {len(data)} bytes JPEG frame.")
    except Exception as e:
        print(f"[FAIL]\nCould not reach http://{args.ip}: {e}")
        print("Please ensure your PC is connected to the ESP32-CAM Wi-Fi network (SSID: ESP32-CAM-TEST).")
        sys.exit(1)

    captured = 0
    current_label = args.label

    try:
        if args.interval > 0:
            print(f"\nStarting auto-capture for '{current_label}'... (Press Ctrl+C to stop)")
            while args.count == 0 or captured < args.count:
                try:
                    data = capture_frame(args.ip)
                    path = save_image(data, args.out, current_label)
                    captured += 1
                    print(f"[{captured}] Saved: {os.path.basename(path)}")
                except Exception as e:
                    print(f"Capture warning: {e}")
                time.sleep(args.interval)
            print(f"\nFinished capturing {captured} samples for '{current_label}'.")
        else:
            while True:
                user_cmd = input(f"\n[{current_label}] Press [Enter] to capture (or label name / 'q' to quit): ").strip()
                if user_cmd.lower() == 'q':
                    break
                elif user_cmd:
                    current_label = user_cmd.lower().replace(" ", "_")
                    print(f"Switched label to: '{current_label}'")
                    continue

                try:
                    data = capture_frame(args.ip)
                    path = save_image(data, args.out, current_label)
                    captured += 1
                    print(f"  --> Saved #{captured}: {path}")
                except Exception as e:
                    print(f"  --> [ERROR] Capture failed: {e}")

    except KeyboardInterrupt:
        print("\nCollection stopped by user.")

    print(f"\nDone! Total images collected: {captured}")
    print(f"Dataset location: {args.out}")


if __name__ == "__main__":
    main()
