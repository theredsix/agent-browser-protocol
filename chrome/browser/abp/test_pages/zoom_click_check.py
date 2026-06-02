#!/usr/bin/env python3
"""Zoom-aware click coordinate check for ABP.

Verifies the core contract: a click at a screenshot-pixel (DIP) coordinate
lands on the element that visually occupies that pixel, even when the page
zoom is not 100%.

Background: ABP screenshots are produced in viewport DIP pixels, but CDP
Input.dispatchMouseEvent interprets x/y as CSS pixels (Chromium multiplies by
the page zoom factor internally). At ABP's default 80% zoom these two spaces
diverge by the zoom factor, so an agent that reads a coordinate off the
screenshot and clicks it misses the target. This check fails before the fix
and passes after it.

Usage: zoom_click_check.py <port>
Exit: 0 = PASS, non-zero = FAIL.
"""
import json
import sys
import time
import urllib.request
import urllib.error

PORT = sys.argv[1] if len(sys.argv) > 1 else "8333"
BASE = "http://localhost:%s/api/v1" % PORT
TARGET_TOL = 6  # CSS px tolerance for a "hit" on the 40px box centered at 310


def req(method, path, body=None, timeout=30):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(
        BASE + path, data=data, method=method,
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
            req("GET", "/browser/status", timeout=3)
            return True
        except urllib.error.HTTPError:
            # The server bound and answered (even with a not-ready status code),
            # so the HTTP server is up. Good enough to proceed.
            return True
        except Exception:
            time.sleep(1)
    return False


def first_tab():
    for _ in range(60):
        try:
            d = req("GET", "/tabs")
            tabs = d["tabs"] if isinstance(d, dict) and "tabs" in d else d
            if tabs:
                t = tabs[0]
                return t.get("id") or t.get("tab_id")
        except Exception:
            pass
        time.sleep(1)
    return None


def execute(tab, script):
    d = req("POST", "/tabs/%s/execute" % tab, {"script": script})
    res = d.get("result") if isinstance(d, dict) else None
    if isinstance(res, dict) and "value" in res:
        return res["value"]
    return res


def fail(msg):
    print("FAIL: " + msg)
    return 1


def main():
    if not wait_ready():
        return fail("browser did not become ready")
    tab = first_tab()
    if not tab:
        return fail("no tab found")

    # Wait for the test page to finish loading.
    for _ in range(40):
        if execute(tab, "document.readyState") == "complete" and \
           execute(tab, "!!document.getElementById('t')"):
            break
        time.sleep(0.5)
    else:
        return fail("test page (zoom-click-test.html) never loaded")

    meta = json.loads(execute(
        tab,
        "JSON.stringify({"
        "cw:document.documentElement.clientWidth,"
        "dpr:window.devicePixelRatio,"
        "rect:(function(){var r=document.getElementById('t')"
        ".getBoundingClientRect();return{cx:r.left+r.width/2,"
        "cy:r.top+r.height/2};})()})"))
    cw = float(meta["cw"])
    cx_css = float(meta["rect"]["cx"])
    cy_css = float(meta["rect"]["cy"])

    # DIP viewport width comes from the screenshot envelope's scroll info.
    shot = req("POST", "/tabs/%s/screenshot" % tab, {})
    scroll = shot.get("scroll", {}) if isinstance(shot, dict) else {}
    vw_dip = scroll.get("viewportWidth")
    if not vw_dip:
        return fail("no viewportWidth in screenshot scroll info: %r" % scroll)
    z = float(vw_dip) / cw
    print("clientWidth(CSS)=%.1f  viewportWidth(DIP)=%.1f  =>  zoom=%.1f%%  "
          "(devicePixelRatio=%s)" % (cw, float(vw_dip), z * 100, meta.get("dpr")))

    # The element center is at CSS (cx,cy); in the DIP screenshot it appears at
    # pixel (cx*z, cy*z). A vision agent reads that pixel and clicks it.
    px = round(cx_css * z)
    py = round(cy_css * z)
    print("element CSS center=(%.0f,%.0f)  ->  screenshot pixel(DIP)=(%d,%d)"
          % (cx_css, cy_css, px, py))

    execute(tab, "window.__c=null")
    req("POST", "/tabs/%s/click" % tab, {"x": px, "y": py})

    rec = None
    for _ in range(15):
        v = execute(tab, "JSON.stringify(window.__c)")
        if v and v != "null":
            rec = json.loads(v)
            break
        time.sleep(0.2)
    if rec is None:
        return fail("the click did not register on the page at all")

    print("recorded click: clientX=%.1f clientY=%.1f target=<%s id=%r>"
          % (rec["x"], rec["y"], rec.get("tag"), rec.get("id")))

    hit_id = rec.get("id") == "t"
    hit_xy = abs(rec["x"] - cx_css) <= TARGET_TOL and \
        abs(rec["y"] - cy_css) <= TARGET_TOL
    if hit_id and hit_xy:
        print("PASS: click at screenshot pixel (%d,%d) hit the element "
              "(CSS center %.0f,%.0f) at %.0f%% zoom"
              % (px, py, cx_css, cy_css, z * 100))
        return 0
    return fail(
        "click at screenshot pixel (%d,%d) landed at CSS (%.1f,%.1f), but the "
        "element is at CSS (%.0f,%.0f) [target=%r]. At %.0f%% zoom the DIP "
        "screenshot space and the CSS click-input space diverge, so the click "
        "misses." % (px, py, rec["x"], rec["y"], cx_css, cy_css,
                     rec.get("id"), z * 100))


if __name__ == "__main__":
    sys.exit(main())
