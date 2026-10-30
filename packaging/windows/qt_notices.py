"""Collect a conservative superset of Qt 6.8.3 qtbase/qtsvg license notices."""
import json
from pathlib import Path
import posixpath
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from macos.qt_notices import QT_TAG, QT_OWN_LICENSES, attributions, fetch, license_files


def main():
    output = Path(sys.argv[1])
    output.mkdir(parents=True, exist_ok=True)
    cache = Path(__file__).resolve().parents[2] / "build/packaging/qt-notice-metadata"
    cache.mkdir(parents=True, exist_ok=True)
    for name in QT_OWN_LICENSES:
        (output / name).write_bytes(fetch(f"https://raw.githubusercontent.com/qt/qtbase/{QT_TAG}/LICENSES/{name}"))
    metadata = []
    for repo, path, component in attributions(cache):
        metadata.append({"repo": repo, "path": path, "component": component})
        for relative in license_files(component):
            source = posixpath.normpath(posixpath.join(posixpath.dirname(path), relative))
            if source.startswith("../"):
                raise RuntimeError(f"Qt license path escapes repository: {source}")
            destination = output / "third-party" / repo / component["Id"] / Path(relative).name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(fetch(f"https://raw.githubusercontent.com/qt/{repo}/{QT_TAG}/{source}"))
    (output / "attributions.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (output / "README.md").write_text(
        "# Qt 6.8.3 Windows license notices\n\n"
        "Qt is dynamically linked; replace its DLLs/plugins with compatible builds to modify Qt.\n"
        "Exact source: https://download.qt.io/archive/qt/6.8/6.8.3/ and the v6.8.3 tags of\n"
        "https://github.com/qt/qtbase and https://github.com/qt/qtsvg.\n\n"
        "This collects all qtbase/qtsvg attribution metadata and named license files, including\n"
        "components not compiled into this Windows configuration. It is a conservative superset;\n"
        "the metadata records notices whose licensing is stated inline. Release licensing review\n"
        "remains open. It does not reuse Mac-specific component inclusion claims.\n")


if __name__ == "__main__":
    main()
