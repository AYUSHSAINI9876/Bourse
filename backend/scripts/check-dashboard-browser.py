#!/usr/bin/env python3
"""Drive the dashboard in a real browser and check that it actually works.

Every other check on this file is static. They are worth having, but they all
missed the worst outage the dashboard has had: a string literal written across
two lines, which is a parse error for the whole <script> block. Every handler
failed to attach. The markup was valid, every element id resolved, no external
resource had crept in, all eight verify-all stages were green -- and the page
did nothing at all when you clicked it.

The only thing that catches that is loading the page and clicking. So this
starts a server, opens the dashboard in Chromium, signs up, signs in, signs
out, runs commands, and reads the results back out of the DOM.

    python3 scripts/check-dashboard-browser.py [--binary path/to/bourse-server]
    python3 scripts/check-dashboard-browser.py --base-url http://127.0.0.1:8080

With --base-url it drives a server that is already running instead of starting
one, which is how to point it at a deployment. That server must have auth on
and no accounts yet, since the first thing the run does is create one.

Playwright is a development dependency and not everyone has it, so its absence
is reported as a skip rather than a failure -- the server itself still has no
third-party runtime dependencies. Install it with:

    pip install playwright && playwright install chromium
"""

import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
BACKEND_ROOT = os.path.dirname(HERE)


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def wait_for_health(base, deadline_seconds=30):
    deadline = time.time() + deadline_seconds
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(base + "/health", timeout=1) as response:
                if response.status == 200:
                    return True
        except (urllib.error.URLError, OSError):
            time.sleep(0.2)
    return False


class Checks:
    """Collects results so one failure does not hide the rest."""

    def __init__(self):
        self.results = []

    def __call__(self, label, ok, detail=""):
        self.results.append(ok)
        prefix = "  ok   " if ok else "  FAIL "
        line = prefix + label
        if detail and not ok:
            line += "\n       got: " + str(detail).replace("\n", " ")[:160]
        print(line, flush=True)

    @property
    def failed(self):
        return self.results.count(False)


def run(page, base, checks):
    errors = []
    page.on("pageerror", lambda e: errors.append(str(e)))
    page.on("console",
            lambda m: errors.append("console." + m.type + ": " + m.text) if m.type == "error" else None)

    page.goto(base, wait_until="networkidle")

    # ---- the gate ---------------------------------------------------------
    checks("dashboard loads", "Bourse" in page.title())
    checks("sign-in gate is shown", page.locator("#gate").is_visible())

    # Nobody has registered, so the gate must open on the tab that can make an
    # account rather than one that cannot possibly succeed.
    checks("opens on Create account for an empty server",
           page.locator("#tabSignup").get_attribute("aria-selected") == "true")
    checks("says the first account is the administrator",
           "administrator" in page.locator("#gateBlurb").inner_text())
    checks("confirm-password field is shown", page.locator("#confirmRow").is_visible())
    checks("submit says Create account",
           page.locator("#loginBtn").inner_text().strip() == "Create account")

    # ---- the tabs are wired ----------------------------------------------
    page.click("#tabSignin")
    checks("clicking Sign in switches mode",
           page.locator("#loginBtn").inner_text().strip() == "Sign in")
    checks("confirm field hides in sign-in mode", page.locator("#confirmRow").is_hidden())
    page.click("#tabSignup")
    checks("clicking Create account switches back",
           page.locator("#loginBtn").inner_text().strip() == "Create account")

    # ---- a typo in the confirmation is caught before the account is made ---
    page.fill("#lu", "alice")
    page.fill("#lp", "alice-password-1")
    page.fill("#lc", "something-else-1")
    page.click("#loginBtn")
    page.wait_for_timeout(400)
    checks("mismatched passwords are refused",
           "do not match" in page.locator("#loginMsg").inner_text())
    checks("gate stays open after a mismatch", page.locator("#gate").is_visible())

    # ---- signup -----------------------------------------------------------
    page.fill("#lc", "alice-password-1")
    page.click("#loginBtn")
    page.wait_for_selector("#gate", state="hidden", timeout=30000)
    checks("signup signs you in and closes the gate", not page.locator("#gate").is_visible())
    checks("header shows the username",
           page.locator("#whoamiName").inner_text().strip() == "alice")
    # The badge is uppercased in CSS, so compare case-insensitively.
    checks("the first account is an administrator",
           page.locator("#whoamiRole").inner_text().strip().lower() == "admin")
    checks("administrator sees the Users panel", page.locator("#usersCard").is_visible())
    checks("security panel is shown", page.locator("#securityCard").is_visible())

    page.wait_for_timeout(1500)
    checks("server status reads as up", "up" in (page.locator("#dot").get_attribute("class") or ""))

    # ---- the panels do something -----------------------------------------
    page.fill("#cmd", "SET browser-key hello")
    page.click("#run")
    page.wait_for_timeout(900)
    console_text = page.locator("#out").inner_text()
    checks("command console returns a reply", "OK" in console_text, console_text)

    page.fill("#cmd", "ORDER AAPL BUY LIMIT 5 101.25")
    page.click("#run")
    page.wait_for_timeout(1200)
    console_text = page.locator("#out").inner_text()
    checks("an order placed from the console is accepted",
           "NEW" in console_text or "FILLED" in console_text, console_text)

    # The order form is a separate control from the console.
    page.select_option("#oside", "SELL")
    page.fill("#oqty", "3")
    page.fill("#oprice", "142.50")
    page.click("#send")
    page.wait_for_timeout(1200)
    checks("an order placed from the form reaches the book",
           "142.50" in page.locator("#asks").inner_text(),
           page.locator("#asks").inner_text())
    checks("the console's bid is in the book",
           "101.25" in page.locator("#bids").inner_text(),
           page.locator("#bids").inner_text())

    page.fill("#sql", "CREATE TABLE fills (id INTEGER, sym TEXT, qty INTEGER)")
    page.click("#runSql")
    page.wait_for_timeout(700)
    page.fill("#sql", "INSERT INTO fills VALUES (1, 'AAPL', 100), (2, 'MSFT', 50)")
    page.click("#runSql")
    page.wait_for_timeout(700)
    page.fill("#sql", "SELECT sym, qty FROM fills WHERE qty > 60")
    page.click("#runSql")
    page.wait_for_timeout(900)
    checks("SQL returns rows", "AAPL" in page.locator("#sqlBody").inner_text(),
           page.locator("#sqlBody").inner_text())

    page.click("#explain")
    page.wait_for_timeout(900)
    checks("Explain shows a query plan", "SeqScan" in page.locator("#sqlOut").inner_text(),
           page.locator("#sqlOut").inner_text())

    page.fill("#pattern", "*")
    page.click("#scan")
    page.wait_for_timeout(900)
    checks("keyspace scan lists the key", "browser-key" in page.locator("#keys").inner_text(),
           page.locator("#keys").inner_text())

    # ---- sign out, and back in -------------------------------------------
    page.click("#logout")
    page.wait_for_timeout(900)
    checks("signing out re-opens the gate", page.locator("#gate").is_visible())
    # Somebody signing out has an account, so the dialog must come back ready
    # to sign in rather than on whichever tab it was left on.
    checks("the gate returns on the Sign in tab",
           page.locator("#tabSignin").get_attribute("aria-selected") == "true")

    page.reload(wait_until="networkidle")
    page.wait_for_timeout(600)
    # An account exists now, so the default tab has to be the other one.
    checks("gate opens on Sign in once an account exists",
           page.locator("#tabSignin").get_attribute("aria-selected") == "true")

    page.fill("#lu", "alice")
    page.fill("#lp", "alice-password-1")
    page.click("#loginBtn")
    page.wait_for_selector("#gate", state="hidden", timeout=30000)
    checks("signing back in works", page.locator("#whoamiName").inner_text().strip() == "alice")

    # ---- the second account is not an administrator -----------------------
    page.click("#logout")
    page.wait_for_timeout(700)
    page.click("#tabSignup")
    page.fill("#lu", "bob")
    page.fill("#lp", "bob-password-12")
    page.fill("#lc", "bob-password-12")
    page.click("#loginBtn")
    page.wait_for_selector("#gate", state="hidden", timeout=30000)
    checks("the second account is a trader",
           page.locator("#whoamiRole").inner_text().strip().lower() == "trader")
    checks("a trader does not see the Users panel", page.locator("#usersCard").is_hidden())

    # A 403 is the expected answer below, and the browser logs every non-2xx
    # fetch as a console error, so the error list is snapshotted first.
    errors_before_refusal = list(errors)

    page.fill("#cmd", "FLUSHALL")
    page.click("#run")
    page.wait_for_timeout(900)
    refusal = page.locator("#out").inner_text()
    checks("a trader is refused FLUSHALL",
           "NOPERM" in refusal or "403" in refusal or "permission" in refusal.lower(), refusal)

    checks("no uncaught JavaScript errors", not errors_before_refusal,
           "; ".join(errors_before_refusal[:3]))
    unexpected = [e for e in errors[len(errors_before_refusal):] if "403" not in e]
    checks("the refusal was the only new console error", not unexpected, "; ".join(unexpected[:3]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=None,
                        help="bourse-server to run; defaults to $BOURSE_BUILD_DIR/bin or build/bin")
    parser.add_argument("--base-url", default=None,
                        help="drive a server already running at this URL instead of starting one")
    args = parser.parse_args()

    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print("  skip playwright is not installed; the dashboard was not opened")
        print("       pip install playwright && playwright install chromium")
        return 0

    server = None
    data_dir = None
    if args.base_url:
        base = args.base_url.rstrip("/")
    else:
        build_dir = os.environ.get("BOURSE_BUILD_DIR", "build")
        if not os.path.isabs(build_dir):
            build_dir = os.path.join(BACKEND_ROOT, build_dir)
        binary = args.binary or os.path.join(build_dir, "bin", "bourse-server")
        if not os.path.exists(binary):
            print("check-dashboard-browser: {} not built".format(binary), file=sys.stderr)
            return 1

        http_port = free_port()
        resp_port = free_port()
        data_dir = tempfile.mkdtemp(prefix="bourse-browser-check-")
        base = "http://127.0.0.1:{}".format(http_port)

        environment = dict(os.environ)
        # Auth on, and no seeded account: the point is that signup is the way in.
        environment["BOURSE_AUTH"] = "yes"
        environment.pop("BOURSE_ADMIN_USER", None)
        environment.pop("BOURSE_ADMIN_PASSWORD", None)

        server = subprocess.Popen(
            [binary, "--host", "127.0.0.1", "--port", str(resp_port), "--http-port", str(http_port),
             "--dir", data_dir, "--auth-iterations", "1000", "--log-level", "error"],
            env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    checks = Checks()
    try:
        if not wait_for_health(base):
            print("check-dashboard-browser: no healthy server at {}".format(base), file=sys.stderr)
            return 1

        with sync_playwright() as pw:
            # A system Chrome or Edge is used when playwright's own chromium
            # has not been downloaded, so the check works after a bare
            # `pip install playwright`.
            browser = None
            for launch in (lambda: pw.chromium.launch(),
                           lambda: pw.chromium.launch(channel="chrome"),
                           lambda: pw.chromium.launch(channel="msedge")):
                try:
                    browser = launch()
                    break
                except Exception:  # noqa: BLE001 -- any launch failure means try the next
                    continue
            if browser is None:
                print("  skip no chromium, chrome or edge available to drive")
                return 0

            page = browser.new_page()
            try:
                run(page, base, checks)
            finally:
                browser.close()
    finally:
        if server is not None:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
        if data_dir is not None:
            shutil.rmtree(data_dir, ignore_errors=True)

    print()
    print("  {} passed, {} failed".format(len(checks.results) - checks.failed, checks.failed))
    return 1 if checks.failed else 0


if __name__ == "__main__":
    sys.exit(main())
