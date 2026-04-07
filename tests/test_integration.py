#!/usr/bin/env python3
"""
Integration tests for cnet-cli.

Runs cnet-cli commands against a live CNet BBS via amigactl exec and validates
the JSON output. Non-destructive tests run unconditionally. Mutation tests
(create/edit/delete) create test data, validate, and clean up.

Usage:
    python3 tests/test_integration.py [--host 192.168.6.228] [--skip-mutations]
"""

import json
import subprocess
import sys
import time

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

DEFAULT_HOST = "192.168.6.228"
AMIGACTL_PATH = "/home/thomas/ClaudeProjects/amigactl/client"
CLI_BINARY = "C:cnet-cli"

# Test subboard GO keys -- prefixed so we can detect and clean orphans.
TEST_PREFIX = "_test_"
TEST_PARENT_GO = "_test_parent"
TEST_CHILD1_GO = "_test_child1"
TEST_CHILD2_GO = "_test_child2"
TEST_SINGLE_GO = "_test_single"
TEST_EDIT_GO = "_test_edit"
TEST_MSGBOARD_GO = "_test_msgboard"
TEST_ACCESS_GO = "_test_access"
TEST_BOOL_GO = "_test_bools"
TEST_OBITS_GO = "_test_obits"

# Known facts about the live BBS for validation.
EXPECTED_SERIAL = 26031402
EXPECTED_REGISTERED_TO = "Thomas Dye"
EXPECTED_PORTS = 6
EXPECTED_ROOT_SUB = 4

# Valid marker names for door types.
DOOR_MARKER_NAMES = {
    "TextDoor", "TextFile", "CNetCDoor", "ARexxDoor",
    "ADosDoor", "BBSMacro", "DirCommander",
}


# ---------------------------------------------------------------------------
# Test framework
# ---------------------------------------------------------------------------

class TestRunner:
    """Minimal TAP-like test runner."""

    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.skipped = 0
        self.results = []

    def record(self, name, ok, detail=""):
        status = "PASS" if ok else "FAIL"
        self.results.append((name, status, detail))
        if ok:
            self.passed += 1
        else:
            self.failed += 1
        tag = status
        msg = f"  {tag}  {name}"
        if detail:
            msg += f"  --  {detail}"
        print(msg, flush=True)

    def skip(self, name, reason=""):
        self.skipped += 1
        self.results.append((name, "SKIP", reason))
        msg = f"  SKIP  {name}"
        if reason:
            msg += f"  --  {reason}"
        print(msg, flush=True)

    def summary(self):
        total = self.passed + self.failed + self.skipped
        print()
        print("=" * 60)
        print(f"  Total: {total}  Passed: {self.passed}  "
              f"Failed: {self.failed}  Skipped: {self.skipped}")
        print("=" * 60)
        if self.failed > 0:
            print()
            print("Failures:")
            for name, status, detail in self.results:
                if status == "FAIL":
                    print(f"  - {name}: {detail}")
        return 0 if self.failed == 0 else 1


runner = TestRunner()


# ---------------------------------------------------------------------------
# Helper functions
# ---------------------------------------------------------------------------

def run_cli(args, expect_error=False, host=DEFAULT_HOST):
    """
    Run a cnet-cli command via amigactl exec.

    Returns (parsed_json_or_None, stdout_text, exit_code).

    amigactl exec captures stdout only. Error JSON goes to Amiga stderr,
    which is not captured. For error cases we can only check exit_code.
    """
    cmd_str = f'{CLI_BINARY} {args}'
    full_cmd = [
        sys.executable, "-m", "amigactl",
        "--host", host,
        "exec", cmd_str,
    ]
    env = {"PYTHONPATH": AMIGACTL_PATH}
    # Inherit PATH so python3 can find itself and system libs.
    import os
    env["PATH"] = os.environ.get("PATH", "/usr/bin:/bin")
    env["HOME"] = os.environ.get("HOME", "/root")

    try:
        result = subprocess.run(
            full_cmd,
            capture_output=True,
            text=True,
            timeout=30,
            env=env,
        )
    except subprocess.TimeoutExpired:
        return None, "", -1

    stdout = result.stdout.strip()
    exit_code = result.returncode

    if expect_error:
        return None, stdout, exit_code

    if exit_code != 0:
        return None, stdout, exit_code

    # Try to parse JSON from stdout.
    if not stdout:
        return None, stdout, exit_code

    try:
        data = json.loads(stdout)
    except json.JSONDecodeError:
        return None, stdout, exit_code

    return data, stdout, exit_code


def assert_keys(obj, required_keys, test_name):
    """Verify all required keys are present in a dict. Returns True if ok."""
    if not isinstance(obj, dict):
        runner.record(test_name, False, f"Expected dict, got {type(obj).__name__}")
        return False
    missing = [k for k in required_keys if k not in obj]
    if missing:
        runner.record(test_name, False, f"Missing keys: {missing}")
        return False
    return True


def assert_eq(actual, expected, test_name):
    """Assert equality. Records result."""
    ok = actual == expected
    detail = "" if ok else f"expected {expected!r}, got {actual!r}"
    runner.record(test_name, ok, detail)
    return ok


def assert_true(condition, test_name, detail=""):
    """Assert a boolean condition. Records result."""
    runner.record(test_name, bool(condition), detail if not condition else "")
    return bool(condition)


def count_tree_nodes(tree):
    """Recursively count nodes in a tree array."""
    count = 0
    for node in tree:
        count += 1
        if "children" in node and isinstance(node["children"], list):
            count += count_tree_nodes(node["children"])
    return count


def collect_tree_physnums(tree):
    """Recursively collect all physnums from a tree array."""
    result = []
    for node in tree:
        if "physnum" in node:
            result.append(node["physnum"])
        if "children" in node and isinstance(node["children"], list):
            result.extend(collect_tree_physnums(node["children"]))
    return result


def find_in_tree(tree, physnum):
    """Check if a physnum appears anywhere in the tree. Returns True/False."""
    for node in tree:
        if node.get("physnum") == physnum:
            return True
        if "children" in node and isinstance(node["children"], list):
            if find_in_tree(node["children"], physnum):
                return True
    return False


def find_parent_in_tree(tree, physnum, parent_physnum=None):
    """
    Find the parent physnum of a given physnum in the tree.
    Returns the parent's physnum, or None if physnum is at the root level.
    """
    for node in tree:
        if node.get("physnum") == physnum:
            return parent_physnum
        if "children" in node and isinstance(node["children"], list):
            result = find_parent_in_tree(
                node["children"], physnum, node.get("physnum"))
            if result is not None:
                return result
    return None


def validate_tree_integrity(test_label, host):
    """
    Run sub tree and sub list --active, verify consistency.
    Returns (tree_data, active_list_data) or (None, None) on failure.
    """
    tree_data, _, tree_rc = run_cli("sub tree", host=host)
    list_data, _, list_rc = run_cli("sub list --active", host=host)

    if tree_rc != 0 or tree_data is None:
        runner.record(f"{test_label}: tree fetch", False,
                      "sub tree returned error or no data")
        return None, None

    if list_rc != 0 or list_data is None:
        runner.record(f"{test_label}: active list fetch", False,
                      "sub list --active returned error or no data")
        return None, None

    tree_arr = tree_data.get("tree", [])
    active_subs = list_data.get("subboards", [])

    # Node count should match active subboard count.
    tree_count = count_tree_nodes(tree_arr)
    active_count = len(active_subs)
    assert_eq(tree_count, active_count,
              f"{test_label}: tree nodes == active subboards "
              f"({tree_count} vs {active_count})")

    # No duplicate physnums in tree.
    tree_physnums = collect_tree_physnums(tree_arr)
    unique_physnums = set(tree_physnums)
    assert_eq(len(tree_physnums), len(unique_physnums),
              f"{test_label}: no duplicate physnums in tree")

    # All tree physnums should be valid (exist in full list).
    full_list_data, _, _ = run_cli("sub list", host=host)
    if full_list_data:
        all_physnums = {s["physnum"] for s in full_list_data.get("subboards", [])}
        invalid = [p for p in tree_physnums if p not in all_physnums]
        assert_true(len(invalid) == 0,
                    f"{test_label}: all tree physnums valid",
                    f"invalid physnums: {invalid}" if invalid else "")

    return tree_data, list_data


# ---------------------------------------------------------------------------
# Cleanup: remove orphaned test subboards from previous runs
# ---------------------------------------------------------------------------

def cleanup_test_subboards(host):
    """
    Scan for subboards with GO keys starting with _test_ and delete them.
    Processes children before parents to avoid --force complications.
    """
    print("--- Cleanup: checking for orphaned test subboards ---", flush=True)
    data, _, rc = run_cli("sub list", host=host)
    if rc != 0 or data is None:
        print("  WARNING: could not fetch subboard list for cleanup",
              flush=True)
        return

    test_subs = []
    for sub in data.get("subboards", []):
        if (sub.get("go_key", "").startswith(TEST_PREFIX)
                and not sub.get("killed", False)):
            test_subs.append(sub)

    if not test_subs:
        print("  No orphaned test subboards found.", flush=True)
        return

    print(f"  Found {len(test_subs)} orphaned test subboard(s):", flush=True)
    for sub in test_subs:
        print(f"    physnum={sub['physnum']} go_key={sub['go_key']!r}",
              flush=True)

    # Delete leaves first (no children), then parents.
    # Repeat until all are gone or we get stuck.
    max_passes = 5
    for pass_num in range(max_passes):
        # Re-fetch to get current state.
        data, _, rc = run_cli("sub list --active", host=host)
        if rc != 0 or data is None:
            break

        remaining = []
        for sub in data.get("subboards", []):
            if sub.get("go_key", "").startswith(TEST_PREFIX):
                remaining.append(sub)

        if not remaining:
            break

        deleted_any = False
        for sub in remaining:
            physnum = sub["physnum"]
            has_children = sub.get("child", -1) >= 0
            if has_children:
                # Try --force deletion.
                _, _, drc = run_cli(
                    f'sub delete {physnum} --force',
                    expect_error=True, host=host)
            else:
                _, _, drc = run_cli(
                    f'sub delete {physnum}',
                    expect_error=True, host=host)
            if drc == 0:
                deleted_any = True
                print(f"    Deleted orphan physnum={physnum} "
                      f"go_key={sub['go_key']!r}", flush=True)

        if not deleted_any:
            print("  WARNING: could not delete remaining test subboards",
                  flush=True)
            break

    print("--- Cleanup complete ---", flush=True)
    print(flush=True)


# ---------------------------------------------------------------------------
# System Status
# ---------------------------------------------------------------------------

def test_status(host):
    print("=== System Status ===", flush=True)

    data, raw, rc = run_cli("status", host=host)

    assert_true(rc == 0, "status: exit code 0", f"got {rc}")
    assert_true(data is not None, "status: valid JSON",
                f"raw output: {raw[:200]!r}" if data is None else "")

    if data is None:
        return

    required = [
        "system_name", "sysop_name", "version", "serial",
        "registered_to", "ports", "hi_port", "accounts",
        "total_calls", "logged_now", "subboards", "root_sub",
        "open_pfiles",
    ]
    if assert_keys(data, required, "status: all expected keys present"):
        assert_eq(data["serial"], EXPECTED_SERIAL,
                  "status: serial == 26031402")
        assert_eq(data["registered_to"], EXPECTED_REGISTERED_TO,
                  "status: registered_to == Thomas Dye")
        assert_eq(data["ports"], EXPECTED_PORTS,
                  "status: ports == 6")
        assert_true(isinstance(data["version"], int) and data["version"] > 0,
                    "status: version is positive int")
        assert_true(isinstance(data["hi_port"], int) and data["hi_port"] >= 0,
                    "status: hi_port >= 0")
        assert_true(isinstance(data["accounts"], int) and data["accounts"] >= 0,
                    "status: accounts >= 0")
        assert_true(isinstance(data["total_calls"], int)
                    and data["total_calls"] >= 0,
                    "status: total_calls >= 0")
        assert_true(isinstance(data["logged_now"], int)
                    and data["logged_now"] >= 0,
                    "status: logged_now >= 0")
        assert_true(isinstance(data["subboards"], int)
                    and data["subboards"] > 0,
                    "status: subboards > 0")
        assert_eq(data["root_sub"], EXPECTED_ROOT_SUB,
                  "status: root_sub == 4")
        assert_true(isinstance(data["open_pfiles"], int)
                    and data["open_pfiles"] >= 0,
                    "status: open_pfiles >= 0")


def test_ports(host):
    data, raw, rc = run_cli("ports", host=host)

    assert_true(rc == 0, "ports: exit code 0", f"got {rc}")
    assert_true(data is not None, "ports: valid JSON",
                f"raw output: {raw[:200]!r}" if data is None else "")

    if data is None:
        return

    assert_true("ports" in data, "ports: has 'ports' key")
    ports = data.get("ports", [])
    assert_true(isinstance(ports, list), "ports: 'ports' is an array")
    assert_true(
        len(ports) == EXPECTED_PORTS or len(ports) <= EXPECTED_PORTS,
        f"ports: array length <= {EXPECTED_PORTS}",
        f"got {len(ports)}")

    port_keys = ["port", "loaded", "online", "baud"]
    for i, port in enumerate(ports):
        if not assert_keys(port, port_keys, f"ports[{i}]: has required keys"):
            continue
        assert_true(isinstance(port["port"], int),
                    f"ports[{i}]: port is int")
        assert_true(isinstance(port["loaded"], bool),
                    f"ports[{i}]: loaded is bool")
        assert_true(isinstance(port["online"], bool),
                    f"ports[{i}]: online is bool")

        # Online ports must have user and account keys.
        if port["online"]:
            assert_true("user" in port,
                        f"ports[{i}]: online port has 'user'")
            assert_true("account" in port,
                        f"ports[{i}]: online port has 'account'")


def test_who(host):
    data, raw, rc = run_cli("who", host=host)

    assert_true(rc == 0, "who: exit code 0", f"got {rc}")
    assert_true(data is not None, "who: valid JSON",
                f"raw output: {raw[:200]!r}" if data is None else "")

    if data is None:
        return

    assert_true("users" in data, "who: has 'users' key")
    users = data.get("users", [])
    assert_true(isinstance(users, list), "who: 'users' is an array")

    user_keys = [
        "handle", "port", "account", "idle_minutes",
        "time_online_minutes",
    ]
    for i, user in enumerate(users):
        if not assert_keys(user, user_keys, f"who: user[{i}] has required keys"):
            continue
        assert_true(isinstance(user["handle"], str),
                    f"who: user[{i}] handle is string")
        assert_true(isinstance(user["port"], int),
                    f"who: user[{i}] port is int")
        assert_true(isinstance(user["account"], int),
                    f"who: user[{i}] account is int")
        # location may be null.
        assert_true(
            "location" in user,
            f"who: user[{i}] has 'location' key")


# ---------------------------------------------------------------------------
# Subboard Reads
# ---------------------------------------------------------------------------

def test_sub_list(host):
    print(flush=True)
    print("=== Subboard Reads ===", flush=True)

    data, raw, rc = run_cli("sub list", host=host)

    assert_true(rc == 0, "sub list: exit code 0", f"got {rc}")
    assert_true(data is not None, "sub list: valid JSON",
                f"raw output: {raw[:200]!r}" if data is None else "")

    if data is None:
        return None

    assert_true("subboards" in data, "sub list: has 'subboards' key")
    subs = data.get("subboards", [])
    assert_true(isinstance(subs, list) and len(subs) > 0,
                "sub list: subboards is non-empty array")

    sub_keys = [
        "physnum", "title", "go_key", "marker", "marker_name",
        "killed", "root", "parent", "child", "next", "data_path",
        "users", "next_id", "item_count", "access", "serial",
    ]
    for i, sub in enumerate(subs[:3]):  # Spot-check first 3.
        assert_keys(sub, sub_keys, f"sub list[{i}]: has all required keys")

    return data


def test_sub_list_active(host):
    data, _, rc = run_cli("sub list --active", host=host)

    assert_true(rc == 0, "sub list --active: exit code 0", f"got {rc}")
    assert_true(data is not None, "sub list --active: valid JSON")

    if data is None:
        return

    subs = data.get("subboards", [])
    killed_subs = [s for s in subs if s.get("killed", False)]
    assert_eq(len(killed_subs), 0,
              "sub list --active: no killed subboards in output")


def test_sub_list_type_msg(host):
    data, _, rc = run_cli("sub list --type msg", host=host)

    assert_true(rc == 0, "sub list --type msg: exit code 0", f"got {rc}")
    assert_true(data is not None, "sub list --type msg: valid JSON")

    if data is None:
        return

    subs = data.get("subboards", [])
    assert_true(len(subs) > 0, "sub list --type msg: non-empty result")
    non_msg = [s for s in subs if s.get("marker_name") != "MsgBase"]
    assert_eq(len(non_msg), 0,
              "sub list --type msg: all marker_name == MsgBase")


def test_sub_list_type_file(host):
    data, _, rc = run_cli("sub list --type file", host=host)

    assert_true(rc == 0, "sub list --type file: exit code 0", f"got {rc}")
    assert_true(data is not None, "sub list --type file: valid JSON")

    if data is None:
        return

    subs = data.get("subboards", [])
    # May be empty if no file subs exist -- just validate types if present.
    non_file = [s for s in subs if s.get("marker_name") != "FileTxfer"]
    assert_eq(len(non_file), 0,
              "sub list --type file: all marker_name == FileTxfer")


def test_sub_list_type_door(host):
    data, _, rc = run_cli("sub list --type door", host=host)

    assert_true(rc == 0, "sub list --type door: exit code 0", f"got {rc}")
    assert_true(data is not None, "sub list --type door: valid JSON")

    if data is None:
        return

    subs = data.get("subboards", [])
    non_door = [s for s in subs
                if s.get("marker_name") not in DOOR_MARKER_NAMES]
    assert_eq(len(non_door), 0,
              "sub list --type door: all marker_name in door types")


def test_sub_show_by_number(host):
    # Show subboard 0 (always exists).
    data, raw, rc = run_cli("sub show 0", host=host)

    assert_true(rc == 0, "sub show <number>: exit code 0", f"got {rc}")
    assert_true(data is not None, "sub show <number>: valid JSON",
                f"raw output: {raw[:200]!r}" if data is None else "")

    if data is None:
        return

    # Summary keys from emit_sub_summary.
    summary_keys = [
        "physnum", "title", "go_key", "marker", "marker_name",
        "killed", "root", "parent", "child", "next", "data_path",
        "users", "next_id", "item_count", "access", "serial",
    ]
    # Detail keys from emit_sub_detail.
    detail_keys = [
        "subdirectory", "closed", "max_items",
        "post_access", "respond_access", "upload_access",
        "download_access", "real_names", "anonymous",
        "private_area", "no_mci", "subvalid",
        "subop_ids", "subop_accs", "last_upload", "last_message",
    ]
    all_keys = summary_keys + detail_keys
    assert_keys(data, all_keys, "sub show <number>: has all summary+detail keys")
    assert_eq(data.get("physnum"), 0, "sub show <number>: physnum == 0")


def test_sub_show_by_gokey(host):
    # First get a known GO key from sub list.
    list_data, _, rc = run_cli("sub list --active", host=host)
    if rc != 0 or list_data is None:
        runner.skip("sub show <gokey>", "could not fetch sub list")
        return

    subs = list_data.get("subboards", [])
    if not subs:
        runner.skip("sub show <gokey>", "no active subboards")
        return

    # Pick the first subboard with a non-empty GO key.
    target = None
    for s in subs:
        gokey = s.get("go_key", "")
        if gokey and not gokey.startswith(TEST_PREFIX):
            target = s
            break

    if target is None:
        runner.skip("sub show <gokey>", "no suitable GO key found")
        return

    gokey = target["go_key"]
    expected_physnum = target["physnum"]

    data, _, rc = run_cli(f'sub show {gokey}', host=host)

    assert_true(rc == 0, f"sub show <gokey={gokey}>: exit code 0", f"got {rc}")
    assert_true(data is not None, f"sub show <gokey={gokey}>: valid JSON")

    if data is None:
        return

    assert_eq(data.get("physnum"), expected_physnum,
              f"sub show <gokey={gokey}>: resolves to physnum {expected_physnum}")

    detail_keys = [
        "subdirectory", "closed", "max_items",
        "post_access", "respond_access", "upload_access",
        "download_access", "real_names", "anonymous",
        "private_area", "no_mci", "subvalid",
        "subop_ids", "subop_accs", "last_upload", "last_message",
    ]
    assert_keys(data, detail_keys,
                f"sub show <gokey={gokey}>: has detail keys")


def test_sub_show_nonexistent(host):
    _, _, rc = run_cli("sub show _no_such_gokey_ever_", expect_error=True,
                       host=host)
    assert_true(rc != 0, "sub show nonexistent: exit code != 0",
                f"got {rc}")


def test_sub_tree(host):
    data, raw, rc = run_cli("sub tree", host=host)

    assert_true(rc == 0, "sub tree: exit code 0", f"got {rc}")
    assert_true(data is not None, "sub tree: valid JSON",
                f"raw output: {raw[:200]!r}" if data is None else "")

    if data is None:
        return

    assert_true("tree" in data, "sub tree: has 'tree' key")
    tree = data.get("tree", [])
    assert_true(isinstance(tree, list) and len(tree) > 0,
                "sub tree: tree is non-empty array")

    # Check tree node structure.
    if tree:
        node = tree[0]
        node_keys = [
            "physnum", "title", "go_key", "marker_name",
            "root", "subdirectory", "children",
        ]
        assert_keys(node, node_keys,
                    "sub tree: root node has expected keys")
        assert_true(node.get("root") is True,
                    "sub tree: first node is root",
                    f"got root={node.get('root')}")
        assert_eq(node.get("physnum"), EXPECTED_ROOT_SUB,
                  "sub tree: root physnum matches expected")

    # Cross-check node count against active subboard count.
    list_data, _, list_rc = run_cli("sub list --active", host=host)
    if list_rc == 0 and list_data is not None:
        active_count = len(list_data.get("subboards", []))
        tree_count = count_tree_nodes(tree)
        assert_eq(tree_count, active_count,
                  f"sub tree: node count ({tree_count}) == "
                  f"active subboards ({active_count})")

    # No duplicate physnums.
    physnums = collect_tree_physnums(tree)
    unique = set(physnums)
    assert_eq(len(physnums), len(unique),
              "sub tree: no duplicate physnums")


# ---------------------------------------------------------------------------
# Subboard Mutations
# ---------------------------------------------------------------------------

def test_sub_create_edit_delete(host):
    """Create a test subboard, edit it, delete it."""
    print(flush=True)
    print("=== Subboard Mutations ===", flush=True)

    # --- Create ---
    create_args = (
        f'sub create '
        f'--title "Test Single Board" '
        f'--go {TEST_SINGLE_GO} '
        f'--type msg '
        f'--parent {EXPECTED_ROOT_SUB}'
    )
    data, raw, rc = run_cli(create_args, host=host)

    assert_true(rc == 0, "sub create: exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    assert_true(data is not None, "sub create: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is None:
        return

    assert_eq(data.get("status"), "created", "sub create: status == created")
    new_physnum = data.get("physnum")
    assert_true(new_physnum is not None and isinstance(new_physnum, int),
                "sub create: physnum returned",
                f"got {new_physnum!r}")
    assert_eq(data.get("go_key"), TEST_SINGLE_GO,
              "sub create: go_key matches")
    assert_eq(data.get("marker_name"), "MsgBase",
              "sub create: marker_name == MsgBase")
    assert_eq(data.get("parent"), EXPECTED_ROOT_SUB,
              "sub create: parent is root")

    if new_physnum is None:
        return

    # Verify it appears in sub list.
    list_data, _, _ = run_cli("sub list --active", host=host)
    if list_data:
        physnums = [s["physnum"] for s in list_data.get("subboards", [])]
        assert_true(new_physnum in physnums,
                    "sub create: new board in active list")

    # Verify tree linkage.
    tree_data, _ = validate_tree_integrity("sub create", host)
    if tree_data:
        tree = tree_data.get("tree", [])
        assert_true(find_in_tree(tree, new_physnum),
                    "sub create: new board in tree")

    # --- Edit ---
    edit_args = (
        f'sub edit {new_physnum} '
        f'--title "Test Single Edited" '
        f'--go {TEST_EDIT_GO} '
        f'--closed true '
        f'--max-items 100'
    )
    data, raw, rc = run_cli(edit_args, host=host)

    assert_true(rc == 0, "sub edit: exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    assert_true(data is not None, "sub edit: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is not None:
        assert_eq(data.get("status"), "updated",
                  "sub edit: status == updated")
        assert_eq(data.get("title"), "Test Single Edited",
                  "sub edit: title changed")
        assert_eq(data.get("go_key"), TEST_EDIT_GO,
                  "sub edit: go_key changed")
        assert_true(data.get("closed") is True,
                    "sub edit: closed == true",
                    f"got {data.get('closed')!r}")
        assert_eq(data.get("max_items"), 100,
                  "sub edit: max_items == 100")

    # Verify via sub show (use the new GO key).
    show_data, _, show_rc = run_cli(f'sub show {TEST_EDIT_GO}', host=host)
    if show_rc == 0 and show_data is not None:
        assert_eq(show_data.get("physnum"), new_physnum,
                  "sub edit: show resolves edited GO key to same physnum")
        assert_eq(show_data.get("title"), "Test Single Edited",
                  "sub edit: show confirms title change")
        assert_true(show_data.get("closed") is True,
                    "sub edit: show confirms closed=true")

    # --- Delete ---
    data, raw, rc = run_cli(f'sub delete {new_physnum}', host=host)

    assert_true(rc == 0, "sub delete: exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    assert_true(data is not None, "sub delete: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is not None:
        assert_eq(data.get("status"), "deleted",
                  "sub delete: status == deleted")
        assert_eq(data.get("physnum"), new_physnum,
                  "sub delete: correct physnum")

    # Verify it's gone from --active list.
    list_data, _, _ = run_cli("sub list --active", host=host)
    if list_data:
        active_physnums = [s["physnum"]
                           for s in list_data.get("subboards", [])]
        assert_true(new_physnum not in active_physnums,
                    "sub delete: board not in active list")

    # Verify tree integrity after delete.
    validate_tree_integrity("sub delete", host)


def test_sub_force_delete_reparent(host):
    """
    Create parent + 2 children, delete parent with --force,
    verify children reparented to grandparent, clean up.
    """
    print(flush=True)
    print("--- Force delete with reparent ---", flush=True)

    created_physnums = []

    # Create parent under root.
    data, _, rc = run_cli(
        f'sub create --title "Test Parent" --go {TEST_PARENT_GO} '
        f'--type subdir --parent {EXPECTED_ROOT_SUB}',
        host=host)

    if rc != 0 or data is None:
        runner.record("force delete: create parent", False,
                      f"exit code {rc}")
        return

    parent_physnum = data.get("physnum")
    assert_true(parent_physnum is not None,
                "force delete: parent created",
                f"physnum={parent_physnum}")
    if parent_physnum is None:
        return
    created_physnums.append(parent_physnum)

    # Create child1 under parent.
    data, _, rc = run_cli(
        f'sub create --title "Test Child 1" --go {TEST_CHILD1_GO} '
        f'--type msg --parent {parent_physnum}',
        host=host)

    if rc != 0 or data is None:
        runner.record("force delete: create child1", False,
                      f"exit code {rc}")
        _cleanup_physnums(created_physnums, host)
        return

    child1_physnum = data.get("physnum")
    assert_true(child1_physnum is not None,
                "force delete: child1 created",
                f"physnum={child1_physnum}")
    if child1_physnum is None:
        _cleanup_physnums(created_physnums, host)
        return
    created_physnums.append(child1_physnum)

    # Create child2 under parent.
    data, _, rc = run_cli(
        f'sub create --title "Test Child 2" --go {TEST_CHILD2_GO} '
        f'--type msg --parent {parent_physnum}',
        host=host)

    if rc != 0 or data is None:
        runner.record("force delete: create child2", False,
                      f"exit code {rc}")
        _cleanup_physnums(created_physnums, host)
        return

    child2_physnum = data.get("physnum")
    assert_true(child2_physnum is not None,
                "force delete: child2 created",
                f"physnum={child2_physnum}")
    if child2_physnum is None:
        _cleanup_physnums(created_physnums, host)
        return
    created_physnums.append(child2_physnum)

    # Verify parent has children in tree.
    tree_data, _ = validate_tree_integrity("force delete: pre-delete", host)
    if tree_data:
        tree = tree_data.get("tree", [])
        assert_true(find_in_tree(tree, parent_physnum),
                    "force delete: parent in tree")
        assert_true(find_in_tree(tree, child1_physnum),
                    "force delete: child1 in tree")
        assert_true(find_in_tree(tree, child2_physnum),
                    "force delete: child2 in tree")

    # Try delete parent WITHOUT --force (should fail).
    _, _, rc = run_cli(f'sub delete {parent_physnum}',
                       expect_error=True, host=host)
    assert_true(rc != 0,
                "force delete: delete parent without --force fails",
                f"got exit code {rc}")

    # Delete parent WITH --force.
    data, raw, rc = run_cli(f'sub delete {parent_physnum} --force',
                            host=host)
    assert_true(rc == 0, "force delete: delete parent --force succeeds",
                f"got {rc}, raw: {raw[:200]!r}")

    if rc == 0 and data is not None:
        assert_eq(data.get("status"), "deleted",
                  "force delete: status == deleted")

    # Verify parent is gone from active list.
    list_data, _, _ = run_cli("sub list --active", host=host)
    if list_data:
        active_physnums = [s["physnum"]
                           for s in list_data.get("subboards", [])]
        assert_true(parent_physnum not in active_physnums,
                    "force delete: parent gone from active list")

        # Children should still be active.
        assert_true(child1_physnum in active_physnums,
                    "force delete: child1 still active")
        assert_true(child2_physnum in active_physnums,
                    "force delete: child2 still active")

    # Verify children reparented to root (the grandparent).
    child1_show, _, _ = run_cli(f'sub show {child1_physnum}', host=host)
    if child1_show:
        assert_eq(child1_show.get("parent"), EXPECTED_ROOT_SUB,
                  "force delete: child1 reparented to root")

    child2_show, _, _ = run_cli(f'sub show {child2_physnum}', host=host)
    if child2_show:
        assert_eq(child2_show.get("parent"), EXPECTED_ROOT_SUB,
                  "force delete: child2 reparented to root")

    # Verify tree integrity after reparent.
    tree_data, _ = validate_tree_integrity("force delete: post-reparent", host)
    if tree_data:
        tree = tree_data.get("tree", [])
        # Children should be in the tree.
        assert_true(find_in_tree(tree, child1_physnum),
                    "force delete: child1 in tree after reparent")
        assert_true(find_in_tree(tree, child2_physnum),
                    "force delete: child2 in tree after reparent")
        # Parent should NOT be in the tree.
        assert_true(not find_in_tree(tree, parent_physnum),
                    "force delete: parent not in tree")

    # Clean up: delete the reparented children.
    for phys in [child1_physnum, child2_physnum]:
        del_data, _, drc = run_cli(f'sub delete {phys}', host=host)
        if drc != 0:
            print(f"  WARNING: cleanup failed for physnum={phys} "
                  f"(exit code {drc})", flush=True)
        else:
            runner.record(
                f"force delete: cleanup child physnum={phys}",
                True)

    # Final tree integrity check after full cleanup.
    validate_tree_integrity("force delete: post-cleanup", host)


def test_sub_delete_root(host):
    """Attempting to delete the root subboard should fail."""
    print(flush=True)
    print("--- Error: delete root ---", flush=True)

    _, _, rc = run_cli(f'sub delete {EXPECTED_ROOT_SUB}',
                       expect_error=True, host=host)
    assert_true(rc != 0, "delete root: exit code != 0",
                f"got {rc}")


def _cleanup_physnums(physnums, host):
    """Emergency cleanup: try to delete a list of physnums."""
    for phys in reversed(physnums):
        run_cli(f'sub delete {phys} --force', expect_error=True, host=host)


# ---------------------------------------------------------------------------
# Message Operations
# ---------------------------------------------------------------------------

def test_msg_operations(host):
    """
    Create a test MsgBase subboard, exercise all msg commands, clean up.

    All message tests use a dedicated test board to avoid interference
    with production data. Cleanup runs in a finally block to ensure the
    test subboard is removed even when assertions fail.
    """
    print(flush=True)
    print("=== Message Operations ===", flush=True)

    test_physnum = None

    # --- Setup: create test message board ---
    create_args = (
        f'sub create '
        f'--title "Test MsgBoard" '
        f'--go {TEST_MSGBOARD_GO} '
        f'--type msg '
        f'--parent {EXPECTED_ROOT_SUB}'
    )
    data, raw, rc = run_cli(create_args, host=host)

    assert_true(rc == 0, "msg setup: create test board exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    assert_true(data is not None, "msg setup: create returns valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is None or rc != 0:
        print("  FATAL: cannot create test message board, "
              "skipping message tests", flush=True)
        return

    test_physnum = data.get("physnum")
    assert_true(test_physnum is not None and isinstance(test_physnum, int),
                "msg setup: test board physnum returned",
                f"got {test_physnum!r}")

    if test_physnum is None:
        return

    try:
        _run_msg_tests(host, test_physnum)
    finally:
        # --- Cleanup: delete test message board ---
        print(flush=True)
        print("--- Message cleanup ---", flush=True)
        del_data, _, drc = run_cli(f'sub delete {test_physnum}',
                                   host=host)
        if drc == 0:
            runner.record("msg cleanup: test board deleted", True)
        else:
            # May have children or other issue -- try force.
            del_data, _, drc = run_cli(
                f'sub delete {test_physnum} --force',
                expect_error=True, host=host)
            runner.record("msg cleanup: test board deleted (force)",
                          drc == 0, f"exit code {drc}")

        # Verify it is gone from active list.
        list_data, _, _ = run_cli("sub list --active", host=host)
        if list_data:
            active_physnums = [s["physnum"]
                               for s in list_data.get("subboards", [])]
            assert_true(test_physnum not in active_physnums,
                        "msg cleanup: test board gone from active list")


def _run_msg_tests(host, test_physnum):
    """Run all message tests against the test board."""
    go = TEST_MSGBOARD_GO

    # --- msg list (empty board) ---
    print(flush=True)
    print("--- msg list (empty board) ---", flush=True)

    data, raw, rc = run_cli(f'msg list {go}', host=host)

    assert_true(rc == 0, "msg list empty: exit code 0", f"got {rc}")
    assert_true(data is not None, "msg list empty: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is not None:
        list_keys = ["subboard", "physnum", "items", "total"]
        if assert_keys(data, list_keys,
                       "msg list empty: has required keys"):
            assert_true(isinstance(data["items"], list),
                        "msg list empty: items is array")
            assert_eq(len(data["items"]), 0,
                      "msg list empty: items is empty")
            assert_eq(data["total"], 0,
                      "msg list empty: total == 0")

    # --- msg post (first message) ---
    print(flush=True)
    print("--- msg post ---", flush=True)

    post1_args = (
        f'msg post {go} '
        f'--title "Test Msg 1" '
        f'--author 1 '
        f'--text "Hello world"'
    )
    data, raw, rc = run_cli(post1_args, host=host)

    assert_true(rc == 0, "msg post 1: exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    assert_true(data is not None, "msg post 1: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is not None:
        assert_eq(data.get("status"), "posted",
                  "msg post 1: status == posted")
        assert_eq(data.get("item_count"), 1,
                  "msg post 1: item_count == 1")
        assert_eq(data.get("title"), "Test Msg 1",
                  "msg post 1: title matches")
        assert_eq(data.get("by_account"), 1,
                  "msg post 1: by_account == 1")

    # Post second message from a different author (account 3).
    post2_args = (
        f'msg post {go} '
        f'--title "Test Msg 2" '
        f'--author 3 '
        f'--text "Second post"'
    )
    data, raw, rc = run_cli(post2_args, host=host)

    assert_true(rc == 0, "msg post 2: exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    if data is not None:
        assert_eq(data.get("item_count"), 2,
                  "msg post 2: item_count == 2")

    # --- msg list (after posts) ---
    print(flush=True)
    print("--- msg list (after posts) ---", flush=True)

    data, raw, rc = run_cli(f'msg list {go}', host=host)

    assert_true(rc == 0, "msg list posted: exit code 0", f"got {rc}")
    assert_true(data is not None, "msg list posted: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is not None:
        assert_eq(data.get("total"), 2,
                  "msg list posted: total == 2")

        items = data.get("items", [])
        assert_eq(len(items), 2,
                  "msg list posted: items array has 2 entries")

        if len(items) >= 2:
            # Check required keys on each item.
            item_keys = [
                "number", "index", "title", "by_account",
                "by_handle", "responses", "post_date", "killed", "size",
            ]
            for idx, item in enumerate(items):
                assert_keys(item, item_keys,
                            f"msg list posted: item[{idx}] has required keys")

            # Index values are sequential.
            assert_eq(items[0].get("index"), 1,
                      "msg list posted: item[0] index == 1")
            assert_eq(items[1].get("index"), 2,
                      "msg list posted: item[1] index == 2")

            # Handles match expected values.
            assert_eq(items[0].get("by_handle"), "Samoht",
                      "msg list posted: item[0] by_handle == Samoht")
            assert_eq(items[1].get("by_handle"), "Bender",
                      "msg list posted: item[1] by_handle == Bender")

    # --- msg read ---
    print(flush=True)
    print("--- msg read ---", flush=True)

    data, raw, rc = run_cli(f'msg read {go} 1', host=host)

    assert_true(rc == 0, "msg read 1: exit code 0", f"got {rc}")
    assert_true(data is not None, "msg read 1: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is not None:
        assert_true("item" in data, "msg read 1: has item key")

        item_obj = data.get("item", {})
        read_keys = [
            "number", "index", "title", "by_account", "by_handle",
            "text", "post_date", "responses", "killed", "size",
            "responses_list",
        ]
        if assert_keys(item_obj, read_keys,
                       "msg read 1: item has all required keys"):
            assert_eq(item_obj.get("text"), "Hello world",
                      "msg read 1: text matches posted content")
            assert_true(
                isinstance(item_obj.get("responses_list"), list),
                "msg read 1: responses_list is array")
            assert_eq(len(item_obj.get("responses_list", [])), 0,
                      "msg read 1: responses_list is empty")

    # --- msg respond ---
    print(flush=True)
    print("--- msg respond ---", flush=True)

    respond_args = (
        f'msg respond {go} 1 '
        f'--author 3 '
        f'--text "Nice post!"'
    )
    data, raw, rc = run_cli(respond_args, host=host)

    assert_true(rc == 0, "msg respond: exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    assert_true(data is not None, "msg respond: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is not None:
        assert_eq(data.get("status"), "responded",
                  "msg respond: status == responded")

    # Re-read item 1 to verify response was added.
    data, raw, rc = run_cli(f'msg read {go} 1', host=host)

    assert_true(rc == 0, "msg read after respond: exit code 0",
                f"got {rc}")

    if data is not None:
        item_obj = data.get("item", {})
        assert_eq(item_obj.get("responses"), 1,
                  "msg read after respond: responses == 1")

        resp_list = item_obj.get("responses_list", [])
        assert_eq(len(resp_list), 1,
                  "msg read after respond: responses_list has 1 entry")

        if len(resp_list) >= 1:
            resp = resp_list[0]
            resp_keys = ["by_handle", "text", "post_date"]
            assert_keys(resp, resp_keys,
                        "msg respond: response has required keys")
            assert_eq(resp.get("text"), "Nice post!",
                      "msg respond: response text matches")
            assert_eq(resp.get("by_handle"), "Bender",
                      "msg respond: response by_handle == Bender")

    # --- msg list with pagination ---
    print(flush=True)
    print("--- msg list pagination ---", flush=True)

    # --limit 1: should return only 1 item.
    data, _, rc = run_cli(f'msg list {go} --limit 1', host=host)

    assert_true(rc == 0, "msg list --limit 1: exit code 0", f"got {rc}")
    if data is not None:
        items = data.get("items", [])
        assert_eq(len(items), 1,
                  "msg list --limit 1: returns 1 item")
        # Total should still reflect the full count.
        assert_eq(data.get("total"), 2,
                  "msg list --limit 1: total still == 2")

    # --offset 1: should skip the first item.
    data, _, rc = run_cli(f'msg list {go} --offset 1', host=host)

    assert_true(rc == 0, "msg list --offset 1: exit code 0", f"got {rc}")
    if data is not None:
        items = data.get("items", [])
        assert_eq(len(items), 1,
                  "msg list --offset 1: returns 1 item")
        if len(items) >= 1:
            assert_eq(items[0].get("index"), 2,
                      "msg list --offset 1: first returned item index == 2")

    # --- msg delete ---
    print(flush=True)
    print("--- msg delete ---", flush=True)

    data, raw, rc = run_cli(f'msg delete {go} 2', host=host)

    assert_true(rc == 0, "msg delete: exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    assert_true(data is not None, "msg delete: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is not None:
        assert_eq(data.get("status"), "deleted",
                  "msg delete: status == deleted")

    # Verify killed flag via msg list.
    data, _, rc = run_cli(f'msg list {go}', host=host)

    assert_true(rc == 0, "msg list after delete: exit code 0",
                f"got {rc}")

    if data is not None:
        items = data.get("items", [])
        if len(items) >= 2:
            assert_true(items[1].get("killed") is True,
                        "msg delete: item 2 killed == true",
                        f"got {items[1].get('killed')!r}")
            assert_true(items[0].get("killed") is not True,
                        "msg delete: item 1 killed still false",
                        f"got {items[0].get('killed')!r}")

    # --- msg move (regression: verify normal move still works) ---
    print(flush=True)
    print("--- msg move ---", flush=True)

    # Need a second test board for the move destination.
    dst_go = "_test_msgdst"
    dst_data, _, drc = run_cli(
        f'sub create --title "Test Move Dst" '
        f'--go {dst_go} --type msg --parent {EXPECTED_ROOT_SUB}',
        host=host)

    if drc != 0 or not dst_data:
        runner.skip("msg move: cannot create destination board")
    else:
        dst_physnum = dst_data.get("physnum")
        try:
            # Post a message to source board.
            post_data, _, prc = run_cli(
                f'msg post {go} --title "Move Test" '
                f'--author 1 --text "Body for move test"',
                host=host)
            assert_true(prc == 0, "msg move: post to source",
                        f"rc={prc}")

            if prc == 0 and post_data:
                item_num = post_data.get("item_index", 1)

                # Add a response.
                resp_data, _, rrc = run_cli(
                    f'msg respond {go} {item_num} '
                    f'--author 1 --text "Response for move"',
                    host=host)
                assert_true(rrc == 0, "msg move: add response",
                            f"rc={rrc}")

                # Move to destination.
                move_data, move_raw, mrc = run_cli(
                    f'msg move {go} {item_num} {dst_go}',
                    host=host)
                assert_true(mrc == 0, "msg move: exit code 0",
                            f"rc={mrc}, raw={move_raw[:200]!r}")
                if move_data:
                    assert_eq(move_data.get("status"), "moved",
                              "msg move: status is moved")
                    assert_true(
                        move_data.get("destination", {})
                        .get("responses_copied", 0) >= 1,
                        "msg move: response copied",
                        f"got {move_data}")
        finally:
            # Cleanup destination board.
            run_cli(f'sub delete {dst_physnum} --force',
                    host=host)

    # --- Error cases ---
    print(flush=True)
    print("--- msg error cases ---", flush=True)

    # Out of range item number.
    _, _, rc = run_cli(f'msg read {go} 99', expect_error=True, host=host)
    assert_true(rc != 0, "msg read out of range: exit code != 0",
                f"got {rc}")

    # Post to a non-MsgBase subboard (use root sub, which is a subdir).
    _, _, rc = run_cli(
        f'msg post {EXPECTED_ROOT_SUB} '
        f'--title "x" --author 1 --text "x"',
        expect_error=True, host=host)
    assert_true(rc != 0, "msg post to non-MsgBase: exit code != 0",
                f"got {rc}")


# ---------------------------------------------------------------------------
# cnet4.library integration and bug fixes
# ---------------------------------------------------------------------------

def test_cnet4_library(host):
    """Verify cnet4.library is opened (no warning in status output)."""
    data, raw, rc = run_cli("status", host=host)
    assert_true(rc == 0, "cnet4: status exit code 0")
    # If cnet4.library failed to open, there would be a warning.
    # The status command itself doesn't use cnet4, but init_cnet()
    # emits a warning that propagates to warn_emit() in the response.
    if data and "warnings" in data:
        warnings = data["warnings"]
        has_cnet4_warn = any("cnet4" in w for w in warnings)
        assert_true(not has_cnet4_warn,
                    "cnet4: no cnet4.library warning in status",
                    f"warnings: {warnings}")
    else:
        runner.record("cnet4: no warnings (library opened OK)", True)


def test_olm_timestamp(host):
    """
    Send OLM via the direct I/O fallback path and verify delivery.

    This test requires a user online on a port. If no user is online,
    skip. Uses --from 2 (non-sysop) to force the direct I/O fallback
    where the timestamp offset bug lives.
    """
    # Check if any port has a user online.
    data, _, rc = run_cli("who", host=host)
    if rc != 0 or not data or not data.get("users"):
        runner.skip("olm timestamp: no users online for OLM test")
        return

    # Use the first online user's port.
    user = data["users"][0]
    port = user["port"]

    # Send OLM from account 2 (non-sysop) to force the direct I/O
    # fallback path. FileOLM only succeeds for account 1 (sysop)
    # from standalone CLI, so --from 2 exercises the code path where
    # the timestamp offset bug lives.
    olm_args = (
        f'olm {port} '
        f'--from 2 '
        f'--text "OLM timestamp test"'
    )
    data, raw, rc = run_cli(olm_args, host=host)
    assert_true(rc == 0, "olm timestamp: send exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    if data:
        assert_true(data.get("status") == "sent",
                    "olm timestamp: delivery status",
                    f"got {data.get('status')}")


# ---------------------------------------------------------------------------
# Extended read operations
# ---------------------------------------------------------------------------

def test_sub_show_subop_fields(host):
    """SubOpIDs and SubOpAccs arrays in sub show."""
    data, raw, rc = run_cli("sub show 0", host=host)
    assert_true(rc == 0, "sub show subop: exit code 0")
    assert_true(data is not None, "sub show subop: valid JSON")
    if data is None:
        return
    # SubOpIDs and SubOpAccs should be 6-element arrays
    assert_true("subop_ids" in data, "sub show subop: has subop_ids")
    assert_true("subop_accs" in data, "sub show subop: has subop_accs")
    ids = data.get("subop_ids", [])
    accs = data.get("subop_accs", [])
    assert_eq(len(ids), 6, "sub show subop: subop_ids has 6 elements")
    assert_eq(len(accs), 6, "sub show subop: subop_accs has 6 elements")


def test_sub_show_activity_dates(host):
    """LastUpload and LastMessage dates in sub show."""
    data, raw, rc = run_cli("sub show 0", host=host)
    assert_true(rc == 0, "sub show dates: exit code 0")
    assert_true(data is not None, "sub show dates: valid JSON")
    if data is None:
        return
    # LastUpload and LastMessage should be present (string or null)
    assert_true("last_upload" in data, "sub show dates: has last_upload")
    assert_true("last_message" in data, "sub show dates: has last_message")


def test_user_plan(host):
    """User plan file read."""
    # Test with account 1 (sysop -- always exists)
    data, raw, rc = run_cli("user plan 1", host=host)
    assert_true(rc == 0, "user plan: exit code 0", f"got {rc}")
    assert_true(data is not None, "user plan: valid JSON")
    if data is None:
        return
    assert_eq(data.get("account"), 1, "user plan: account == 1")
    assert_true("plan" in data, "user plan: has plan key")
    assert_true("uucp" in data, "user plan: has uucp key")


def test_user_plan_nonexistent(host):
    """User plan for nonexistent user."""
    _, _, rc = run_cli("user plan _no_such_user_", expect_error=True,
                       host=host)
    assert_true(rc != 0, "user plan nonexistent: nonzero exit code")


def test_stats_sam_labels(host):
    """SAM/SAG human-readable labels."""
    data, raw, rc = run_cli("stats", host=host)
    assert_true(rc == 0, "stats sam labels: exit code 0")
    assert_true(data is not None, "stats sam labels: valid JSON")
    if data is None:
        return
    sam = data.get("sam", {})
    assert_true("row_labels" in sam, "stats sam: has row_labels")
    assert_true("column_labels" in sam, "stats sam: has column_labels")
    assert_true("data" in sam, "stats sam: has data")
    assert_eq(len(sam.get("row_labels", [])), 5,
              "stats sam: 5 row labels")
    assert_eq(len(sam.get("column_labels", [])), 15,
              "stats sam: 15 column labels")

    sag = data.get("sag", {})
    assert_true("row_labels" in sag, "stats sag: has row_labels")
    assert_true("data" in sag, "stats sag: has data")
    assert_eq(len(sag.get("row_labels", [])), 2,
              "stats sag: 2 row labels")


def test_user_find_phone(host):
    """FindPhone search via --phone flag."""
    # FindPhone with an unlikely number -- just verify the command works
    data, raw, rc = run_cli("user find --phone 0000000", host=host)
    assert_true(rc == 0, "user find --phone: exit code 0", f"got {rc}")
    assert_true(data is not None, "user find --phone: valid JSON")
    if data is None:
        return
    assert_true("users" in data, "user find --phone: has users array")
    assert_true("matched" in data, "user find --phone: has matched count")


def test_conf_list(host):
    """Conference room listing."""
    data, raw, rc = run_cli("conf list", host=host)
    assert_true(rc == 0, "conf list: exit code 0", f"got {rc}")
    assert_true(data is not None, "conf list: valid JSON")
    if data is None:
        return
    assert_true("rooms" in data, "conf list: has rooms array")
    rooms = data.get("rooms", [])
    assert_true(isinstance(rooms, list), "conf list: rooms is array")


def test_conf_list_all(host):
    """Conference room listing with --all flag."""
    data, raw, rc = run_cli("conf list --all", host=host)
    assert_true(rc == 0, "conf list --all: exit code 0", f"got {rc}")
    assert_true(data is not None, "conf list --all: valid JSON")
    if data is None:
        return
    assert_true("rooms" in data, "conf list --all: has rooms array")


# ---------------------------------------------------------------------------
# Group edit and transpose
# ---------------------------------------------------------------------------

# Test group number -- use group 31 (typically unused).
TEST_GROUP = 31


def test_group_edit_transpose(host):
    """
    Test group edit and group transpose.

    Uses group 31 as a test group. Captures original values, modifies
    them, validates, then restores originals via try/finally.
    """
    print(flush=True)
    print("=== Group Edit / Transpose ===", flush=True)

    # --- Baseline: capture original group state ---
    orig_data, raw, rc = run_cli(f'group show {TEST_GROUP}', host=host)
    assert_true(rc == 0, "group show baseline: exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    assert_true(orig_data is not None, "group show baseline: valid JSON",
                f"raw: {raw[:200]!r}" if orig_data is None else "")

    if orig_data is None:
        return

    orig_name = orig_data.get("name", "")
    orig_privs = orig_data.get("privileges", {})
    orig_expire_days = orig_data.get("expire_days", 0)
    orig_expire_access = orig_data.get("expire_access", 0)
    orig_daily_minutes = orig_privs.get("daily_minutes", 0)
    orig_idle = orig_privs.get("idle_limit", 0)
    orig_abits = orig_privs.get("abits", "0x00000000")

    try:
        _test_group_edit_cases(host, orig_data)
        _test_group_transpose_cases(host)
    finally:
        # Restore original group state regardless of test outcome.
        _restore_group(host, orig_name, orig_expire_days,
                       orig_expire_access, orig_daily_minutes,
                       orig_idle, orig_abits, orig_privs)


def _restore_group(host, orig_name, orig_expire_days,
                   orig_expire_access, orig_daily_minutes,
                   orig_idle, orig_abits, orig_privs):
    """Restore test group to original state."""
    print("--- Restoring test group ---", flush=True)

    # Build restore command with all the fields we changed.
    # Name may be empty -- pass explicit empty string.
    name_arg = f'--name "{orig_name}"' if orig_name else '--name ""'

    restore_args = (
        f'group edit {TEST_GROUP} '
        f'{name_arg} '
        f'--expire-days {orig_expire_days} '
        f'--expire-access {orig_expire_access} '
        f'--daily-minutes {orig_daily_minutes} '
        f'--idle {orig_idle} '
        f'--abits {orig_abits}'
    )
    _, _, rc = run_cli(restore_args, host=host)
    if rc == 0:
        print("  Group restored.", flush=True)
    else:
        print(f"  WARNING: group restore failed (rc={rc})", flush=True)


def _test_group_edit_cases(host, orig_data):
    """Test cases: group edit."""

    # --- Edit name ---
    data, raw, rc = run_cli(
        f'group edit {TEST_GROUP} --name "CLI Test Group"', host=host)
    assert_true(rc == 0, "group edit name: exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    if data is not None:
        assert_eq(data.get("status"), "updated",
                  "group edit name: status == updated")
        assert_eq(data.get("name"), "CLI Test Group",
                  "group edit name: name changed")
        changed = data.get("fields_changed", [])
        assert_true("name" in changed,
                    "group edit name: fields_changed includes name",
                    f"got {changed!r}")

    # Verify via group show.
    show_data, _, show_rc = run_cli(
        f'group show {TEST_GROUP}', host=host)
    if show_rc == 0 and show_data is not None:
        assert_eq(show_data.get("name"), "CLI Test Group",
                  "group edit name: show confirms name change")

    # --- Edit numeric fields ---
    data, raw, rc = run_cli(
        f'group edit {TEST_GROUP} --daily-minutes 120 --idle 30',
        host=host)
    assert_true(rc == 0, "group edit numeric: exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    if data is not None:
        assert_eq(data.get("status"), "updated",
                  "group edit numeric: status == updated")
        privs = data.get("privileges", {})
        assert_eq(privs.get("daily_minutes"), 120,
                  "group edit numeric: daily_minutes == 120")
        assert_eq(privs.get("idle_limit"), 30,
                  "group edit numeric: idle == 30")
        changed = data.get("fields_changed", [])
        assert_true("daily_minutes" in changed and "idle_limit" in changed,
                    "group edit numeric: fields_changed correct",
                    f"got {changed!r}")

    # --- Edit hex bitmask ---
    data, raw, rc = run_cli(
        f'group edit {TEST_GROUP} --abits 0x0000001F', host=host)
    assert_true(rc == 0, "group edit hex: exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    if data is not None:
        privs = data.get("privileges", {})
        assert_eq(privs.get("abits"), "0x0000001f",
                  "group edit hex: abits == 0x0000001f")

    # --- Edit expire fields ---
    data, raw, rc = run_cli(
        f'group edit {TEST_GROUP} --expire-days 90 --expire-access 0',
        host=host)
    assert_true(rc == 0, "group edit expire: exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    if data is not None:
        assert_eq(data.get("expire_days"), 90,
                  "group edit expire: expire_days == 90")
        assert_eq(data.get("expire_access"), 0,
                  "group edit expire: expire_access == 0")

    # --- Error: no flags ---
    _, _, rc = run_cli(
        f'group edit {TEST_GROUP}', expect_error=True, host=host)
    assert_true(rc != 0, "group edit no-flags: error exit code",
                f"got {rc}")

    # --- Error: invalid group number ---
    _, _, rc = run_cli(
        'group edit 99 --name "X"', expect_error=True, host=host)
    assert_true(rc != 0, "group edit invalid group: error exit code",
                f"got {rc}")

    # --- Error: invalid hex value ---
    _, _, rc = run_cli(
        f'group edit {TEST_GROUP} --abits not_hex',
        expect_error=True, host=host)
    assert_true(rc != 0, "group edit invalid hex: error exit code",
                f"got {rc}")

    # --- Error: unknown flag ---
    _, _, rc = run_cli(
        f'group edit {TEST_GROUP} --nonexistent 42',
        expect_error=True, host=host)
    assert_true(rc != 0, "group edit unknown flag: error exit code",
                f"got {rc}")


def _test_group_transpose_cases(host):
    """Test cases: group transpose."""

    # --- Transpose on an empty/unused group ---
    # First find an unused group (high number, no members).
    # Group 31 may have 0 members after our edit test.
    data, raw, rc = run_cli(
        f'group transpose {TEST_GROUP}', host=host)
    assert_true(rc == 0, "group transpose: exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    if data is not None:
        assert_eq(data.get("status"), "transposed",
                  "group transpose: status == transposed")
        assert_eq(data.get("group"), TEST_GROUP,
                  "group transpose: group matches")
        assert_true("accounts_modified" in data,
                    "group transpose: has accounts_modified field")
        assert_true("accounts_skipped" in data,
                    "group transpose: has accounts_skipped field")
        assert_true("total_scanned" in data,
                    "group transpose: has total_scanned field")
        assert_true(data.get("total_scanned", 0) > 0,
                    "group transpose: total_scanned > 0")

    # --- Error: invalid group number ---
    _, _, rc = run_cli(
        'group transpose 99', expect_error=True, host=host)
    assert_true(rc != 0, "group transpose invalid group: error exit code",
                f"got {rc}")


# ---------------------------------------------------------------------------
# Config and control enhancements
# ---------------------------------------------------------------------------

def test_config_show_extended(host):
    """Extended config show has smtpd_temp_dir."""
    data, raw, rc = run_cli("config show", host=host)
    assert_true(rc == 0, "config show extended: exit code 0", f"got {rc}")
    assert_true(data is not None, "config show extended: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")
    if data is None:
        return

    network = data.get("network", {})
    assert_true(isinstance(network, dict),
                "config show extended: network is object")

    # smtpd_temp_dir is the new field.
    assert_true("smtpd_temp_dir" in network,
                "config show extended: smtpd_temp_dir present in network")
    assert_true(isinstance(network.get("smtpd_temp_dir"), str),
                "config show extended: smtpd_temp_dir is string",
                f"got {type(network.get('smtpd_temp_dir')).__name__}")

    # Regression: verify representative existing gc2 fields.
    assert_true("mail_server" in network,
                "config show extended: mail_server still present")
    assert_true("timezone" in network,
                "config show extended: timezone still present")
    assert_true("user_cache" in network,
                "config show extended: user_cache still present")
    assert_true("port_log_dir" in network,
                "config show extended: port_log_dir still present")

    # Verify top-level sections are intact.
    top_sections = [
        "identity", "limits", "defaults", "paths", "options",
        "resource_counts", "network", "task_buffer_limits",
    ]
    for section in top_sections:
        assert_true(section in data,
                    f"config show extended: {section} section present")


def test_config_flags_read(host):
    """Read control panel flags."""
    data, raw, rc = run_cli("config flags", host=host)
    assert_true(rc == 0, "config flags read: exit code 0", f"got {rc}")
    assert_true(data is not None, "config flags read: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")
    if data is None:
        return

    assert_true("flags" in data, "config flags read: has flags object")
    flags = data.get("flags", {})
    assert_true(isinstance(flags, dict),
                "config flags read: flags is object")

    expected_keys = [
        "doors_closed", "files_closed", "msgs_closed",
        "no_new_users", "sysop_in",
    ]
    # Verify exactly these keys.
    assert_eq(sorted(flags.keys()), sorted(expected_keys),
              "config flags read: exactly 5 expected flag keys")

    # Verify all values are booleans.
    for key in expected_keys:
        assert_true(isinstance(flags.get(key), bool),
                    f"config flags read: {key} is bool",
                    f"got {type(flags.get(key)).__name__}")


def test_config_port_loaded(host):
    """Per-port config for loaded port (port 0)."""
    data, raw, rc = run_cli("config port 0", host=host)
    assert_true(rc == 0, "config port loaded: exit code 0", f"got {rc}")
    assert_true(data is not None, "config port loaded: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")
    if data is None:
        return

    assert_eq(data.get("port"), 0,
              "config port loaded: port == 0")
    assert_true(data.get("loaded") is True,
                "config port loaded: loaded is true",
                f"got {data.get('loaded')!r}")

    # Verify port_config structure.
    pc = data.get("port_config")
    assert_true(isinstance(pc, dict),
                "config port loaded: port_config is object")
    if isinstance(pc, dict):
        pc_keys = [
            "online", "screen_open", "check", "idle",
            "offline", "bplanes", "interlace",
        ]
        assert_keys(pc, pc_keys,
                    "config port loaded: port_config has all keys")

    # Verify serial_config structure.
    sc = data.get("serial_config")
    assert_true(isinstance(sc, dict),
                "config port loaded: serial_config is object",
                f"got {type(sc).__name__}" if sc is not None
                else "got None")
    if isinstance(sc, dict):
        sc_keys = [
            "device_name", "unit", "idle_who", "port_flags",
        ]
        assert_keys(sc, sc_keys,
                    "config port loaded: serial_config has key fields")
        assert_true(
            isinstance(sc.get("device_name"), str)
            and len(sc.get("device_name", "")) > 0,
            "config port loaded: device_name is non-empty string",
            f"got {sc.get('device_name')!r}")

        pf = sc.get("port_flags")
        assert_true(isinstance(pf, dict),
                    "config port loaded: port_flags is object")
        if isinstance(pf, dict):
            pf_keys = ["show_on_who", "telnetd", "offclose"]
            assert_keys(pf, pf_keys,
                        "config port loaded: port_flags has all keys")


def test_config_port_unloaded(host):
    """Per-port config for unloaded port (port 99)."""
    data, raw, rc = run_cli("config port 99", host=host)
    assert_true(rc == 0, "config port unloaded: exit code 0", f"got {rc}")
    assert_true(data is not None, "config port unloaded: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")
    if data is None:
        return

    assert_eq(data.get("port"), 99,
              "config port unloaded: port == 99")
    assert_true(data.get("loaded") is False,
                "config port unloaded: loaded is false",
                f"got {data.get('loaded')!r}")

    # port_config should still be present (read from disk).
    assert_true("port_config" in data,
                "config port unloaded: port_config present")

    # serial_config should be null (no bbsport99 file).
    assert_true(data.get("serial_config") is None,
                "config port unloaded: serial_config is null",
                f"got {data.get('serial_config')!r}")

    # Should have a warning about missing serial config.
    warnings = data.get("warnings", [])
    assert_true(
        any("serial config" in w.lower() or "port 99" in w
            for w in warnings),
        "config port unloaded: warning about missing serial config",
        f"warnings: {warnings}")


def test_config_port_invalid(host):
    """Port number validation (out of range and non-numeric)."""
    # Out of range: port 100.
    _, _, rc = run_cli("config port 100", expect_error=True, host=host)
    assert_true(rc != 0,
                "config port invalid: port 100 rejected",
                f"got exit code {rc}")

    # Non-numeric: port abc.
    _, _, rc = run_cli("config port abc", expect_error=True, host=host)
    assert_true(rc != 0,
                "config port invalid: port abc rejected",
                f"got exit code {rc}")


def test_config_flags_invalid(host):
    """Reject unknown flag name and invalid flag value."""
    # Unknown flag name.
    _, _, rc = run_cli("config flags --set bogus_flag=true",
                       expect_error=True, host=host)
    assert_true(rc != 0,
                "config flags invalid: unknown flag rejected",
                f"got exit code {rc}")

    # Invalid flag value.
    _, _, rc = run_cli("config flags --set sysop_in=maybe",
                       expect_error=True, host=host)
    assert_true(rc != 0,
                "config flags invalid: bad value rejected",
                f"got exit code {rc}")


def test_config_flags_write(host):
    """Write and read-back control panel flags."""
    print(flush=True)
    print("=== Config/Control Enhancements (Mutations) ===", flush=True)

    # Capture original flag state.
    orig_data, _, orig_rc = run_cli("config flags", host=host)
    if orig_rc != 0 or orig_data is None:
        runner.record("config flags write: capture original state",
                      False, f"exit code {orig_rc}")
        return

    orig_flags = orig_data.get("flags", {})
    orig_sysop = orig_flags.get("sysop_in", False)

    try:
        # Toggle sysop_in to the opposite.
        new_val = "false" if orig_sysop else "true"
        data, raw, rc = run_cli(
            f'config flags --set sysop_in={new_val}', host=host)
        assert_true(rc == 0, "config flags write: set exit code 0",
                    f"got {rc}, raw: {raw[:200]!r}")
        if data is not None:
            assert_true(data.get("updated") is True,
                        "config flags write: updated == true",
                        f"got {data.get('updated')!r}")
            set_flags = data.get("flags", {})
            expected_val = not orig_sysop
            assert_eq(set_flags.get("sysop_in"), expected_val,
                      "config flags write: sysop_in toggled in response")

        # Read back via separate call.
        readback, _, rb_rc = run_cli("config flags", host=host)
        assert_true(rb_rc == 0, "config flags write: readback exit code 0")
        if readback is not None:
            rb_flags = readback.get("flags", {})
            assert_eq(rb_flags.get("sysop_in"), not orig_sysop,
                      "config flags write: readback confirms toggle")

        # Restore original value.
        restore_val = "true" if orig_sysop else "false"
        data, raw, rc = run_cli(
            f'config flags --set sysop_in={restore_val}', host=host)
        assert_true(rc == 0, "config flags write: restore exit code 0",
                    f"got {rc}")
        if data is not None:
            set_flags = data.get("flags", {})
            assert_eq(set_flags.get("sysop_in"), orig_sysop,
                      "config flags write: sysop_in restored")

        # Multi-flag set: set doors_closed and msgs_closed.
        orig_doors = orig_flags.get("doors_closed", False)
        orig_msgs = orig_flags.get("msgs_closed", False)

        data, raw, rc = run_cli(
            'config flags --set doors_closed=true --set msgs_closed=true',
            host=host)
        assert_true(rc == 0,
                    "config flags write: multi-set exit code 0",
                    f"got {rc}, raw: {raw[:200]!r}")
        if data is not None:
            mf = data.get("flags", {})
            assert_eq(mf.get("doors_closed"), True,
                      "config flags write: multi doors_closed == true")
            assert_eq(mf.get("msgs_closed"), True,
                      "config flags write: multi msgs_closed == true")

        # Readback multi-flag.
        readback, _, rb_rc = run_cli("config flags", host=host)
        if rb_rc == 0 and readback is not None:
            rb = readback.get("flags", {})
            assert_eq(rb.get("doors_closed"), True,
                      "config flags write: multi readback doors_closed")
            assert_eq(rb.get("msgs_closed"), True,
                      "config flags write: multi readback msgs_closed")

        # Restore both.
        restore_doors = "true" if orig_doors else "false"
        restore_msgs = "true" if orig_msgs else "false"
        data, _, rc = run_cli(
            f'config flags --set doors_closed={restore_doors} '
            f'--set msgs_closed={restore_msgs}',
            host=host)
        assert_true(rc == 0,
                    "config flags write: multi-restore exit code 0",
                    f"got {rc}")

    except Exception as e:
        runner.record("config flags write: exception", False, str(e))
        # Best-effort restore on exception.
        restore_val = "true" if orig_sysop else "false"
        run_cli(f'config flags --set sysop_in={restore_val}',
                expect_error=True, host=host)


def test_config_reload_text(host):
    """Trigger BBSTEXT/BBSMENU reload."""
    data, raw, rc = run_cli("config reload-text", host=host)
    assert_true(rc == 0, "config reload-text: exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    assert_true(data is not None, "config reload-text: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")
    if data is None:
        return

    assert_eq(data.get("action"), "reload-text",
              "config reload-text: action == reload-text")
    assert_eq(data.get("status"), "triggered",
              "config reload-text: status == triggered")


# ---------------------------------------------------------------------------
# Subboard edit extensions
# ---------------------------------------------------------------------------

def test_sub_show_new_fields(host):
    """
    Read-only: verify all extended subboard fields are present in sub show
    output with correct types.
    """
    print(flush=True)
    print("=== Sub Show Extended Fields ===", flush=True)

    data, raw, rc = run_cli("sub show 0", host=host)
    assert_true(rc == 0, "sub show extended fields: exit code 0", f"got {rc}")
    assert_true(data is not None, "sub show extended fields: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")
    if data is None:
        return

    # Access restriction fields (hex bitmasks -> strings).
    hex_fields = [
        "hours", "baud_hours", "hour_access", "hour_union_flags",
        "union_flags",
    ]
    for field in hex_fields:
        assert_true(field in data,
                    f"sub show extended fields: {field} present")
        val = data.get(field)
        assert_true(isinstance(val, str) and val.startswith("0x"),
                    f"sub show extended fields: {field} is hex string",
                    f"got {val!r}")

    # Integer fields.
    int_fields = [
        "baud", "youngest", "inactive_days", "free_days",
        "min_free_bytes", "time_credit",
    ]
    for field in int_fields:
        assert_true(field in data,
                    f"sub show extended fields: {field} present")
        assert_true(isinstance(data.get(field), int),
                    f"sub show extended fields: {field} is int",
                    f"got {type(data.get(field)).__name__}")

    # Gender field (string: "any", "M", or "F").
    assert_true("gender" in data,
                "sub show extended fields: gender present")
    assert_true(data.get("gender") in ("any", "M", "F"),
                "sub show extended fields: gender is valid string",
                f"got {data.get('gender')!r}")

    # Boolean flags.
    bool_fields = [
        "verification", "dup_check", "show_unvalidated",
        "no_signatures", "no_read_charges", "no_write_charges",
        "invitation", "user_must_join", "delete_own",
        "carbon_copy", "cdrom", "qwk_replies",
        "persist", "delay", "diz_save",
    ]
    for field in bool_fields:
        assert_true(field in data,
                    f"sub show extended fields: {field} present")
        assert_true(isinstance(data.get(field), bool),
                    f"sub show extended fields: {field} is bool",
                    f"got {type(data.get(field)).__name__}")

    # Obits bitfield and decomposed booleans.
    assert_true("obits" in data, "sub show extended fields: obits present")
    assert_true(isinstance(data.get("obits"), str)
                and data.get("obits", "").startswith("0x"),
                "sub show extended fields: obits is hex string",
                f"got {data.get('obits')!r}")

    obit_bools = [
        "obit_showbows", "obit_diz_alnum", "obit_diz_strip_chars",
        "obit_diz_strip_text", "obit_diz_strip_cr",
    ]
    for field in obit_bools:
        assert_true(field in data,
                    f"sub show extended fields: {field} present")
        assert_true(isinstance(data.get(field), bool),
                    f"sub show extended fields: {field} is bool",
                    f"got {type(data.get(field)).__name__}")


def test_sub_edit_access_restrictions(host):
    """
    Mutation test: create a test board, exercise access restriction
    fields (hex bitmasks, integers, gender), then clean up.
    """
    print(flush=True)
    print("=== Sub Edit Extensions ===", flush=True)
    print("--- Access restrictions ---", flush=True)

    # Create test board.
    create_args = (
        f'sub create '
        f'--title "Test Access" '
        f'--go {TEST_ACCESS_GO} '
        f'--type msg '
        f'--parent {EXPECTED_ROOT_SUB}'
    )
    data, raw, rc = run_cli(create_args, host=host)
    assert_true(rc == 0, "access: create exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    if rc != 0 or data is None:
        return
    test_physnum = data.get("physnum")
    if test_physnum is None:
        return

    try:
        # --- Step 1: Set hex bitmask fields ---
        edit_args = (
            f'sub edit {test_physnum} '
            f'--hours 0x000000ff '
            f'--baud-hours 0x0000ff00 '
            f'--hour-access 0x00ff0000 '
            f'--hour-union-flags 0x12345678 '
            f'--union-flags 0xabcdef01'
        )
        data, raw, rc = run_cli(edit_args, host=host)
        assert_true(rc == 0, "access: set hex fields exit code 0",
                    f"got {rc}, raw: {raw[:200]!r}")

        if data is not None:
            assert_eq(data.get("hours"), "0x000000ff",
                      "access: edit response hours")
            assert_eq(data.get("baud_hours"), "0x0000ff00",
                      "access: edit response baud_hours")
            assert_eq(data.get("hour_access"), "0x00ff0000",
                      "access: edit response hour_access")
            assert_eq(data.get("hour_union_flags"), "0x12345678",
                      "access: edit response hour_union_flags")
            assert_eq(data.get("union_flags"), "0xabcdef01",
                      "access: edit response union_flags")

        # Read back via sub show.
        show_data, _, show_rc = run_cli(
            f'sub show {test_physnum}', host=host)
        if show_rc == 0 and show_data is not None:
            assert_eq(show_data.get("hours"), "0x000000ff",
                      "access: show readback hours")
            assert_eq(show_data.get("baud_hours"), "0x0000ff00",
                      "access: show readback baud_hours")
            assert_eq(show_data.get("hour_access"), "0x00ff0000",
                      "access: show readback hour_access")
            assert_eq(show_data.get("hour_union_flags"), "0x12345678",
                      "access: show readback hour_union_flags")
            assert_eq(show_data.get("union_flags"), "0xabcdef01",
                      "access: show readback union_flags")

        # --- Step 2: Set integer fields ---
        edit_args = (
            f'sub edit {test_physnum} '
            f'--baud 2400 '
            f'--youngest 18 '
            f'--inactive-days 90 '
            f'--free-days 30 '
            f'--min-free-bytes 100000 '
            f'--time-credit 50'
        )
        data, raw, rc = run_cli(edit_args, host=host)
        assert_true(rc == 0, "access: set int fields exit code 0",
                    f"got {rc}, raw: {raw[:200]!r}")

        if data is not None:
            assert_eq(data.get("baud"), 2400,
                      "access: edit response baud")
            assert_eq(data.get("youngest"), 18,
                      "access: edit response youngest")
            assert_eq(data.get("inactive_days"), 90,
                      "access: edit response inactive_days")
            assert_eq(data.get("free_days"), 30,
                      "access: edit response free_days")
            assert_eq(data.get("min_free_bytes"), 100000,
                      "access: edit response min_free_bytes")
            assert_eq(data.get("time_credit"), 50,
                      "access: edit response time_credit")

        # Read back via sub show.
        show_data, _, show_rc = run_cli(
            f'sub show {test_physnum}', host=host)
        if show_rc == 0 and show_data is not None:
            assert_eq(show_data.get("baud"), 2400,
                      "access: show readback baud")
            assert_eq(show_data.get("youngest"), 18,
                      "access: show readback youngest")
            assert_eq(show_data.get("inactive_days"), 90,
                      "access: show readback inactive_days")
            assert_eq(show_data.get("free_days"), 30,
                      "access: show readback free_days")
            assert_eq(show_data.get("min_free_bytes"), 100000,
                      "access: show readback min_free_bytes")
            assert_eq(show_data.get("time_credit"), 50,
                      "access: show readback time_credit")

        # --- Step 3: Gender cycling ---
        for gender_val, expected in [("M", "M"), ("F", "F"),
                                     ("any", "any")]:
            data, _, rc = run_cli(
                f'sub edit {test_physnum} --gender {gender_val}',
                host=host)
            assert_true(rc == 0,
                        f"access: set gender={gender_val} exit code 0",
                        f"got {rc}")
            if data is not None:
                assert_eq(data.get("gender"), expected,
                          f"access: edit response gender={expected}")

            show_data, _, show_rc = run_cli(
                f'sub show {test_physnum}', host=host)
            if show_rc == 0 and show_data is not None:
                assert_eq(show_data.get("gender"), expected,
                          f"access: show readback gender={expected}")

        # --- Step 4: Error cases ---
        _, _, rc = run_cli(
            f'sub edit {test_physnum} --time-credit 101',
            expect_error=True, host=host)
        assert_true(rc != 0,
                    "access: --time-credit 101 rejected",
                    f"got exit code {rc}")

        _, _, rc = run_cli(
            f'sub edit {test_physnum} --youngest 256',
            expect_error=True, host=host)
        assert_true(rc != 0,
                    "access: --youngest 256 rejected",
                    f"got exit code {rc}")

        _, _, rc = run_cli(
            f'sub edit {test_physnum} --gender X',
            expect_error=True, host=host)
        assert_true(rc != 0,
                    "access: --gender X rejected",
                    f"got exit code {rc}")

    finally:
        # Cleanup.
        del_data, _, drc = run_cli(f'sub delete {test_physnum}',
                                    host=host)
        if drc == 0:
            runner.record("access: cleanup delete", True)
        else:
            run_cli(f'sub delete {test_physnum} --force',
                    expect_error=True, host=host)


def test_sub_edit_boolean_flags(host):
    """
    Mutation test: create a test board, exercise boolean flag fields,
    then clean up.
    """
    print(flush=True)
    print("--- Boolean flags ---", flush=True)

    # Create test board.
    create_args = (
        f'sub create '
        f'--title "Test Booleans" '
        f'--go {TEST_BOOL_GO} '
        f'--type msg '
        f'--parent {EXPECTED_ROOT_SUB}'
    )
    data, raw, rc = run_cli(create_args, host=host)
    assert_true(rc == 0, "bools: create exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    if rc != 0 or data is None:
        return
    test_physnum = data.get("physnum")
    if test_physnum is None:
        return

    try:
        # --- Step 1: Set first batch of booleans to true ---
        edit_args = (
            f'sub edit {test_physnum} '
            f'--verification true '
            f'--invitation true '
            f'--user-must-join true '
            f'--delete-own true '
            f'--persist true '
            f'--dup-check true'
        )
        data, raw, rc = run_cli(edit_args, host=host)
        assert_true(rc == 0, "bools: set batch 1 true exit code 0",
                    f"got {rc}, raw: {raw[:200]!r}")

        if data is not None:
            assert_true(data.get("verification") is True,
                        "bools: edit response verification=true",
                        f"got {data.get('verification')!r}")
            assert_true(data.get("invitation") is True,
                        "bools: edit response invitation=true",
                        f"got {data.get('invitation')!r}")
            assert_true(data.get("user_must_join") is True,
                        "bools: edit response user_must_join=true",
                        f"got {data.get('user_must_join')!r}")
            assert_true(data.get("delete_own") is True,
                        "bools: edit response delete_own=true",
                        f"got {data.get('delete_own')!r}")
            assert_true(data.get("persist") is True,
                        "bools: edit response persist=true",
                        f"got {data.get('persist')!r}")
            assert_true(data.get("dup_check") is True,
                        "bools: edit response dup_check=true",
                        f"got {data.get('dup_check')!r}")

        # Read back via sub show.
        show_data, _, show_rc = run_cli(
            f'sub show {test_physnum}', host=host)
        if show_rc == 0 and show_data is not None:
            assert_true(show_data.get("verification") is True,
                        "bools: show readback verification=true")
            assert_true(show_data.get("invitation") is True,
                        "bools: show readback invitation=true")
            assert_true(show_data.get("user_must_join") is True,
                        "bools: show readback user_must_join=true")
            assert_true(show_data.get("delete_own") is True,
                        "bools: show readback delete_own=true")
            assert_true(show_data.get("persist") is True,
                        "bools: show readback persist=true")
            assert_true(show_data.get("dup_check") is True,
                        "bools: show readback dup_check=true")

        # --- Step 2: Set them back to false ---
        edit_args = (
            f'sub edit {test_physnum} '
            f'--verification false '
            f'--invitation false '
            f'--user-must-join false '
            f'--delete-own false '
            f'--persist false '
            f'--dup-check false'
        )
        data, raw, rc = run_cli(edit_args, host=host)
        assert_true(rc == 0, "bools: set batch 1 false exit code 0",
                    f"got {rc}, raw: {raw[:200]!r}")

        if data is not None:
            assert_true(data.get("verification") is False,
                        "bools: edit response verification=false",
                        f"got {data.get('verification')!r}")
            assert_true(data.get("invitation") is False,
                        "bools: edit response invitation=false",
                        f"got {data.get('invitation')!r}")
            assert_true(data.get("user_must_join") is False,
                        "bools: edit response user_must_join=false",
                        f"got {data.get('user_must_join')!r}")
            assert_true(data.get("delete_own") is False,
                        "bools: edit response delete_own=false",
                        f"got {data.get('delete_own')!r}")
            assert_true(data.get("persist") is False,
                        "bools: edit response persist=false",
                        f"got {data.get('persist')!r}")
            assert_true(data.get("dup_check") is False,
                        "bools: edit response dup_check=false",
                        f"got {data.get('dup_check')!r}")

        # --- Step 3: Second batch of booleans ---
        edit_args = (
            f'sub edit {test_physnum} '
            f'--show-unvalidated true '
            f'--no-signatures true '
            f'--no-read-charges true '
            f'--no-write-charges true '
            f'--carbon-copy false '
            f'--cdrom true '
            f'--qwk-replies true '
            f'--delay true '
            f'--diz-save true'
        )
        data, raw, rc = run_cli(edit_args, host=host)
        assert_true(rc == 0, "bools: set batch 2 exit code 0",
                    f"got {rc}, raw: {raw[:200]!r}")

        if data is not None:
            assert_true(data.get("show_unvalidated") is True,
                        "bools: edit response show_unvalidated=true",
                        f"got {data.get('show_unvalidated')!r}")
            assert_true(data.get("no_signatures") is True,
                        "bools: edit response no_signatures=true",
                        f"got {data.get('no_signatures')!r}")
            assert_true(data.get("no_read_charges") is True,
                        "bools: edit response no_read_charges=true",
                        f"got {data.get('no_read_charges')!r}")
            assert_true(data.get("no_write_charges") is True,
                        "bools: edit response no_write_charges=true",
                        f"got {data.get('no_write_charges')!r}")
            assert_true(data.get("carbon_copy") is False,
                        "bools: edit response carbon_copy=false",
                        f"got {data.get('carbon_copy')!r}")
            assert_true(data.get("cdrom") is True,
                        "bools: edit response cdrom=true",
                        f"got {data.get('cdrom')!r}")
            assert_true(data.get("qwk_replies") is True,
                        "bools: edit response qwk_replies=true",
                        f"got {data.get('qwk_replies')!r}")
            assert_true(data.get("delay") is True,
                        "bools: edit response delay=true",
                        f"got {data.get('delay')!r}")
            assert_true(data.get("diz_save") is True,
                        "bools: edit response diz_save=true",
                        f"got {data.get('diz_save')!r}")

        # Read back via sub show.
        show_data, _, show_rc = run_cli(
            f'sub show {test_physnum}', host=host)
        if show_rc == 0 and show_data is not None:
            assert_true(show_data.get("show_unvalidated") is True,
                        "bools: show readback show_unvalidated=true")
            assert_true(show_data.get("no_signatures") is True,
                        "bools: show readback no_signatures=true")
            assert_true(show_data.get("no_read_charges") is True,
                        "bools: show readback no_read_charges=true")
            assert_true(show_data.get("no_write_charges") is True,
                        "bools: show readback no_write_charges=true")
            assert_true(show_data.get("carbon_copy") is False,
                        "bools: show readback carbon_copy=false")
            assert_true(show_data.get("cdrom") is True,
                        "bools: show readback cdrom=true")
            assert_true(show_data.get("qwk_replies") is True,
                        "bools: show readback qwk_replies=true")
            assert_true(show_data.get("delay") is True,
                        "bools: show readback delay=true")
            assert_true(show_data.get("diz_save") is True,
                        "bools: show readback diz_save=true")

        # Restore batch 2 to defaults.
        edit_args = (
            f'sub edit {test_physnum} '
            f'--show-unvalidated false '
            f'--no-signatures false '
            f'--no-read-charges false '
            f'--no-write-charges false '
            f'--carbon-copy false '
            f'--cdrom false '
            f'--qwk-replies false '
            f'--delay false '
            f'--diz-save false'
        )
        run_cli(edit_args, host=host)

    finally:
        # Cleanup.
        del_data, _, drc = run_cli(f'sub delete {test_physnum}',
                                    host=host)
        if drc == 0:
            runner.record("bools: cleanup delete", True)
        else:
            run_cli(f'sub delete {test_physnum} --force',
                    expect_error=True, host=host)


def test_sub_edit_obits_flags(host):
    """
    Mutation test: create a test board, exercise obits bitfield flags,
    verify raw obits hex value, then clean up.
    """
    print(flush=True)
    print("--- Obits bitfield ---", flush=True)

    # Create test board.
    create_args = (
        f'sub create '
        f'--title "Test Obits" '
        f'--go {TEST_OBITS_GO} '
        f'--type msg '
        f'--parent {EXPECTED_ROOT_SUB}'
    )
    data, raw, rc = run_cli(create_args, host=host)
    assert_true(rc == 0, "obits: create exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    if rc != 0 or data is None:
        return
    test_physnum = data.get("physnum")
    if test_physnum is None:
        return

    try:
        # --- Step 1: Set showbows + diz_alnum + strip_cr ---
        # Expected raw obits = 0x01 | 0x02 | 0x10 = 0x13
        edit_args = (
            f'sub edit {test_physnum} '
            f'--obit-showbows true '
            f'--obit-diz-alnum true '
            f'--obit-diz-strip-cr true'
        )
        data, raw, rc = run_cli(edit_args, host=host)
        assert_true(rc == 0, "obits: set flags exit code 0",
                    f"got {rc}, raw: {raw[:200]!r}")

        if data is not None:
            assert_true(data.get("obit_showbows") is True,
                        "obits: edit response showbows=true",
                        f"got {data.get('obit_showbows')!r}")
            assert_true(data.get("obit_diz_alnum") is True,
                        "obits: edit response diz_alnum=true",
                        f"got {data.get('obit_diz_alnum')!r}")
            assert_true(data.get("obit_diz_strip_cr") is True,
                        "obits: edit response strip_cr=true",
                        f"got {data.get('obit_diz_strip_cr')!r}")
            assert_true(data.get("obit_diz_strip_chars") is False,
                        "obits: edit response strip_chars=false",
                        f"got {data.get('obit_diz_strip_chars')!r}")
            assert_true(data.get("obit_diz_strip_text") is False,
                        "obits: edit response strip_text=false",
                        f"got {data.get('obit_diz_strip_text')!r}")
            assert_eq(data.get("obits"), "0x00000013",
                      "obits: edit response raw obits == 0x00000013")

        # --- Step 2: Read back via sub show ---
        show_data, _, show_rc = run_cli(
            f'sub show {test_physnum}', host=host)
        if show_rc == 0 and show_data is not None:
            assert_eq(show_data.get("obits"), "0x00000013",
                      "obits: show readback raw obits == 0x00000013")
            assert_true(show_data.get("obit_showbows") is True,
                        "obits: show readback showbows=true")
            assert_true(show_data.get("obit_diz_alnum") is True,
                        "obits: show readback diz_alnum=true")
            assert_true(show_data.get("obit_diz_strip_cr") is True,
                        "obits: show readback strip_cr=true")

        # --- Step 3: Clear all obits ---
        edit_args = (
            f'sub edit {test_physnum} '
            f'--obit-showbows false '
            f'--obit-diz-alnum false '
            f'--obit-diz-strip-cr false'
        )
        data, raw, rc = run_cli(edit_args, host=host)
        assert_true(rc == 0, "obits: clear flags exit code 0",
                    f"got {rc}, raw: {raw[:200]!r}")

        if data is not None:
            assert_eq(data.get("obits"), "0x00000000",
                      "obits: edit response raw obits == 0x00000000")
            assert_true(data.get("obit_showbows") is False,
                        "obits: edit response showbows=false after clear",
                        f"got {data.get('obit_showbows')!r}")
            assert_true(data.get("obit_diz_alnum") is False,
                        "obits: edit response diz_alnum=false after clear",
                        f"got {data.get('obit_diz_alnum')!r}")
            assert_true(data.get("obit_diz_strip_cr") is False,
                        "obits: edit response strip_cr=false after clear",
                        f"got {data.get('obit_diz_strip_cr')!r}")

        # Read back to confirm all zeros.
        show_data, _, show_rc = run_cli(
            f'sub show {test_physnum}', host=host)
        if show_rc == 0 and show_data is not None:
            assert_eq(show_data.get("obits"), "0x00000000",
                      "obits: show readback raw obits == 0x00000000")

    finally:
        # Cleanup.
        del_data, _, drc = run_cli(f'sub delete {test_physnum}',
                                    host=host)
        if drc == 0:
            runner.record("obits: cleanup delete", True)
        else:
            run_cli(f'sub delete {test_physnum} --force',
                    expect_error=True, host=host)


# ---------------------------------------------------------------------------
# Utility integrations
# ---------------------------------------------------------------------------

TEST_ACCESS_EDIT_GO = "_test_acsedit"


def test_sub_show_access_groups(host):
    """access_groups field present in sub show as a string."""
    print(flush=True)
    print("=== Utility Integrations ===", flush=True)

    data, raw, rc = run_cli("sub show 0", host=host)
    assert_true(rc == 0, "sub show access_groups: exit code 0", f"got {rc}")
    assert_true(data is not None, "sub show access_groups: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")
    if data is None:
        return

    # All 8 _groups fields must be present as strings.
    groups_fields = [
        "access_groups", "post_access_groups", "respond_access_groups",
        "upload_access_groups", "download_access_groups",
        "hour_access_groups", "hour_union_flags_groups",
        "union_flags_groups",
    ]
    for field in groups_fields:
        assert_true(field in data,
                    f"sub show access_groups: {field} present")
        assert_true(isinstance(data.get(field), str),
                    f"sub show access_groups: {field} is string",
                    f"got {type(data.get(field)).__name__}")


def test_sub_list_no_access_groups(host):
    """access_groups NOT in sub list (lean view)."""
    data, raw, rc = run_cli("sub list --active", host=host)
    assert_true(rc == 0, "sub list no access_groups: exit code 0",
                f"got {rc}")
    assert_true(data is not None, "sub list no access_groups: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")
    if data is None:
        return

    subs = data.get("subboards", [])
    assert_true(len(subs) > 0,
                "sub list no access_groups: non-empty list")
    if subs:
        first = subs[0]
        assert_true("access_groups" not in first,
                    "sub list no access_groups: access_groups absent "
                    "from list entry",
                    f"found access_groups={first.get('access_groups')!r}")


def test_group_show_access_groups(host):
    """group show has _groups fields under privileges."""
    data, raw, rc = run_cli("group show 0", host=host)
    assert_true(rc == 0, "group show access_groups: exit code 0",
                f"got {rc}")
    assert_true(data is not None, "group show access_groups: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")
    if data is None:
        return

    privs = data.get("privileges", {})
    assert_true(isinstance(privs, dict),
                "group show access_groups: privileges is object")

    for field in ["mbase_flags_groups", "fbase_flags_groups",
                  "lbase_flags_groups"]:
        assert_true(field in privs,
                    f"group show access_groups: {field} present",
                    f"privileges keys: {list(privs.keys())}")
        assert_true(isinstance(privs.get(field), str),
                    f"group show access_groups: {field} is string",
                    f"got {type(privs.get(field)).__name__}")


def test_group_list_no_access_groups(host):
    """group list does NOT have _groups fields."""
    data, raw, rc = run_cli("group list", host=host)
    assert_true(rc == 0, "group list no access_groups: exit code 0",
                f"got {rc}")
    assert_true(data is not None, "group list no access_groups: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")
    if data is None:
        return

    groups = data.get("groups", [])
    assert_true(len(groups) > 0,
                "group list no access_groups: non-empty list")
    # Check all entries -- none should have _groups fields.
    has_groups_field = False
    for entry in groups:
        if "mbase_flags_groups" in entry:
            has_groups_field = True
            break
    assert_true(not has_groups_field,
                "group list no access_groups: mbase_flags_groups absent "
                "from all list entries")


def test_user_show_address_type(host):
    """user show has address_type field."""
    data, raw, rc = run_cli("user show 1", host=host)
    assert_true(rc == 0, "user show address_type: exit code 0",
                f"got {rc}")
    assert_true(data is not None, "user show address_type: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")
    if data is None:
        return

    assert_true("address_type" in data,
                "user show address_type: field present")
    atype = data.get("address_type")
    assert_true(isinstance(atype, str),
                "user show address_type: is string",
                f"got {type(atype).__name__}")
    assert_true(atype in ("local", "internet", "unknown"),
                "user show address_type: valid value",
                f"got {atype!r}")


def test_sub_disk_usage(host):
    """sub disk-usage returns size info for a valid subboard."""
    data, raw, rc = run_cli("sub disk-usage 0", host=host)
    assert_true(rc == 0, "sub disk-usage: exit code 0", f"got {rc}")
    assert_true(data is not None, "sub disk-usage: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")
    if data is None:
        return

    required_keys = ["physnum", "title", "data_path", "bytes"]
    assert_keys(data, required_keys, "sub disk-usage: has required keys")

    assert_eq(data.get("physnum"), 0,
              "sub disk-usage: physnum == 0")
    assert_true(isinstance(data.get("bytes"), int),
                "sub disk-usage: bytes is integer",
                f"got {type(data.get('bytes')).__name__}")
    assert_true(data.get("bytes", -1) >= 0,
                "sub disk-usage: bytes >= 0",
                f"got {data.get('bytes')}")
    assert_true(isinstance(data.get("data_path"), str)
                and len(data.get("data_path", "")) > 0,
                "sub disk-usage: data_path is non-empty string",
                f"got {data.get('data_path')!r}")


def test_sub_disk_usage_invalid(host):
    """sub disk-usage rejects invalid subboard number."""
    _, _, rc = run_cli("sub disk-usage 999", expect_error=True, host=host)
    assert_true(rc != 0, "sub disk-usage invalid: nonzero exit code",
                f"got {rc}")


def test_sub_edit_access_groups_mutation(host):
    """
    Create test board, set access via group string,
    verify access hex and access_groups, test hex backward compat,
    then clean up.
    """
    print(flush=True)
    print("--- Access group conversion ---", flush=True)

    # Create test board.
    create_args = (
        f'sub create '
        f'--title "Test Access Groups" '
        f'--go {TEST_ACCESS_EDIT_GO} '
        f'--type msg '
        f'--parent {EXPECTED_ROOT_SUB}'
    )
    data, raw, rc = run_cli(create_args, host=host)
    assert_true(rc == 0, "access groups: create exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    if rc != 0 or data is None:
        return
    test_physnum = data.get("physnum")
    if test_physnum is None:
        return

    try:
        # --- Step 1: Set access via group string "0-3,5,10" ---
        # Expected: bits 0,1,2,3,5,10 = 0x0000042f
        edit_args = (
            f'sub edit {test_physnum} '
            f'--access 0-3,5,10'
        )
        data, raw, rc = run_cli(edit_args, host=host)
        assert_true(rc == 0,
                    "access groups: set via group string exit code 0",
                    f"got {rc}, raw: {raw[:200]!r}")

        if data is not None:
            assert_eq(data.get("access"), "0x0000042f",
                      "access groups: edit response access == 0x0000042f")
            assert_eq(data.get("access_groups"), "0-3,5,10",
                      "access groups: edit response access_groups == 0-3,5,10")

        # Read back via sub show.
        show_data, _, show_rc = run_cli(
            f'sub show {test_physnum}', host=host)
        if show_rc == 0 and show_data is not None:
            assert_eq(show_data.get("access"), "0x0000042f",
                      "access groups: show readback access == 0x0000042f")
            assert_eq(show_data.get("access_groups"), "0-3,5,10",
                      "access groups: show readback access_groups == 0-3,5,10")

        # --- Step 2: Backward compat -- set via hex ---
        edit_args = (
            f'sub edit {test_physnum} '
            f'--access 0xffffffff'
        )
        data, raw, rc = run_cli(edit_args, host=host)
        assert_true(rc == 0,
                    "access groups: set via hex exit code 0",
                    f"got {rc}, raw: {raw[:200]!r}")

        if data is not None:
            assert_eq(data.get("access"), "0xffffffff",
                      "access groups: edit response access == 0xffffffff")
            # All 32 groups set -> "0-31"
            assert_eq(data.get("access_groups"), "0-31",
                      "access groups: edit response access_groups == 0-31")

    finally:
        # Cleanup.
        del_data, _, drc = run_cli(f'sub delete {test_physnum}',
                                    host=host)
        if drc == 0:
            runner.record("access groups: cleanup delete", True)
        else:
            run_cli(f'sub delete {test_physnum} --force',
                    expect_error=True, host=host)


def test_file_validate_range(host):
    """
    file validate with --range all on a file subboard.
    Skipped if no file subboards exist.
    """
    print(flush=True)
    print("--- File validate range ---", flush=True)

    # Find a file subboard.
    list_data, _, list_rc = run_cli("sub list --type file", host=host)
    if list_rc != 0 or list_data is None:
        runner.skip("file validate range: cannot fetch file subboard list")
        return

    file_subs = list_data.get("subboards", [])
    if not file_subs:
        runner.skip("file validate range: no file subboards exist")
        return

    # Use the first file subboard.
    physnum = file_subs[0]["physnum"]

    data, raw, rc = run_cli(
        f'file validate {physnum} --range all', host=host)
    assert_true(rc == 0, "file validate range: exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    assert_true(data is not None, "file validate range: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is not None:
        assert_eq(data.get("status"), "validated",
                  "file validate range: status == validated")
        assert_eq(data.get("physnum"), physnum,
                  f"file validate range: physnum == {physnum}")
        assert_true("total_in_range" in data,
                    "file validate range: has total_in_range")
        assert_true(isinstance(data.get("total_in_range"), int),
                    "file validate range: total_in_range is int",
                    f"got {type(data.get('total_in_range')).__name__}")


# ---------------------------------------------------------------------------
# Event commands
# ---------------------------------------------------------------------------

def test_event_list_empty(host):
    """event list with no events.cfg returns empty array."""
    data, raw, rc = run_cli("event list", host=host)
    assert_true(rc == 0, "event list empty: exit code 0", f"got {rc}")
    assert_true(data is not None, "event list empty: valid JSON",
                f"raw output: {raw[:200]!r}" if data is None else "")
    if data is None:
        return
    assert_true("events" in data, "event list empty: has 'events' key")
    events = data.get("events", None)
    assert_true(isinstance(events, list) and len(events) == 0,
                "event list empty: events is empty array",
                f"got {events!r}")
    assert_eq(data.get("count"), 0, "event list empty: count == 0")


def test_event_list_all_empty(host):
    """event list --all with no events.cfg returns empty array."""
    data, raw, rc = run_cli("event list --all", host=host)
    assert_true(rc == 0, "event list --all empty: exit code 0",
                f"got {rc}")
    assert_true(data is not None, "event list --all empty: valid JSON",
                f"raw output: {raw[:200]!r}" if data is None else "")
    if data is None:
        return
    assert_true("events" in data,
                "event list --all empty: has 'events' key")
    events = data.get("events", None)
    assert_true(isinstance(events, list) and len(events) == 0,
                "event list --all empty: events is empty array",
                f"got {events!r}")
    assert_eq(data.get("count"), 0,
              "event list --all empty: count == 0")


def test_event_show_no_events(host):
    """event show with no events.cfg returns error."""
    # Error JSON goes to Amiga stderr (not captured by amigactl exec).
    # We can only verify non-zero exit code.
    _, _, rc = run_cli("event show 0", expect_error=True, host=host)
    assert_true(rc != 0, "event show no events: nonzero exit code",
                f"got {rc}")


def test_event_show_negative_index(host):
    """event show -1 is rejected (not all digits)."""
    # "-1" fails all_digits() check -> usage error on stderr, exit code 1.
    _, _, rc = run_cli("event show -1", expect_error=True, host=host)
    assert_true(rc != 0, "event show negative index: nonzero exit code",
                f"got {rc}")


# ---------------------------------------------------------------------------
# File and log enhancements
# ---------------------------------------------------------------------------

def test_file_missing_all(host):
    """file missing scans all file subboards, returns JSON summary."""
    data, raw, rc = run_cli("file missing", host=host)
    assert_true(rc == 0, "file missing all: exit code 0", f"got {rc}")
    assert_true(data is not None, "file missing all: valid JSON",
                f"raw output: {raw[:200]!r}" if data is None else "")
    if data is None:
        return

    # Top-level keys.
    required_keys = ["missing", "restored", "summary"]
    if not assert_keys(data, required_keys,
                       "file missing all: has required keys"):
        return

    assert_true(isinstance(data["missing"], list),
                "file missing all: missing is array")
    assert_true(isinstance(data["restored"], list),
                "file missing all: restored is array")

    # With 0 items in file subboards, both arrays should be empty.
    assert_eq(len(data["missing"]), 0,
              "file missing all: missing array empty (no files uploaded)")
    assert_eq(len(data["restored"]), 0,
              "file missing all: restored array empty")

    # Summary validation.
    summary = data.get("summary", {})
    summary_keys = [
        "subboards_scanned", "items_scanned", "missing_count",
        "restored_count",
    ]
    if not assert_keys(summary, summary_keys,
                       "file missing all: summary has required keys"):
        return

    assert_true(isinstance(summary["subboards_scanned"], int)
                and summary["subboards_scanned"] > 0,
                "file missing all: subboards_scanned > 0",
                f"got {summary['subboards_scanned']}")
    assert_true(isinstance(summary["items_scanned"], int)
                and summary["items_scanned"] >= 0,
                "file missing all: items_scanned >= 0",
                f"got {summary['items_scanned']}")
    assert_eq(summary["missing_count"], 0,
              "file missing all: missing_count == 0")
    assert_eq(summary["restored_count"], 0,
              "file missing all: restored_count == 0")


def test_file_missing_specific_sub(host):
    """file missing on a specific file subboard by physnum."""
    # Find a file subboard to target.
    list_data, _, list_rc = run_cli("sub list --type file", host=host)
    if list_rc != 0 or list_data is None:
        runner.skip("file missing specific sub: "
                    "cannot fetch file subboard list")
        return

    file_subs = list_data.get("subboards", [])
    if not file_subs:
        runner.skip("file missing specific sub: no file subboards exist")
        return

    physnum = file_subs[0]["physnum"]

    data, raw, rc = run_cli(f'file missing {physnum}', host=host)
    assert_true(rc == 0, "file missing specific sub: exit code 0",
                f"got {rc}")
    assert_true(data is not None, "file missing specific sub: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is None:
        return

    assert_true("missing" in data,
                "file missing specific sub: has 'missing' key")
    assert_true("summary" in data,
                "file missing specific sub: has 'summary' key")

    summary = data.get("summary", {})
    assert_eq(summary.get("subboards_scanned"), 1,
              "file missing specific sub: subboards_scanned == 1")


def test_file_missing_invalid_sub(host):
    """file missing on nonexistent subboard returns error."""
    _, _, rc = run_cli("file missing 999", expect_error=True, host=host)
    assert_true(rc != 0, "file missing invalid sub: nonzero exit code",
                f"got {rc}")


def test_file_missing_msg_sub(host):
    """file missing on a message subboard returns error."""
    # Physnum 0 is always a MsgBase on the live BBS.
    _, _, rc = run_cli("file missing 0", expect_error=True, host=host)
    assert_true(rc != 0,
                "file missing msg sub: nonzero exit code (not a file area)",
                f"got {rc}")


def test_log_callers_parsed(host):
    """log callers-parsed returns parsed call records."""
    data, raw, rc = run_cli("log callers-parsed", host=host)
    assert_true(rc == 0, "log callers-parsed: exit code 0", f"got {rc}")
    assert_true(data is not None, "log callers-parsed: valid JSON",
                f"raw output: {raw[:200]!r}" if data is None else "")
    if data is None:
        return

    required_keys = ["records", "count", "truncated"]
    if not assert_keys(data, required_keys,
                       "log callers-parsed: has required keys"):
        return

    assert_true(isinstance(data["records"], list),
                "log callers-parsed: records is array")
    assert_true(isinstance(data["count"], int) and data["count"] > 0,
                "log callers-parsed: count > 0",
                f"got {data['count']}")
    assert_true(isinstance(data["truncated"], bool),
                "log callers-parsed: truncated is bool",
                f"got {type(data['truncated']).__name__}")
    assert_true(len(data["records"]) == data["count"],
                "log callers-parsed: records length matches count",
                f"records={len(data['records'])}, count={data['count']}")


def test_log_callers_parsed_tail(host):
    """log callers-parsed --tail 3 returns exactly 3 records."""
    data, raw, rc = run_cli("log callers-parsed --tail 3", host=host)
    assert_true(rc == 0, "log callers-parsed --tail 3: exit code 0",
                f"got {rc}")
    assert_true(data is not None,
                "log callers-parsed --tail 3: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")
    if data is None:
        return

    assert_eq(data.get("count"), 3,
              "log callers-parsed --tail 3: count == 3")
    assert_true(isinstance(data.get("records"), list)
                and len(data["records"]) == 3,
                "log callers-parsed --tail 3: records length == 3",
                f"got {len(data.get('records', []))}")


def test_log_callers_parsed_record_structure(host):
    """log callers-parsed record has expected field structure."""
    data, raw, rc = run_cli("log callers-parsed --tail 1", host=host)
    assert_true(rc == 0,
                "log callers-parsed record structure: exit code 0",
                f"got {rc}")
    assert_true(data is not None,
                "log callers-parsed record structure: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")
    if data is None:
        return

    records = data.get("records", [])
    if not records:
        runner.skip("log callers-parsed record structure: no records")
        return

    rec = records[0]
    # Required fields on every record.
    record_keys = ["date", "time", "port", "connect", "events"]
    if not assert_keys(rec, record_keys,
                       "log callers-parsed record structure: "
                       "has required keys"):
        return

    assert_true(isinstance(rec["date"], str) and len(rec["date"]) > 0,
                "log callers-parsed record: date is non-empty string",
                f"got {rec['date']!r}")
    assert_true(isinstance(rec["time"], str) and len(rec["time"]) > 0,
                "log callers-parsed record: time is non-empty string",
                f"got {rec['time']!r}")
    assert_true(isinstance(rec["port"], int) and rec["port"] >= 0,
                "log callers-parsed record: port is non-negative int",
                f"got {rec['port']}")
    assert_true(isinstance(rec["connect"], str),
                "log callers-parsed record: connect is string",
                f"got {type(rec['connect']).__name__}")
    assert_true(isinstance(rec["events"], list),
                "log callers-parsed record: events is array",
                f"got {type(rec['events']).__name__}")

    # Validate event structure if events exist.
    if rec["events"]:
        ev = rec["events"][0]
        event_keys = ["time", "event"]
        assert_keys(ev, event_keys,
                    "log callers-parsed record: event has time+event keys")


def test_log_callers_parsed_tail_1(host):
    """log callers-parsed --tail 1 returns exactly 1 record."""
    data, raw, rc = run_cli("log callers-parsed --tail 1", host=host)
    assert_true(rc == 0, "log callers-parsed --tail 1: exit code 0",
                f"got {rc}")
    assert_true(data is not None,
                "log callers-parsed --tail 1: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")
    if data is None:
        return

    assert_eq(data.get("count"), 1,
              "log callers-parsed --tail 1: count == 1")
    assert_true(isinstance(data.get("records"), list)
                and len(data["records"]) == 1,
                "log callers-parsed --tail 1: records length == 1",
                f"got {len(data.get('records', []))}")


# ---------------------------------------------------------------------------
# Maintenance Operations
# ---------------------------------------------------------------------------

def test_maint_pointers(host):
    """maint pointers rebuilds index files."""
    print(flush=True)
    print("=== Maintenance Operations ===", flush=True)

    data, raw, rc = run_cli("maint pointers", host=host)

    assert_true(rc == 0, "maint pointers: exit code 0", f"got {rc}")
    assert_true(data is not None, "maint pointers: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is None:
        return

    assert_eq(data.get("command"), "maint_pointers",
              "maint pointers: command == maint_pointers")
    assert_true(isinstance(data.get("accounts"), int)
                and data["accounts"] > 0,
                "maint pointers: accounts > 0",
                f"got {data.get('accounts')!r}")
    assert_true(isinstance(data.get("iname_entries"), int)
                and data["iname_entries"] >= 0,
                "maint pointers: iname_entries >= 0",
                f"got {data.get('iname_entries')!r}")
    assert_true(isinstance(data.get("iphone_entries"), int)
                and data["iphone_entries"] >= 0,
                "maint pointers: iphone_entries >= 0",
                f"got {data.get('iphone_entries')!r}")

    # files_written should be a list of exactly 4 known filenames.
    files = data.get("files_written")
    assert_true(isinstance(files, list) and len(files) == 4,
                "maint pointers: files_written is list of 4",
                f"got {files!r}")
    expected_files = ["bbs.ukeys4", "bbs.uind1", "bbs.uind2", "bbs.sdata"]
    if isinstance(files, list):
        assert_eq(files, expected_files,
                  "maint pointers: files_written matches expected")

    # warnings should be an empty list.
    assert_eq(data.get("warnings"), [],
              "maint pointers: warnings is empty list")


def test_maint_count_dry_run(host):
    """maint count --dry-run scans all subboards without applying."""
    data, raw, rc = run_cli("maint count --dry-run", host=host)

    assert_true(rc == 0, "maint count --dry-run: exit code 0",
                f"got {rc}")
    assert_true(data is not None, "maint count --dry-run: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is None:
        return

    assert_eq(data.get("command"), "maint_count",
              "maint count --dry-run: command == maint_count")
    assert_eq(data.get("mode"), "dry-run",
              "maint count --dry-run: mode == dry-run")

    # changes key must exist and be a list.
    changes = data.get("changes")
    assert_true(isinstance(changes, list),
                "maint count --dry-run: changes is a list",
                f"got {type(changes).__name__}" if changes is not None
                else "key missing")

    assert_true(isinstance(data.get("subboards_scanned"), int)
                and data["subboards_scanned"] > 0,
                "maint count --dry-run: subboards_scanned > 0",
                f"got {data.get('subboards_scanned')!r}")
    assert_true(isinstance(data.get("subboards_skipped"), int)
                and data["subboards_skipped"] >= 0,
                "maint count --dry-run: subboards_skipped >= 0",
                f"got {data.get('subboards_skipped')!r}")

    # Nested nums structure.
    nums = data.get("nums", {})
    ca = nums.get("current_accounts", {})
    assert_true(isinstance(ca.get("old"), int) and ca["old"] > 0,
                "maint count --dry-run: nums.current_accounts.old > 0",
                f"got {ca.get('old')!r}")
    ia = nums.get("inuse_accounts", {})
    assert_true(isinstance(ia.get("old"), int) and ia["old"] > 0,
                "maint count --dry-run: nums.inuse_accounts.old > 0",
                f"got {ia.get('old')!r}")


def test_maint_count_single_sub(host):
    """maint count --sub General --dry-run scans only one subboard."""
    data, raw, rc = run_cli("maint count --sub General --dry-run", host=host)

    assert_true(rc == 0, "maint count --sub General: exit code 0",
                f"got {rc}")
    assert_true(data is not None, "maint count --sub General: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is None:
        return

    assert_eq(data.get("command"), "maint_count",
              "maint count --sub General: command == maint_count")
    assert_eq(data.get("subboards_scanned"), 1,
              "maint count --sub General: subboards_scanned == 1")


def test_maint_repair_mail_dry_run(host):
    """maint repair-mail --all --dry-run scans all users without applying."""
    data, raw, rc = run_cli("maint repair-mail --all --dry-run", host=host)

    assert_true(rc == 0, "maint repair-mail --all --dry-run: exit code 0",
                f"got {rc}")
    assert_true(data is not None,
                "maint repair-mail --all --dry-run: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is None:
        return

    assert_eq(data.get("command"), "maint_repair_mail",
              "maint repair-mail --all --dry-run: command == maint_repair_mail")
    assert_eq(data.get("mode"), "dry-run",
              "maint repair-mail --all --dry-run: mode == dry-run")

    users = data.get("users")
    assert_true(isinstance(users, list) and len(users) > 0,
                "maint repair-mail --all --dry-run: users is non-empty list",
                f"got {type(users).__name__}, len={len(users) if isinstance(users, list) else 'N/A'}")

    assert_true(isinstance(data.get("users_scanned"), int)
                and data["users_scanned"] > 0,
                "maint repair-mail --all --dry-run: users_scanned > 0",
                f"got {data.get('users_scanned')!r}")
    assert_true(isinstance(data.get("total_bytes_reclaimed"), int)
                and data["total_bytes_reclaimed"] >= 0,
                "maint repair-mail --all --dry-run: total_bytes_reclaimed >= 0",
                f"got {data.get('total_bytes_reclaimed')!r}")


def test_maint_repair_mail_single_user(host):
    """maint repair-mail 1 --dry-run scans only account #1 (sysop)."""
    data, raw, rc = run_cli("maint repair-mail 1 --dry-run", host=host)

    assert_true(rc == 0,
                "maint repair-mail 1 --dry-run: exit code 0",
                f"got {rc}")
    assert_true(data is not None,
                "maint repair-mail 1 --dry-run: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is None:
        return

    assert_eq(data.get("command"), "maint_repair_mail",
              "maint repair-mail 1 --dry-run: command == maint_repair_mail")

    users = data.get("users")
    assert_true(isinstance(users, list) and len(users) == 1,
                "maint repair-mail 1 --dry-run: users list has exactly 1 entry",
                f"got len={len(users) if isinstance(users, list) else 'N/A'}")


def test_maint_repair_sub_unavailable(host):
    """maint repair-sub returns an error (not yet available)."""
    # repair-sub outputs error JSON to stdout but returns exit code 1.
    # run_cli returns (None, stdout, exit_code) for non-zero exits,
    # so we parse the JSON from raw stdout.
    _, raw, rc = run_cli("maint repair-sub Feedback", host=host)

    assert_true(rc != 0,
                "maint repair-sub unavailable: exit code != 0",
                f"got {rc}")

    # Parse error JSON from stdout.
    err_data = None
    if raw:
        try:
            err_data = json.loads(raw)
        except json.JSONDecodeError:
            pass

    assert_true(err_data is not None and "error" in err_data,
                "maint repair-sub unavailable: response has 'error' key",
                f"raw: {raw[:200]!r}" if err_data is None else "")

    if err_data and "error" in err_data:
        assert_true("not yet available" in err_data["error"],
                    "maint repair-sub unavailable: error contains "
                    "'not yet available'",
                    f"got: {err_data['error']!r}")


# ---------------------------------------------------------------------------
# BBSList, vote, and mail alias commands
# ---------------------------------------------------------------------------

def test_bbslist_list(host):
    """bbslist list returns JSON with entries array."""
    print(flush=True)
    print("=== BBSList Commands ===", flush=True)

    data, raw, rc = run_cli("bbslist list", host=host)

    assert_true(rc == 0, "bbslist list: exit code 0", f"got {rc}")
    assert_true(data is not None, "bbslist list: valid JSON",
                f"raw output: {raw[:200]!r}" if data is None else "")

    if data is None:
        return

    assert_true("entries" in data, "bbslist list: has 'entries' key")
    entries = data.get("entries")
    assert_true(isinstance(entries, list),
                "bbslist list: entries is array",
                f"got {type(entries).__name__}")
    assert_true("count" in data, "bbslist list: has 'count' key")
    assert_true(isinstance(data.get("count"), int)
                and data["count"] >= 0,
                "bbslist list: count is int >= 0",
                f"got {data.get('count')!r}")
    assert_true("total_records" in data,
                "bbslist list: has 'total_records' key")


def test_bbslist_list_all(host):
    """bbslist list --all returns JSON with entries array."""
    data, raw, rc = run_cli("bbslist list --all", host=host)

    assert_true(rc == 0, "bbslist list --all: exit code 0", f"got {rc}")
    assert_true(data is not None, "bbslist list --all: valid JSON",
                f"raw output: {raw[:200]!r}" if data is None else "")

    if data is None:
        return

    assert_true("entries" in data, "bbslist list --all: has 'entries' key")
    entries = data.get("entries")
    assert_true(isinstance(entries, list),
                "bbslist list --all: entries is array",
                f"got {type(entries).__name__}")


def test_vote_list(host):
    """vote list returns JSON with topics array."""
    print(flush=True)
    print("=== Vote Commands ===", flush=True)

    data, raw, rc = run_cli("vote list", host=host)

    assert_true(rc == 0, "vote list: exit code 0", f"got {rc}")
    assert_true(data is not None, "vote list: valid JSON",
                f"raw output: {raw[:200]!r}" if data is None else "")

    if data is None:
        return

    assert_true("topics" in data, "vote list: has 'topics' key")
    topics = data.get("topics")
    assert_true(isinstance(topics, list),
                "vote list: topics is array",
                f"got {type(topics).__name__}")
    assert_true("count" in data, "vote list: has 'count' key")
    assert_true(isinstance(data.get("count"), int)
                and data["count"] >= 0,
                "vote list: count is int >= 0",
                f"got {data.get('count')!r}")


def test_vote_show(host):
    """vote show 1 -- may fail if no topics exist."""
    data, raw, rc = run_cli("vote show 1", host=host)

    if rc != 0:
        # No topics or index out of range -- acceptable.
        runner.skip("vote show 1", "exit code != 0 (likely no topics)")
        return

    assert_true(data is not None, "vote show 1: valid JSON",
                f"raw output: {raw[:200]!r}" if data is None else "")

    if data is None:
        return

    # If we got valid JSON, verify expected structure.
    assert_true("topic" in data or "question" in data,
                "vote show 1: has 'topic' or 'question' key",
                f"keys: {list(data.keys())}")


def test_vote_results(host):
    """vote results 1 -- may fail if no topics exist."""
    data, raw, rc = run_cli("vote results 1", host=host)

    if rc != 0:
        # No topics or index out of range -- acceptable.
        runner.skip("vote results 1", "exit code != 0 (likely no topics)")
        return

    assert_true(data is not None, "vote results 1: valid JSON",
                f"raw output: {raw[:200]!r}" if data is None else "")

    if data is None:
        return

    # If we got valid JSON, verify expected structure.
    assert_true("topic" in data or "question" in data or "results" in data,
                "vote results 1: has expected keys",
                f"keys: {list(data.keys())}")


def test_mail_alias_list(host):
    """mail alias list for known user."""
    print(flush=True)
    print("=== Mail Alias Commands ===", flush=True)

    data, raw, rc = run_cli("mail alias list Samoht", host=host)

    assert_true(rc == 0, "mail alias list: exit code 0", f"got {rc}")
    assert_true(data is not None, "mail alias list: valid JSON",
                f"raw output: {raw[:200]!r}" if data is None else "")

    if data is None:
        return

    assert_true("aliases" in data, "mail alias list: has 'aliases' key")
    aliases = data.get("aliases")
    assert_true(isinstance(aliases, list),
                "mail alias list: aliases is array",
                f"got {type(aliases).__name__}")
    assert_true("count" in data, "mail alias list: has 'count' key")
    assert_true(isinstance(data.get("count"), int),
                "mail alias list: count is int",
                f"got {type(data.get('count')).__name__}")
    assert_true("account" in data, "mail alias list: has 'account' key")
    assert_true("uucp_name" in data, "mail alias list: has 'uucp_name' key")


def test_mail_alias_mutations(host):
    """mail alias add/remove cycle with read-back verification."""
    print(flush=True)
    print("--- Mail alias mutations ---", flush=True)

    test_alias = "clitest"

    # --- Add alias ---
    add_args = (
        f'mail alias add Samoht '
        f'--alias {test_alias} '
        f'--name "Test Alias"'
    )
    data, raw, rc = run_cli(add_args, host=host)

    assert_true(rc == 0, "mail alias add: exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    assert_true(data is not None, "mail alias add: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is not None:
        assert_eq(data.get("status"), "added",
                  "mail alias add: status == added")

    # --- Read back: verify alias is in the list ---
    data, raw, rc = run_cli("mail alias list Samoht", host=host)
    assert_true(rc == 0, "mail alias list after add: exit code 0",
                f"got {rc}")

    alias_found = False
    if data is not None:
        aliases = data.get("aliases", [])
        for a in aliases:
            name = a if isinstance(a, str) else a.get("alias", "")
            if name == test_alias or (isinstance(a, dict)
                                      and a.get("alias") == test_alias):
                alias_found = True
                break
    assert_true(alias_found,
                "mail alias list after add: clitest alias present",
                f"aliases: {data.get('aliases', [])!r}"
                if data else "no data")

    # --- Remove alias ---
    remove_args = (
        f'mail alias remove Samoht '
        f'--alias {test_alias}'
    )
    data, raw, rc = run_cli(remove_args, host=host)

    assert_true(rc == 0, "mail alias remove: exit code 0",
                f"got {rc}, raw: {raw[:200]!r}")
    assert_true(data is not None, "mail alias remove: valid JSON",
                f"raw: {raw[:200]!r}" if data is None else "")

    if data is not None:
        assert_eq(data.get("status"), "removed",
                  "mail alias remove: status == removed")
        assert_eq(data.get("removed_count"), 1,
                  "mail alias remove: removed_count == 1")

    # --- Read back: verify alias is gone ---
    data, raw, rc = run_cli("mail alias list Samoht", host=host)
    assert_true(rc == 0, "mail alias list after remove: exit code 0",
                f"got {rc}")

    alias_still_present = False
    if data is not None:
        aliases = data.get("aliases", [])
        for a in aliases:
            name = a if isinstance(a, str) else a.get("alias", "")
            if name == test_alias or (isinstance(a, dict)
                                      and a.get("alias") == test_alias):
                alias_still_present = True
                break
    assert_true(not alias_still_present,
                "mail alias list after remove: clitest alias gone",
                f"aliases: {data.get('aliases', [])!r}"
                if data else "no data")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    host = DEFAULT_HOST
    skip_mutations = False

    # Parse CLI arguments.
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--host" and i + 1 < len(args):
            host = args[i + 1]
            i += 2
        elif args[i] == "--skip-mutations":
            skip_mutations = True
            i += 1
        else:
            print(f"Unknown argument: {args[i]}", file=sys.stderr)
            print("Usage: test_integration.py [--host HOST] "
                  "[--skip-mutations]", file=sys.stderr)
            sys.exit(2)

    print(f"cnet-cli integration tests -- host={host}", flush=True)
    print(f"Using amigactl at: {AMIGACTL_PATH}", flush=True)
    print(flush=True)

    # Connectivity check: run status to verify we can reach the BBS.
    print("--- Connectivity check ---", flush=True)
    _, raw, rc = run_cli("status", host=host)
    if rc != 0:
        print(f"FATAL: cannot reach CNet BBS at {host} "
              f"(status exit code {rc})", file=sys.stderr)
        if raw:
            print(f"  stdout: {raw[:300]!r}", file=sys.stderr)
        sys.exit(3)
    print("  Connection OK.", flush=True)
    print(flush=True)

    # Cleanup orphaned test subboards from previous runs.
    cleanup_test_subboards(host)

    # System status.
    test_status(host)
    test_ports(host)
    test_who(host)

    # cnet4.library integration.
    test_cnet4_library(host)

    # Subboard reads.
    test_sub_list(host)
    test_sub_list_active(host)
    test_sub_list_type_msg(host)
    test_sub_list_type_file(host)
    test_sub_list_type_door(host)
    test_sub_show_by_number(host)
    test_sub_show_by_gokey(host)
    test_sub_show_nonexistent(host)
    test_sub_tree(host)

    # Subboard mutations.
    if skip_mutations:
        runner.skip("Subboard mutations", "skipped via --skip-mutations")
    else:
        test_sub_create_edit_delete(host)
        test_sub_force_delete_reparent(host)
        test_sub_delete_root(host)

    # Message operations (includes msg move regression test).
    if skip_mutations:
        runner.skip("Message operations",
                    "skipped via --skip-mutations")
    else:
        test_msg_operations(host)

    # OLM timestamp test (requires user online, may skip).
    if skip_mutations:
        runner.skip("OLM timestamp test",
                    "skipped via --skip-mutations")
    else:
        test_olm_timestamp(host)

    # Extended read operations.
    test_sub_show_subop_fields(host)
    test_sub_show_activity_dates(host)
    test_user_plan(host)
    test_user_plan_nonexistent(host)
    test_stats_sam_labels(host)
    test_user_find_phone(host)
    test_conf_list(host)
    test_conf_list_all(host)

    # Group edit and transpose.
    if skip_mutations:
        runner.skip("Group edit/transpose",
                    "skipped via --skip-mutations")
    else:
        test_group_edit_transpose(host)

    # Config and control enhancements.
    print(flush=True)
    print("=== Config/Control Enhancements ===", flush=True)

    test_config_show_extended(host)
    test_config_flags_read(host)
    test_config_port_loaded(host)
    test_config_port_unloaded(host)
    test_config_port_invalid(host)
    test_config_flags_invalid(host)

    if skip_mutations:
        runner.skip("Config flags write/readback",
                    "skipped via --skip-mutations")
        runner.skip("Config reload-text",
                    "skipped via --skip-mutations")
    else:
        test_config_flags_write(host)
        test_config_reload_text(host)

    # Subboard edit extensions (access restrictions + boolean flags).
    test_sub_show_new_fields(host)

    if skip_mutations:
        runner.skip("Sub edit access restrictions",
                    "skipped via --skip-mutations")
        runner.skip("Sub edit boolean flags",
                    "skipped via --skip-mutations")
        runner.skip("Sub edit obits flags",
                    "skipped via --skip-mutations")
    else:
        test_sub_edit_access_restrictions(host)
        test_sub_edit_boolean_flags(host)
        test_sub_edit_obits_flags(host)

    # Utility integrations.
    test_sub_show_access_groups(host)
    test_sub_list_no_access_groups(host)
    test_group_show_access_groups(host)
    test_group_list_no_access_groups(host)
    test_user_show_address_type(host)
    test_sub_disk_usage(host)
    test_sub_disk_usage_invalid(host)

    if skip_mutations:
        runner.skip("Sub edit access groups mutation",
                    "skipped via --skip-mutations")
        runner.skip("File validate range",
                    "skipped via --skip-mutations")
    else:
        test_sub_edit_access_groups_mutation(host)
        test_file_validate_range(host)

    # Event commands.
    print(flush=True)
    print("=== Event Commands ===", flush=True)

    test_event_list_empty(host)
    test_event_list_all_empty(host)
    test_event_show_no_events(host)
    test_event_show_negative_index(host)

    # File and log enhancements.
    print(flush=True)
    print("=== File + Log Enhancements ===", flush=True)

    test_file_missing_all(host)
    test_file_missing_specific_sub(host)
    test_file_missing_invalid_sub(host)
    test_file_missing_msg_sub(host)
    test_log_callers_parsed(host)
    test_log_callers_parsed_tail(host)
    test_log_callers_parsed_record_structure(host)
    test_log_callers_parsed_tail_1(host)

    # Maintenance operations.
    test_maint_pointers(host)
    test_maint_count_dry_run(host)
    test_maint_count_single_sub(host)
    test_maint_repair_mail_dry_run(host)
    test_maint_repair_mail_single_user(host)
    test_maint_repair_sub_unavailable(host)

    # BBSList, vote, and mail alias.
    test_bbslist_list(host)
    test_bbslist_list_all(host)

    test_vote_list(host)
    test_vote_show(host)
    test_vote_results(host)

    test_mail_alias_list(host)

    if skip_mutations:
        runner.skip("Mail alias add/remove",
                    "skipped via --skip-mutations")
    else:
        test_mail_alias_mutations(host)

    # Final summary.
    rc = runner.summary()
    sys.exit(rc)


if __name__ == "__main__":
    main()
