#!/usr/bin/env python3
"""
ABP Navigation Test Script

Launches Chrome with ABP enabled and tests navigation to Wikipedia and Google.
Saves screenshots and test results to local folders.
"""

import base64
import json
import os
import signal
import sqlite3
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

import requests

# Configuration
CHROME_PATH = "/home/paladin/src/chromium/out/Default/chrome"
ABP_PORT = 8222
ABP_BASE_URL = f"http://localhost:{ABP_PORT}/api/v1"

# Paths
SCRIPT_DIR = Path(__file__).parent.resolve()
SCREENSHOTS_DIR = SCRIPT_DIR / "screenshots"
DB_DIR = SCRIPT_DIR / "db"
DB_PATH = DB_DIR / "test_results.sqlite"

# Ensure directories exist
SCREENSHOTS_DIR.mkdir(exist_ok=True)
DB_DIR.mkdir(exist_ok=True)


def init_database():
    """Initialize SQLite database for test results."""
    conn = sqlite3.connect(DB_PATH)
    cursor = conn.cursor()
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS test_runs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            run_id TEXT NOT NULL,
            started_at TEXT NOT NULL,
            completed_at TEXT,
            status TEXT DEFAULT 'running'
        )
    """)
    cursor.execute("""
        CREATE TABLE IF NOT EXISTS test_steps (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            run_id TEXT NOT NULL,
            step_name TEXT NOT NULL,
            url TEXT,
            status TEXT NOT NULL,
            screenshot_path TEXT,
            error_message TEXT,
            timestamp TEXT NOT NULL
        )
    """)
    conn.commit()
    return conn


def log_step(conn, run_id, step_name, url, status, screenshot_path=None, error_message=None):
    """Log a test step to the database."""
    cursor = conn.cursor()
    cursor.execute("""
        INSERT INTO test_steps (run_id, step_name, url, status, screenshot_path, error_message, timestamp)
        VALUES (?, ?, ?, ?, ?, ?, ?)
    """, (run_id, step_name, url, status, screenshot_path, error_message, datetime.now().isoformat()))
    conn.commit()
    print(f"  [{status.upper()}] {step_name}" + (f": {error_message}" if error_message else ""))


def wait_for_abp(timeout=30):
    """Wait for ABP server to become ready using the status endpoint."""
    start = time.time()
    last_error = None
    attempts = 0

    while time.time() - start < timeout:
        attempts += 1
        try:
            # Use /tabs endpoint directly since /browser/status is not implemented
            response = requests.get(f"{ABP_BASE_URL}/tabs", timeout=2)
            if response.status_code == 200:
                data = response.json()
                if isinstance(data, list):
                    print(f"    ABP ready after {attempts} attempts ({len(data)} tabs)")
                    return True
        except requests.exceptions.ConnectionError:
            last_error = "Connection refused"
        except requests.exceptions.Timeout:
            last_error = "Request timeout"
        except requests.exceptions.RequestException as e:
            last_error = str(e)

        if attempts % 10 == 0:
            print(f"    Waiting... ({attempts} attempts, last error: {last_error})")
        time.sleep(0.5)

    print(f"    Final error after {attempts} attempts: {last_error}")
    return False


def take_screenshot(tab_id, name, run_id):
    """Take a screenshot and save it."""
    try:
        response = requests.post(
            f"{ABP_BASE_URL}/tabs/{tab_id}/screenshot",
            json={"screenshot": {"markup": "interactive", "format": "webp", "quality": 80}},
            timeout=30
        )
        if response.status_code == 200:
            data = response.json()
            image_data_b64 = data.get("data", "")
            if not image_data_b64:
                print(f"    Screenshot error: No image data in response")
                return None
            image_data = base64.b64decode(image_data_b64)
            timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
            filename = f"{run_id}_{name}_{timestamp}.webp"
            filepath = SCREENSHOTS_DIR / filename
            with open(filepath, "wb") as f:
                f.write(image_data)
            return str(filepath)
        else:
            print(f"    Screenshot error: {response.status_code}")
    except Exception as e:
        print(f"    Screenshot error: {e}")
    return None


def wait_for_page_load(tab_id, expected_url_part, timeout=15):
    """Wait for page to load by checking the URL."""
    start = time.time()
    while time.time() - start < timeout:
        try:
            response = requests.get(f"{ABP_BASE_URL}/tabs/{tab_id}", timeout=5)
            if response.status_code == 200:
                data = response.json()
                url = data.get("url", "")
                if expected_url_part.lower() in url.lower() and not url.startswith("chrome://"):
                    # Give extra time for page content to render
                    time.sleep(2)
                    return True
        except requests.exceptions.RequestException:
            pass
        time.sleep(0.5)
    return False


def cleanup_existing_chrome():
    """Kill any existing Chrome ABP processes and free up the port."""
    try:
        # Kill Chrome processes with ABP flag
        subprocess.run(
            ["pkill", "-f", "chrome.*--enable-abp"],
            capture_output=True,
            timeout=5
        )
        time.sleep(1)
    except Exception:
        pass


def run_tests():
    """Run the navigation tests."""
    run_id = datetime.now().strftime("%Y%m%d_%H%M%S")
    chrome_process = None
    chrome_log = None

    print("=" * 60)
    print("ABP Navigation Test")
    print("=" * 60)
    print(f"\nTest Run ID: {run_id}")
    print(f"Screenshots folder: {SCREENSHOTS_DIR}")
    print(f"SQLite database: {DB_PATH}")
    print("=" * 60)

    # Clean up any existing Chrome ABP instances
    print("\n[0] Cleaning up existing Chrome instances...")
    cleanup_existing_chrome()

    # Initialize database
    conn = init_database()
    cursor = conn.cursor()
    cursor.execute("""
        INSERT INTO test_runs (run_id, started_at, status)
        VALUES (?, ?, 'running')
    """, (run_id, datetime.now().isoformat()))
    conn.commit()

    try:
        # Launch Chrome with ABP
        print("\n[1] Launching Chrome with ABP enabled...")
        chrome_args = [
            CHROME_PATH,
            "--enable-abp",
            f"--abp-port={ABP_PORT}",
            "--headless=new",  # Headless mode for cursor rendering via CopyFromSurface
        ]
        print(f"    Command: {' '.join(chrome_args)}")

        # Create log file for Chrome output
        chrome_log_path = SCRIPT_DIR / "chrome_output.log"
        chrome_log = open(chrome_log_path, "w")

        chrome_process = subprocess.Popen(
            chrome_args,
            stdout=chrome_log,
            stderr=subprocess.STDOUT,
            preexec_fn=os.setsid
        )
        print(f"    Chrome PID: {chrome_process.pid}")
        print(f"    Chrome log: {chrome_log_path}")

        # Give Chrome time to initialize before polling
        print("    Waiting 5 seconds for Chrome to initialize...")
        time.sleep(5)

        # Check if Chrome is still running
        if chrome_process.poll() is not None:
            log_step(conn, run_id, "Launch Chrome", None, "failed",
                    error_message=f"Chrome exited with code {chrome_process.returncode}")
            raise Exception(f"Chrome exited prematurely with code {chrome_process.returncode}")

        log_step(conn, run_id, "Launch Chrome", None, "success")

        # Wait for ABP server
        print("\n[2] Waiting for ABP server...")
        if not wait_for_abp(timeout=30):
            log_step(conn, run_id, "ABP Server Ready", None, "failed", error_message="Timeout waiting for ABP")
            raise Exception("ABP server did not start in time")
        log_step(conn, run_id, "ABP Server Ready", None, "success")

        # Get initial tabs
        print("\n[3] Getting initial tabs...")
        response = requests.get(f"{ABP_BASE_URL}/tabs", timeout=10)
        tabs = response.json()
        if not tabs:
            # Create a new tab if none exist
            response = requests.post(f"{ABP_BASE_URL}/tabs", json={"url": "about:blank"}, timeout=10)
            tab_data = response.json()
            tab_id = tab_data.get("id")
        else:
            tab_id = tabs[0].get("id")
        log_step(conn, run_id, "Get Tab ID", None, "success")
        print(f"    Using tab ID: {tab_id}")

        # Navigate to Wikipedia
        print("\n[4] Navigating to Wikipedia...")
        wiki_url = "https://en.wikipedia.org/wiki/Main_Page"
        response = requests.post(
            f"{ABP_BASE_URL}/tabs/{tab_id}/navigate",
            json={"url": wiki_url},
            timeout=10
        )
        if response.status_code != 200:
            log_step(conn, run_id, "Navigate to Wikipedia", wiki_url, "failed",
                    error_message=f"HTTP {response.status_code}")
            raise Exception(f"Navigation failed: {response.status_code}")

        if not wait_for_page_load(tab_id, "wikipedia", timeout=20):
            log_step(conn, run_id, "Navigate to Wikipedia", wiki_url, "failed",
                    error_message="Page load timeout")
            raise Exception("Wikipedia page load timeout")

        screenshot_path = take_screenshot(tab_id, "wikipedia", run_id)
        log_step(conn, run_id, "Navigate to Wikipedia", wiki_url, "success", screenshot_path=screenshot_path)

        # Navigate to Google
        print("\n[5] Navigating to Google...")
        google_url = "https://www.google.com"
        response = requests.post(
            f"{ABP_BASE_URL}/tabs/{tab_id}/navigate",
            json={"url": google_url},
            timeout=10
        )
        if response.status_code != 200:
            log_step(conn, run_id, "Navigate to Google", google_url, "failed",
                    error_message=f"HTTP {response.status_code}")
            raise Exception(f"Navigation failed: {response.status_code}")

        if not wait_for_page_load(tab_id, "google", timeout=20):
            log_step(conn, run_id, "Navigate to Google", google_url, "failed",
                    error_message="Page load timeout")
            raise Exception("Google page load timeout")

        screenshot_path = take_screenshot(tab_id, "google", run_id)
        log_step(conn, run_id, "Navigate to Google", google_url, "success", screenshot_path=screenshot_path)

        # Mark test run as completed
        cursor.execute("""
            UPDATE test_runs SET completed_at = ?, status = 'success'
            WHERE run_id = ?
        """, (datetime.now().isoformat(), run_id))
        conn.commit()

        print("\n" + "=" * 60)
        print("TEST COMPLETED SUCCESSFULLY")
        print("=" * 60)

    except Exception as e:
        print(f"\n[ERROR] {e}")
        cursor.execute("""
            UPDATE test_runs SET completed_at = ?, status = 'failed'
            WHERE run_id = ?
        """, (datetime.now().isoformat(), run_id))
        conn.commit()

    finally:
        # Cleanup
        if chrome_process:
            print("\n[*] Shutting down Chrome...")
            try:
                os.killpg(os.getpgid(chrome_process.pid), signal.SIGTERM)
                chrome_process.wait(timeout=5)
            except Exception:
                try:
                    os.killpg(os.getpgid(chrome_process.pid), signal.SIGKILL)
                except Exception:
                    pass

        # Close log file
        try:
            chrome_log.close()
        except Exception:
            pass

        conn.close()

        # Print summary
        print("\n" + "=" * 60)
        print("OUTPUT LOCATIONS")
        print("=" * 60)
        print(f"Screenshots: {SCREENSHOTS_DIR}")
        print(f"SQLite DB:   {DB_PATH}")
        print("=" * 60)


if __name__ == "__main__":
    run_tests()
