"""Install the pinned Node executable and Pi dependencies beside bundled Python."""
from __future__ import annotations

import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
from tempfile import TemporaryDirectory
import zipfile

from fetch_tools import fetch_tool

PACKAGING = Path(__file__).resolve().parent


def prepare_pi(destination: Path, python: Path, cache: Path) -> dict[str, str]:
    lock = json.loads((PACKAGING / "node.lock.json").read_text(encoding="utf-8"))
    version = lock["version"]
    machine = platform.machine().lower()
    architecture = "x64" if machine in ("amd64", "x86_64") else machine
    system = "win" if sys.platform == "win32" else sys.platform
    target = f"{system}-{architecture}"
    entry = lock["archives"].get(target)
    if entry is None:
        raise RuntimeError(f"No pinned Node distribution for {target}")
    stem = f"node-v{version}-{target}"
    name = f"{stem}.{entry['extension']}"
    archive = fetch_tool({"name": name, "sha256": entry["sha256"],
                          "url": f"https://nodejs.org/dist/v{version}/{name}"}, cache)

    env = os.environ.copy()
    for key in ("NODE_OPTIONS", "NODE_PATH"):
        env.pop(key, None)
    env["npm_config_cache"] = str(cache / "npm")
    bridge = Path(subprocess.check_output([str(python), "-I", "-B", "-c",
        "from pathlib import Path; import backend.tooling; "
        "print(Path(backend.tooling.__file__).parent / 'pi')"], text=True, env=env).strip())

    with TemporaryDirectory(prefix="magnite-pi-", dir=cache) as folder:
        work = Path(folder)
        if archive.suffix == ".zip":
            with zipfile.ZipFile(archive) as zipped:
                zipped.extractall(work)
        else:
            with tarfile.open(archive) as tar:
                tar.extractall(work, filter="data")
        root = work / stem
        if system == "win":
            node, npm = root / "node.exe", root / "node_modules/npm/bin/npm-cli.js"
        else:
            node, npm = root / "bin/node", root / "lib/node_modules/npm/bin/npm-cli.js"
        if subprocess.check_output([str(node), "--version"], env=env, text=True).strip() != f"v{version}":
            raise RuntimeError(f"Unexpected Node version in {name}")
        install = work / "bridge"
        install.mkdir()
        for filename in ("package.json", "package-lock.json"):
            shutil.copy2(bridge / filename, install / filename)
        subprocess.run([str(node), str(npm), "ci", "--omit=dev", "--ignore-scripts", "--no-audit",
                        "--no-fund", "--no-bin-links"], cwd=install, env=env, check=True)
        modules = bridge / "node_modules"
        if modules.exists():
            shutil.rmtree(modules)
        shutil.move(install / "node_modules", modules)
        node_dir = destination / "runtime/node"
        node_dir.mkdir(exist_ok=True)
        shutil.copy2(node, node_dir / node.name)
        licenses = destination / "licenses/node"
        licenses.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / "LICENSE", licenses / "LICENSE")
    packages = json.loads((bridge / "package.json").read_text(encoding="utf-8"))
    return {"node": version, "pi_agent_core": packages["dependencies"]["@earendil-works/pi-agent-core"]}
