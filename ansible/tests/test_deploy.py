"""domator-deploy end to end on the controller: a real git origin, fake uv and
restart commands, and an HTTP server standing in for turbacz."""
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

ANSIBLE = Path(__file__).resolve().parents[1]
DEPLOY = ANSIBLE / "roles/domator_jail/files/domator-deploy"
TOKEN = "t" * 40


def git(cwd, *args):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True,
                          env=os.environ | {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
                                            "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}).stdout.strip()


class App(http.server.BaseHTTPRequestHandler):
    """turbacz stand-in: healthy unless the deployed release says otherwise."""
    current = None
    hosts = []

    def do_GET(self):
        App.hosts.append(self.headers["Host"])
        health = (Path(App.current) / "health").read_text().strip()
        self.send_response(200 if health == "ok" else 500)
        self.end_headers()

    def log_message(self, *args):
        pass


class DeployTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="domator-deploy-test-"))
        self.addCleanup(shutil.rmtree, self.tmp)
        self.origin = self.tmp / "origin"
        (self.origin / "turbacz").mkdir(parents=True)
        git(self.origin, "init", "-q", "-b", "main")
        (self.origin / "README.md").write_text("outside the subdir\n")
        self.commit("ok")

        bin_dir = self.tmp / "bin"
        bin_dir.mkdir()
        uv = bin_dir / "uv"
        uv.write_text("#!/bin/sh\n[ -e uvfail ] && { echo 'build broke' >&2; exit 1; }\n"
                      "mkdir -p .venv/bin && echo '#!/bin/sh' > .venv/bin/turbacz && chmod +x .venv/bin/turbacz\n"
                      "echo \"$@\" > uv.args\n")
        uv.chmod(0o755)
        self.restarts = self.tmp / "restarts"
        restart = bin_dir / "restart"
        restart.write_text(f"#!/bin/sh\nreadlink {self.tmp}/base/current >> {self.restarts}\n")
        restart.chmod(0o755)

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), App)
        App.current = str(self.tmp / "base/current")
        App.hosts = []
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)

        (self.tmp / "token").write_text(TOKEN + "\n")
        self.config = self.tmp / "deploy.json"
        self.config.write_text(json.dumps({
            "name": "czupel", "branch": "main", "repo": str(self.origin), "subdir": "turbacz",
            "base": str(self.tmp / "base"), "python": "/usr/local/bin/python3.14",
            "uv": str(uv), "config": "/usr/local/etc/domator/turbacz.toml",
            "restart": [str(restart)], "keep": 2, "health_timeout": 3, "health_interval": 0.2,
            "health_url": f"http://127.0.0.1:{self.server.server_port}/login",
            "health_host": "czupel.dry.pl", "token_file": str(self.tmp / "token"),
            "listen": "127.0.0.1", "port": 0,
        }))

    def commit(self, health, extra=None):
        (self.origin / "turbacz/health").write_text(health + "\n")
        for name in extra or []:
            (self.origin / "turbacz" / name).write_text("x\n")
        git(self.origin, "add", "-A")
        git(self.origin, "commit", "-q", "-m", health)
        return git(self.origin, "rev-parse", "HEAD")

    def deploy(self):
        return subprocess.run([sys.executable, str(DEPLOY), "--config", str(self.config), "deploy"],
                              capture_output=True, text=True, timeout=60)

    def status(self):
        return json.loads((self.tmp / "base/state/status.json").read_text())

    def current(self):
        return os.path.basename(os.readlink(self.tmp / "base/current"))

    def test_first_deploy_builds_the_subdir_and_links_the_config(self):
        sha = git(self.origin, "rev-parse", "HEAD")
        result = self.deploy()
        self.assertEqual(result.returncode, 0, result.stderr)
        release = self.tmp / "base/releases" / sha
        self.assertEqual(self.current(), sha)
        self.assertEqual(self.status()["state"], "ok")
        self.assertTrue((release / "health").exists())
        self.assertFalse((release / "README.md").exists())
        self.assertEqual(os.readlink(release / "turbacz.toml"), "/usr/local/etc/domator/turbacz.toml")
        self.assertEqual((release / "uv.args").read_text().split(),
                         ["sync", "--frozen", "--no-dev", "--python", "/usr/local/bin/python3.14"])
        self.assertIn("czupel.dry.pl", App.hosts)

    def test_same_commit_is_not_restarted_again(self):
        self.deploy()
        self.deploy()
        self.assertEqual(len(self.restarts.read_text().splitlines()), 1)
        self.assertEqual(self.status()["message"], "already deployed")

    def test_failed_health_check_rolls_back(self):
        good = git(self.origin, "rev-parse", "HEAD")
        self.deploy()
        bad = self.commit("broken")
        result = self.deploy()
        self.assertEqual(result.returncode, 1)
        status = self.status()
        self.assertEqual((status["state"], status["sha"]), ("rolled_back", bad))
        self.assertEqual(self.current(), good)
        # bad release started, then the good one again
        self.assertEqual([os.path.basename(p) for p in self.restarts.read_text().split()], [good, bad, good])

    def test_build_failure_keeps_the_running_release(self):
        good = git(self.origin, "rev-parse", "HEAD")
        self.deploy()
        bad = self.commit("ok", extra=["uvfail"])
        result = self.deploy()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.status()["state"], "failed")
        self.assertEqual(self.current(), good)
        self.assertFalse((self.tmp / "base/releases" / bad).exists())
        self.assertEqual(len(self.restarts.read_text().splitlines()), 1)

    def test_old_releases_are_pruned(self):
        shas = [git(self.origin, "rev-parse", "HEAD")]
        self.deploy()
        for n in range(3):
            time.sleep(0.05)  # distinct mtimes
            shas.append(self.commit("ok", extra=[f"f{n}"]))
            self.assertEqual(self.deploy().returncode, 0)
        left = sorted(p.name for p in (self.tmp / "base/releases").iterdir())
        self.assertEqual(left, sorted(shas[-2:]))


class EnvironmentTest(DeployTest):
    def test_path_reaches_usr_local_when_started_by_rc(self):
        # rc starts the deployer with a PATH that lacks /usr/local (git, uv).
        script = ("import importlib.machinery as m, os, sys\n"
                  "d = m.SourceFileLoader('d', sys.argv[1]).load_module()\n"
                  "d.serve = lambda *a: print(os.environ['PATH'])\n"
                  "d.main(['--config', sys.argv[2], 'serve'])\n")
        result = subprocess.run([sys.executable, "-c", script, str(DEPLOY), str(self.config)],
                                capture_output=True, text=True, timeout=10,
                                env=os.environ | {"PATH": "/usr/sbin:/usr/bin:/sbin:/bin"})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith("/usr/local/sbin:/usr/local/bin:"), result.stdout)


class HookTest(DeployTest):
    def setUp(self):
        super().setUp()
        self.hook = subprocess.Popen([sys.executable, str(DEPLOY), "--config", str(self.config), "serve"],
                                     stderr=subprocess.PIPE, text=True)
        self.addCleanup(self.hook.stderr.close)
        self.addCleanup(self.hook.wait)
        self.addCleanup(self.hook.terminate)
        line = self.hook.stderr.readline()  # "... listening on 127.0.0.1:<port>"
        self.url = f"http://127.0.0.1:{line.rsplit(':', 1)[1].strip()}/_deploy"

    def call(self, method, path="", body=None, token=TOKEN):
        request = urllib.request.Request(self.url + path, method=method, data=body,
                                         headers={"Authorization": f"Bearer {token}"} if token else {})
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as e:
            with e:
                return e.code, json.loads(e.read())

    def test_wrong_or_missing_token_is_refused(self):
        self.assertEqual(self.call("POST", body=b"{}", token="x" * 40)[0], 401)
        self.assertEqual(self.call("POST", body=b"{}", token=None)[0], 401)
        self.assertEqual(self.call("GET", "/status", token="wrong")[0], 401)
        self.assertFalse((self.tmp / "base/state/pending.json").exists())

    def test_bad_requests(self):
        self.assertEqual(self.call("POST", body=b'{"sha": "main; rm -rf /"}')[0], 400)
        self.assertEqual(self.call("POST", body=b"not json")[0], 400)
        self.assertEqual(self.call("POST", body=b"x" * 5000)[0], 413)
        self.assertEqual(self.call("GET", "/other")[0], 404)

    def test_post_deploys_and_status_reports_it(self):
        self.assertEqual(self.call("GET", "/status"), (200, {"state": "none"}))
        sha = git(self.origin, "rev-parse", "HEAD")
        self.assertEqual(self.call("POST", body=json.dumps({"sha": sha}).encode()), (202, {"queued": sha}))
        for _ in range(100):
            code, status = self.call("GET", "/status")
            if status.get("state") in ("ok", "failed", "rolled_back"):
                break
            time.sleep(0.1)
        self.assertEqual((status["state"], status["sha"], status["requested"]), ("ok", sha, sha))
        self.assertEqual(self.current(), sha)

    def test_short_token_refuses_to_start(self):
        (self.tmp / "token").write_text("short\n")
        result = subprocess.run([sys.executable, str(DEPLOY), "--config", str(self.config), "serve"],
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("shorter than 32", result.stderr)

    # The DeployTest cases need no hook running; skip them here.
    test_first_deploy_builds_the_subdir_and_links_the_config = None
    test_same_commit_is_not_restarted_again = None
    test_failed_health_check_rolls_back = None
    test_build_failure_keeps_the_running_release = None
    test_old_releases_are_pruned = None


if __name__ == "__main__":
    unittest.main()
