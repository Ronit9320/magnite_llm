"""Check every staged ELF's required glibc version against the release baseline."""
import argparse
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("appdir", type=Path)
    parser.add_argument("--maximum", default="2.35")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    maximum = tuple(map(int, args.maximum.split(".")))
    highest = (2, 0)
    offenders = []
    checked = 0
    for path in args.appdir.rglob("*"):
        if path.is_symlink() or not path.is_file():
            continue
        with path.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                continue
        checked += 1
        result = subprocess.run(["readelf", "--version-info", str(path)], capture_output=True, text=True, check=True)
        versions = [tuple(map(int, v.split("."))) for v in re.findall(r"\bGLIBC_(\d+\.\d+(?:\.\d+)?)\b", result.stdout)]
        required = max(versions, default=(2, 0))
        highest = max(highest, required)
        if required > maximum:
            offenders.append(str(path.relative_to(args.appdir)) + ": " + ".".join(map(str, required)))
    if not checked:
        raise SystemExit("No ELF files were checked")
    report = {"minimum_glibc": ".".join(map(str, highest)), "maximum_allowed": args.maximum,
              "elf_files": checked, "offenders": offenders}
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    if offenders:
        raise SystemExit("Unsupported glibc requirements:\n" + "\n".join(offenders))
    print(f"glibc audit: {checked} ELF files; highest requirement {report['minimum_glibc']}")


if __name__ == "__main__":
    main()
