"""Tests for consistent hash ring."""

from quasar.ring import ConsistentHashRing


def test_ring_returns_valid_node():
    ring = ConsistentHashRing(["a", "b", "c"], virtual_nodes=32)
    for key in ("user:1", "user:2", "order:99", "x" * 100):
        assert ring.node_for_key(key) in {"a", "b", "c"}


def test_ring_stable_for_same_key():
    ring = ConsistentHashRing(["n1", "n2"], virtual_nodes=64)
    assert ring.node_for_key("stable") == ring.node_for_key("stable")


def test_ring_nodes_property():
    ring = ConsistentHashRing(["x", "y"])
    assert ring.nodes == ["x", "y"]
