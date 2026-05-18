"""Structured errors for production logging and HTTP responses."""

from __future__ import annotations


class QuasarError(Exception):
    """Base Quasar error."""


class QuasarConfigError(QuasarError):
    """Invalid cluster configuration."""


class QuasarSecurityError(QuasarError):
    """Security policy violation."""


class QuasarOverloadError(QuasarError):
    """Backpressure: pool queue full or rate limit exceeded."""


class QuasarCircuitOpenError(QuasarError):
    """Circuit breaker rejected request for a shard."""


class QuasarRestoreError(QuasarError):
    """Unsafe restore (database may be in use)."""


class QuasarLockError(QuasarError):
    """Advisory database lock could not be acquired."""
