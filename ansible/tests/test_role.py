"""domator_jail files rendered for both instances (tests/render.yml), checked
on the controller without the host."""
import json
import os
from pathlib import Path
import re
import subprocess
import tomllib
import unittest

import yaml

ANSIBLE = Path(__file__).resolve().parents[1]
BUILD = ANSIBLE / "build"
ROLE = ANSIBLE / "roles/domator_jail"
DEPLOY = (ROLE / "files/domator-deploy").read_text()
INSTANCES = {i["name"]: i for i in yaml.safe_load((ANSIBLE / "host_vars/virgo/main.yml").read_text())["domator_instances"]}


def setUpModule():
    env = os.environ | {"ANSIBLE_VARS_ENABLED": "host_group_vars", "ANSIBLE_NOCOLOR": "1"}
    subprocess.run(["ansible-playbook", "tests/render.yml"], cwd=ANSIBLE, env=env, check=True,
                   stdout=subprocess.DEVNULL)


class InstancesTest(unittest.TestCase):
    def test_turbacz_and_czupel_share_the_role(self):
        self.assertEqual(sorted(INSTANCES), ["czupel", "turbacz"])
        self.assertEqual((INSTANCES["turbacz"]["branch"], INSTANCES["czupel"]["branch"]), ("main", "czupel"))
        # turbacz keeps the host account's uid; networks and uids never collide.
        self.assertEqual(INSTANCES["turbacz"]["uid"], 1432)
        self.assertEqual(len({i["net"] for i in INSTANCES.values()}), len(INSTANCES))
        self.assertEqual(len({i["uid"] for i in INSTANCES.values()}), len(INSTANCES))


class RenderedTest(unittest.TestCase):
    def files(self, name):
        return BUILD / name

    def test_config_points_at_the_jail_network(self):
        for name, inst in INSTANCES.items():
            net = inst["net"]
            cfg = tomllib.loads((self.files(name) / "turbacz.toml").read_text())
            self.assertEqual(cfg["psql"], {"dbname": name, "user": name, "password": f"pg-{name}",
                                           "host": f"192.168.{net}.10", "port": 5432})
            self.assertEqual(cfg["server"], {"host": f"192.168.{net}.11", "port": 8000,
                                             "forwarded_allow_ips": f"192.168.{net}.10"})
            self.assertEqual(cfg["oidc"]["redirect_uri"], f"https://{inst['domain']}/auth")
            self.assertEqual(cfg["security"]["allowed_origins"], [f"https://{inst['domain']}"])
            # VictoriaMetrics scrapes the jail by IP (scrape.yml).
            self.assertIn(f"192.168.{net}.11", cfg["security"]["allowed_hosts"])
            self.assertEqual(cfg["mqtt"]["host"], "192.168.3.10")
            self.assertEqual(cfg["mqtt"]["username"], name)
            self.assertEqual(cfg["monitoring"]["metrics"], f"http://192.168.{net}.10:8428")
            self.assertEqual(cfg["monitoring"]["labels"], {k: str(v) for k, v in inst["labels"].items()})
            self.assertEqual(cfg["firmware"]["directory"], "/var/db/domator/firmware")
            self.assertEqual(cfg["authorized"], ["a@example.com", "b@example.com"])
            # Quotes and backslashes in secrets survive the JSON-in-TOML quoting.
            self.assertEqual(cfg["session_secret"], 'sess"ion\\\\x')
            self.assertIs(cfg["ha"]["enabled"], False)

    def test_deployer_config_matches_what_the_deployer_reads(self):
        used = set(re.findall(r"""(?:cfg|config)\[["'](\w+)["']\]""", DEPLOY))
        for name, inst in INSTANCES.items():
            cfg = json.loads((self.files(name) / "deploy.json").read_text())
            self.assertLessEqual(used, set(cfg))
            self.assertEqual((cfg["branch"], cfg["base"], cfg["subdir"]), (inst["branch"], f"/home/{name}", "turbacz"))
            self.assertIsInstance(cfg["port"], int)
            self.assertEqual(cfg["listen"], f"192.168.{inst['net']}.11")
            self.assertEqual(cfg["restart"], ["/usr/local/bin/doas", "service", "domator", "restart"])
            self.assertEqual(cfg["health_host"], inst["domain"])

    def test_jail_takes_the_permanent_epair(self):
        for name, inst in INSTANCES.items():
            conf = (self.files(name) / "jail.conf").read_text()
            self.assertIn(f'vnet.interface = "epair{inst["net"]}b";', conf)
            self.assertIn(f'path = "/usr/local/jails/{name}";', conf)
            # PostgreSQL binds the host side at boot: the jail never destroys it.
            self.assertNotIn("destroy", conf)
            self.assertNotIn("create", conf)

    def test_pf_anchor_opens_only_what_the_app_needs(self):
        for name, inst in INSTANCES.items():
            rules = [l for l in (self.files(name) / "pf-anchor").read_text().splitlines()
                     if l and not l.startswith("#")]
            text = "\n".join(rules)
            self.assertIn(f'jail_if   = "epair{inst["net"]}a"', text)
            block = next(i for i, l in enumerate(rules) if l.startswith("block in"))
            passes_before = [l for l in rules[:block] if l.startswith("pass in")]
            self.assertEqual(passes_before, [
                "pass in quick on $jail_if inet proto icmp from $jail_ip to $gw_ip icmp-type echoreq keep state",
                "pass in quick on $jail_if inet proto tcp from $jail_ip to $gw_ip port { 5432, 8428 } keep state",
                "pass in quick on $jail_if inet proto tcp from $jail_ip to 192.168.3.10 port 1883 keep state",
            ])
            self.assertEqual(rules[block], "block in log quick on $jail_if inet from $jail_ip "
                                           "to { self, ($ext_if), $gw_ip, $home_nets }")
            self.assertIn("pass out quick on $jail_if inet proto tcp from $gw_ip to $jail_ip port { 8000, 9000 } keep state", rules)
            self.assertIn("nat on $ext_if inet from $jail_ip to any -> ($ext_if)", rules)

    def test_rc_scripts_are_valid_sh(self):
        for name in INSTANCES:
            for script in ("domator.rc", "domator_hook.rc", "domator-jail"):
                result = subprocess.run(["sh", "-n", str(self.files(name) / script)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, f"{name}/{script}: {result.stderr}")
            rc = (self.files(name) / "domator.rc").read_text()
            self.assertIn(f'domator_user="{name}"', rc)
            self.assertIn(f"/home/{name}/current/.venv/bin/turbacz", rc)


class WrapperTest(unittest.TestCase):
    """doas domator-jail: only admins, only domator jails, as themselves."""

    def setUp(self):
        self.bin = BUILD / "fake-bin"
        self.bin.mkdir(exist_ok=True)
        (self.bin / "id").write_text("#!/bin/sh\necho 0\n")
        (self.bin / "id").chmod(0o755)
        jexec = BUILD / "fake-jexec"
        jexec.write_text('#!/bin/sh\necho "jexec $*"\n')
        jexec.chmod(0o755)

    def run_wrapper(self, *args, user="antek"):
        env = {"PATH": f"{self.bin}:/usr/bin:/bin"} | ({"DOAS_USER": user} if user else {})
        return subprocess.run(["sh", str(BUILD / "turbacz/domator-jail"), *args],
                              env=env, capture_output=True, text=True)

    def test_admin_gets_a_login_shell_or_a_command(self):
        for user in ("dry", "antek"):
            self.assertEqual(self.run_wrapper("czupel", user=user).stdout,
                             f"jexec -l -U {user} czupel /bin/sh -l\n")
        self.assertEqual(self.run_wrapper("turbacz", "doas", "service", "domator", "status").stdout,
                         "jexec -l -U antek turbacz doas service domator status\n")

    def test_refusals(self):
        for args, user, message in [
            (("errands",), "antek", "unknown jail errands"),
            (("turbacz",), "zosia", "zosia is not a domator admin"),
            (("turbacz",), None, "run it through doas"),
            ((), "dry", "usage: doas domator-jail {turbacz|czupel}"),
        ]:
            result = self.run_wrapper(*args, user=user)
            self.assertEqual(result.returncode, 64, (args, user))
            self.assertIn(message, result.stderr)
            self.assertEqual(result.stdout, "")


class TasksTest(unittest.TestCase):
    def tasks(self, name):
        return yaml.safe_load((ROLE / "tasks" / name).read_text())

    def test_secrets_never_reach_the_output(self):
        for task in self.tasks("app.yml"):
            text = json.dumps(task)
            if "domator_secrets" in text or "dj_secret" in text:
                self.assertTrue(task.get("no_log"), task["name"])

    def test_jail_doas_lets_the_service_user_restart_only(self):
        doas = next(t for t in self.tasks("jail.yml") if t["name"] == "doas.conf in the jail")
        lines = [l for l in doas["ansible.builtin.copy"]["content"].splitlines() if not l.startswith("#")]
        self.assertEqual(lines, ["permit nopass :wheel",
                                 "permit nopass {{ dj_name }} as root cmd service args domator restart"])

    def test_epair_is_created_without_restarting_the_network(self):
        text = (ROLE / "tasks/host.yml").read_text()
        self.assertIn("ifconfig {{ dj_epair }} create", text)
        for forbidden in ("netif", "service routing", "/etc/netstart"):
            self.assertNotIn(forbidden, text)


if __name__ == "__main__":
    unittest.main()
