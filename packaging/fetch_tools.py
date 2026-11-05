"""Fetch immutable AppImage assets, verifying bytes before replacing cached tools."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def checksum(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def fetch_tool(entry, destination):
    destination.mkdir(parents=True, exist_ok=True)
    target = destination / entry['name']
    if target.is_file() and checksum(target) == entry['sha256']:
        target.chmod(0o755)
        return target
    with tempfile.NamedTemporaryFile(dir=destination, delete=False) as stream:
        temporary = Path(stream.name)
    try:
        subprocess.run(['curl', '--fail', '--location', '--retry', '3',
                        '--proto', '=https', '--proto-redir', '=https',
                        '--header', 'Accept: application/octet-stream',
                        entry['url'], '--output', str(temporary)], check=True)
        if checksum(temporary) != entry['sha256']:
            raise RuntimeError(f"Checksum mismatch for {target.name}. Review tools.lock.json; do not bypass verification.")
        temporary.chmod(0o755)
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    return target


def main():
    for entry in json.loads((ROOT / 'packaging/tools.lock.json').read_text()):
        fetch_tool(entry, ROOT / 'build/packaging/tools')


if __name__ == '__main__':
    main()
