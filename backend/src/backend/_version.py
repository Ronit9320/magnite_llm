"""Canonical application release identity; consumed by CMake and packaging too."""
import re

RELEASE_VERSION = "v0.2-preview"
# The GitHub release title for this version.
RELEASE_NAME = "magnite-spacev0.2"
UPDATE_SEQUENCE = 2
DATA_SCHEMA = 2

# Python distribution metadata requires PEP 440; derive its spelling rather
# than maintaining another independent application version.
_match = re.fullmatch(r"v(\d+\.\d+(?:\.\d+)?)-preview(?:\.(\d+))?", RELEASE_VERSION)
if _match is None:
    raise ValueError("Expected a vMAJOR.MINOR[.PATCH]-preview[.N] release")
PACKAGE_VERSION = _match[1] + "rc" + (_match[2] or "0")
