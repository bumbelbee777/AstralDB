"""Cross-platform path helpers for Quasar config files and CLI subprocesses."""

from __future__ import annotations

import os
from pathlib import Path
from typing import Optional, Union

PathLike = Union[str, Path]


def coerce_path(value: PathLike) -> Path:
    """Build a :class:`Path` from config or CLI input (JSON often uses forward slashes)."""
    if isinstance(value, Path):
        return value
    text = str(value).strip()
    if os.name == "nt":
        text = text.replace("\\", "/")
    return Path(text)


def resolve_path(value: PathLike, *, base: Optional[Path] = None) -> Path:
    """Resolve relative paths against an optional config directory."""
    target = coerce_path(value)
    if base is not None and not target.is_absolute():
        target = base / target
    return target.resolve()


def path_for_config(path: Path) -> str:
    """Portable path string for cluster JSON (forward slashes on every OS)."""
    return path.resolve().as_posix()


def path_for_cli(path: Path) -> str:
    """Native path string for AstralDB subprocess arguments."""
    return os.fspath(path.resolve())


def path_key(path: PathLike) -> str:
    """Canonical map/set key for one on-disk file (case-insensitive on Windows)."""
    resolved = coerce_path(path).resolve()
    text = os.fspath(resolved)
    if os.name == "nt":
        return os.path.normcase(text)
    return text


def is_path_under_base(target: Path, base: Path) -> bool:
    """Return whether *target* is inside *base* (case-insensitive on Windows)."""
    base_r = base.resolve()
    target_r = target.resolve()
    if os.name == "nt":
        b = os.path.normcase(os.fspath(base_r))
        t = os.path.normcase(os.fspath(target_r))
        if t == b:
            return True
        prefix = b if b.endswith("\\") else b + "\\"
        return t.startswith(prefix)
    try:
        target_r.relative_to(base_r)
        return True
    except ValueError:
        return False


def resolve_path_under_base(
    base: Path,
    value: str,
    *,
    allow_outside: bool = False,
) -> Path:
    """Resolve a config-relative path and optionally enforce directory containment."""
    if not value or not isinstance(value, str):
        from quasar.errors import QuasarSecurityError

        raise QuasarSecurityError("path must be a non-empty string")
    if "\x00" in value:
        from quasar.errors import QuasarSecurityError

        raise QuasarSecurityError("path contains null bytes")
    base = base.resolve()
    target = coerce_path(value)
    if not target.is_absolute():
        target = (base / target).resolve()
    else:
        target = target.resolve()
    if not allow_outside and not is_path_under_base(target, base):
        from quasar.errors import QuasarSecurityError

        raise QuasarSecurityError(f"path escapes config directory: {value}")
    return target


def resolve_cluster_path(
    base: Optional[Path],
    value: str,
    *,
    allow_outside: bool = False,
) -> Path:
    """Resolve a cluster path; enforce containment only when *base* is set."""
    if base is None:
        if not value or not isinstance(value, str):
            from quasar.errors import QuasarSecurityError

            raise QuasarSecurityError("path must be a non-empty string")
        if "\x00" in value:
            from quasar.errors import QuasarSecurityError

            raise QuasarSecurityError("path contains null bytes")
        target = coerce_path(value)
        if target.is_absolute():
            return target.resolve()
        return (Path.cwd() / target).resolve()
    return resolve_path_under_base(base, value, allow_outside=allow_outside)


def resolve_config_path(
    base: Optional[Path],
    value: str,
    *,
    allow_outside: bool = False,
) -> str:
    """Resolve and return a portable config path string."""
    return path_for_config(resolve_cluster_path(base, value, allow_outside=allow_outside))
