"""
exception.py — errors raised by tracecliff modules.
"""


class TracecliffError(Exception):
    """Base of every tracecliff error."""


class AxisConflictError(TracecliffError):
    """Two axes share a Makefile variable but declare different values."""


class SweepCsvError(TracecliffError):
    """A sweep CSV does not match the Bench it was matched to."""
