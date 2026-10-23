"""Discard previous native deployment, retaining only the expensive Python runtime.

linuxdeploy does not replace already staged libraries. Keeping an old AppDir
after a Qt upgrade can silently combine a new client with incompatible old Qt.
"""
from pathlib import Path
import shutil


def main():
    repo = Path(__file__).resolve().parents[1]
    stage = repo / "build/packaging/Magnite.AppDir"
    if not stage.exists():
        return
    if stage.is_symlink() or not stage.resolve().is_relative_to(repo):
        raise SystemExit("Refusing to clean staging outside the checkout")

    def clean(directory, retained):
        for path in directory.iterdir():
            if path.name in retained:
                if path.is_symlink() or not path.is_dir():
                    raise SystemExit(f"Unexpected staging path: {path}")
                children = retained[path.name]
                if children is not None:
                    clean(path, children)
            elif path.is_dir() and not path.is_symlink():
                shutil.rmtree(path)
            else:
                path.unlink()

    clean(stage, {"usr": {"bin": {"runtime": None}}})


if __name__ == "__main__":
    main()
