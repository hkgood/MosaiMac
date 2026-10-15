#!/usr/bin/env python3
"""
Push the working tree to GitHub through the REST API.

The sandbox routes all outbound HTTPS through a MITM proxy whose CA the
system curl trusts but this git build refuses to load ("CAfile: none" even
with http.sslCAFile and GIT_CAINFO both set). Rather than disable certificate
verification -- which would buy nothing here, since the proxy already sees
every byte in plaintext -- this goes over the HTTP API using the same CA
bundle curl is using.

  python3 tools/push_via_api.py hkgood/MosaiMac main
"""

import base64
import json
import os
import ssl
import subprocess
import sys
import urllib.error
import urllib.request

CA = "/etc/ssl/certs/agent-identity/sandbox-gateway-ca.crt"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
API = "https://api.github.com"


def token():
    return os.environ["GITHUB_TOKEN"].strip()


def ctx():
    c = ssl.create_default_context(cafile=CA)
    return c


def call(method, path, payload=None):
    url = path if path.startswith("http") else API + path
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", "Bearer " + token())
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("X-GitHub-Api-Version", "2022-11-28")
    req.add_header("User-Agent", "qd-push")
    if data:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, context=ctx(), timeout=45) as r:
            return json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        body = e.read().decode(errors="replace")[:400]
        sys.exit(f"HTTP {e.code} on {method} {path}\n{body}")


def tracked_files():
    out = subprocess.run(
        ["git", "-C", ROOT, "ls-files"], capture_output=True, text=True, check=True
    ).stdout
    return [f for f in out.split("\n") if f.strip()]


def head_sha(repo, branch):
    """
    Current tip of a branch, or None if the repository is empty.

    An empty repository answers 409 rather than 404 to the ref lookup, which
    is easy to misread as a permissions problem, so both are treated as
    "nothing here yet".
    """
    try:
        return call("GET", f"/repos/{repo}/git/ref/heads/{branch}")["object"]["sha"]
    except SystemExit as e:
        if "404" in str(e) or "409" in str(e):
            return None
        raise


def bootstrap_empty(repo, branch, name, content):
    """
    Seed a brand-new repository with one commit.

    The Git data API refuses to create blobs on a repository that has no refs
    at all ("409 Git Repository is empty"), so the first commit has to go
    through the contents API. Whatever we seed gets replaced by the real tree
    in the next step, so pick something small.
    """
    call(
        "PUT",
        f"/repos/{repo}/contents/{name}",
        {
            "message": "Initial commit",
            "content": base64.b64encode(content.encode()).decode(),
            "branch": branch,
        },
    )
    return head_sha(repo, branch)


def main():
    repo = sys.argv[1] if len(sys.argv) > 1 else "hkgood/MosaiMac"
    branch = sys.argv[2] if len(sys.argv) > 2 else "main"
    files = tracked_files()
    print(f"repo  {repo}  branch {branch}")
    print(f"{len(files)} files")

    # 0. A repository with no refs makes the Git data API refuse to create
    #    blobs at all, so seed a first commit through the contents API before
    #    touching it. Its placeholder content is replaced by the real tree in
    #    step 2.
    parent = head_sha(repo, branch)
    if parent is None:
        parent = bootstrap_empty(repo, branch, files[0], "placeholder\n")
        print(f"  seeded {branch} with a placeholder commit")

    # 1. blobs
    blobs = {}
    for i, f in enumerate(files, 1):
        with open(os.path.join(ROOT, f), "rb") as fh:
            raw = fh.read()
        blobs[f] = call(
            "POST",
            f"/repos/{repo}/git/blobs",
            {"content": base64.b64encode(raw).decode(), "encoding": "base64"},
        )["sha"]
        print(f"  [{i:2d}/{len(files)}] blob {f}")

    # 2. tree
    tree = call(
        "POST",
        f"/repos/{repo}/git/trees",
        {
            "base_tree": None,
            "tree": [
                {"path": f, "mode": "100644", "type": "blob", "sha": blobs[f]}
                for f in files
            ],
        },
    )["sha"]

    # 3. commit -- reuse the local message so the history matches
    msg = subprocess.run(
        ["git", "-C", ROOT, "log", "-1", "--pretty=%B"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()
    commit = call(
        "POST",
        f"/repos/{repo}/git/commits",
        {"message": msg, "tree": tree, "parents": [parent]},
    )["sha"]

    # 4. point the branch at it
    # Force, because re-running this against a repo that already has the
    # previous attempt as its tip is not a fast-forward. There is no history
    # here worth protecting: this is how the branch gets created in the first
    # place, and every push carries the whole tree.
    call("PATCH", f"/repos/{repo}/git/refs/heads/{branch}",
          {"sha": commit, "force": True})
    print(f"\npushed {commit} -> {branch}")


if __name__ == "__main__":
    main()
