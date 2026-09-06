from __future__ import annotations

import os


def app_home() -> str:
    """Resolve the Magnite data directory (settable via MAGNITE_HOME)."""
    app_dir = os.environ.get("MAGNITE_HOME")
    if not app_dir:
        app_dir = os.path.join(os.path.expanduser("~"), ".magnite")
        if os.name == "nt":
            app_dir = os.path.join(os.environ.get("APPDATA") or app_dir, "Magnite")
    return app_dir