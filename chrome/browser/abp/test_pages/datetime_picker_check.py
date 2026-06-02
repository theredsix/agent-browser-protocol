#!/usr/bin/env python3
"""Launch-test checker for ABP date/time picker interception (+ select regression).

For each of the five date/time-family inputs: trigger the picker, assert a
`datetime_picker_open` event surfaces with the right input_type and current
value, respond with a new ISO value via POST /api/v1/datetime-picker/{id}, and
assert the input committed it (input+change fired). Then assert a <select>
still produces `select_open` (Part A: Linux/Windows external popups).

Usage: datetime_picker_check.py <port>
Exit: 0 = all PASS, non-zero = failure.
"""
import json
import sys
import time
import urllib.request
import urllib.error

PORT = sys.argv[1] if len(sys.argv) > 1 else "8355"
BASE = "http://localhost:%s/api/v1" % PORT

# New ISO values to apply per input_type (valid for the field).
NEW_VALUE = {
    "date": "2026-06-15",
    "time": "14:45",
    "datetime-local": "2026-06-15T14:45",
    "month": "2026-06",
    "week": "2026-W25",
}


def req(method, path, body=None, timeout=30):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(BASE + path, data=data, method=method,
                               headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(r, timeout=timeout) as f:
        raw = f.read().decode()
    try:
        return json.loads(raw)
    except Exception:
        return raw


def wait_ready(timeout=120):
    for _ in range(timeout):
        try:
            req("GET", "/browser/status", timeout=3); return True
        except urllib.error.HTTPError:
            return True
        except Exception:
            time.sleep(1)
    return False


def tab():
    for _ in range(60):
        try:
            d = req("GET", "/tabs")
            ts = d["tabs"] if isinstance(d, dict) and "tabs" in d else d
            if ts:
                return ts[0].get("id") or ts[0].get("tab_id")
        except Exception:
            pass
        time.sleep(1)
    return None


def ev(t, script):
    d = req("POST", "/tabs/%s/execute" % t, {"script": script})
    res = d.get("result") if isinstance(d, dict) else None
    return res.get("value") if isinstance(res, dict) and "value" in res else res


def find_picker_event(envelope, want_type):
    """Search an action envelope's events for datetime_picker_open."""
    if not isinstance(envelope, dict):
        return None
    for e in envelope.get("events", []) or []:
        data = e.get("data", e) if isinstance(e, dict) else {}
        if data.get("type") == "datetime_picker_open" and \
           data.get("input_type") == want_type:
            return data
    return None


def fail(msg):
    print("FAIL: " + msg)
    return 1


def check_datetime(t, input_type):
    print("\n----- %s -----" % input_type)
    ev(t, "window.__log = []")
    # Click first (transient activation + focus), then open the picker.
    rects = json.loads(ev(t, "JSON.stringify(window.__rects())"))
    r = rects[input_type]
    req("POST", "/tabs/%s/click" % t, {"x": r["x"], "y": r["y"]})
    env = req("POST", "/tabs/%s/execute" % t,
              {"script": "window.__showPicker('%s')" % input_type})
    picker = find_picker_event(env, input_type)
    if not picker:
        return fail("%s: no datetime_picker_open event surfaced "
                    "(envelope events=%s)"
                    % (input_type, json.dumps(env.get("events") if isinstance(env, dict) else env)[:300]))
    print("  event: id=%s value=%r min=%r max=%r"
          % (picker.get("id"), picker.get("value"),
             picker.get("min"), picker.get("max")))
    pid = picker.get("id")
    if not pid:
        return fail("%s: picker event missing id" % input_type)

    new_val = NEW_VALUE[input_type]
    resp = req("POST", "/datetime-picker/%s" % pid, {"value": new_val})
    if not (isinstance(resp, dict) and resp.get("success")):
        return fail("%s: respond failed: %s" % (input_type, json.dumps(resp)[:200]))

    got = None
    for _ in range(20):
        got = ev(t, "document.getElementById('%s').value" % input_type)
        if got == new_val:
            break
        time.sleep(0.05)
    log = ev(t, "JSON.stringify(window.__log)")
    if got != new_val:
        return fail("%s: value not applied (got %r, expected %r); log=%s"
                    % (input_type, got, new_val, log))
    if "change" not in (log or ""):
        return fail("%s: no change event fired; log=%s" % (input_type, log))
    print("  PASS: applied %r, change fired" % new_val)
    return 0


def check_select(t):
    print("\n----- select (Part A regression) -----")
    rects = json.loads(ev(t, "JSON.stringify(window.__rects())"))
    r = rects["select"]
    env = req("POST", "/tabs/%s/click" % t, {"x": r["x"], "y": r["y"]})
    found = False
    for e in (env.get("events", []) if isinstance(env, dict) else []):
        data = e.get("data", e) if isinstance(e, dict) else {}
        if data.get("type") == "select_open":
            found = True
            print("  event: select_open id=%s items=%s"
                  % (data.get("id"), len(data.get("items", []))))
            break
    if not found:
        return fail("select: no select_open event (Part A not working). "
                    "events=%s" % json.dumps(env.get("events") if isinstance(env, dict) else env)[:300])
    print("  PASS: select_open surfaced")
    return 0


def main():
    if not wait_ready():
        return fail("browser not ready")
    t = tab()
    if not t:
        return fail("no tab")
    for _ in range(40):
        if ev(t, "document.readyState") == "complete" and \
           ev(t, "!!window.__rects"):
            break
        time.sleep(0.5)

    rc = 0
    for input_type in ["date", "time", "datetime-local", "month", "week"]:
        rc |= check_datetime(t, input_type)
    rc |= check_select(t)

    print("\n==== %s ====" % ("ALL PASS" if rc == 0 else "FAILURES (see above)"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
