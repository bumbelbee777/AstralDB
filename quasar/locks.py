"""Advisory file locks so only one Quasar process writes a database path at a time."""

from __future__ import annotations

import os
import sys
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Generator, Iterable, List, Optional, Union

from quasar.errors import QuasarLockError

PathLike = Union[str, Path]


class DatabaseLock:
    """
    Exclusive lock file next to the database (``.quasar/locks/<name>.lock``).

    Uses ``fcntl`` on Unix and ``msvcrt`` on Windows. Non-blocking by default;
    optional timeout polls until the lock is free.
    """

    def __init__(self, database: PathLike, *, timeout_sec: float = 0.0) -> None:
        self.database = Path(database).resolve()
        lock_dir = self.database.parent / ".quasar" / "locks"
        lock_dir.mkdir(parents=True, exist_ok=True)
        self.lock_path = lock_dir / f"{self.database.name}.lock"
        self.timeout_sec = max(0.0, float(timeout_sec))
        self._fh: Optional[object] = None

    def acquire(self) -> None:
        deadline = time.monotonic() + self.timeout_sec
        while True:
            try:
                self._try_acquire_once()
                return
            except QuasarLockError:
                if time.monotonic() >= deadline:
                    raise QuasarLockError(
                        f"database locked (timeout {self.timeout_sec}s): {self.database}"
                    ) from None
                time.sleep(0.05)

    def _try_acquire_once(self) -> None:
        self.lock_path.parent.mkdir(parents=True, exist_ok=True)
        fh = open(self.lock_path, "a+", encoding="utf-8")
        try:
            if sys.platform == "win32":
                import msvcrt

                fh.seek(0)
                msvcrt.locking(fh.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl

                fcntl.flock(fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            fh.close()
            raise QuasarLockError(f"database locked: {self.database}") from exc
        fh.seek(0)
        fh.truncate()
        fh.write(f"{os.getpid()}\n{self.database}\n")
        fh.flush()
        self._fh = fh

    def release(self) -> None:
        if self._fh is None:
            return
        fh = self._fh
        self._fh = None
        try:
            if sys.platform == "win32":
                import msvcrt

                fh.seek(0)
                msvcrt.locking(fh.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl

                fcntl.flock(fh.fileno(), fcntl.LOCK_UN)
        finally:
            fh.close()

    def __enter__(self) -> "DatabaseLock":
        self.acquire()
        return self

    def __exit__(self, *args: object) -> None:
        self.release()


@contextmanager
def lock_databases(
    paths: Iterable[PathLike],
    *,
    timeout_sec: float = 0.0,
) -> Generator[None, None, None]:
    """Acquire locks for multiple DB paths in sorted order (deadlock avoidance)."""
    unique = sorted({Path(p).resolve() for p in paths}, key=lambda p: str(p))
    locks = [DatabaseLock(p, timeout_sec=timeout_sec) for p in unique]
    acquired: List[DatabaseLock] = []
    try:
        for lock in locks:
            lock.acquire()
            acquired.append(lock)
        yield
    finally:
        for lock in reversed(acquired):
            lock.release()
