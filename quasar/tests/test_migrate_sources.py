from __future__ import annotations

import pytest

from quasar.migrate_sources import (
    describe_external_source,
    is_database_uri,
    normalize_engine_name,
    parse_database_uri,
)


@pytest.mark.parametrize(
    "uri,engine",
    [
        ("postgresql://user:pass@localhost:5432/app", "postgres"),
        ("mysql+pymysql://root@127.0.0.1/shop", "mysql"),
        ("oracle+oracledb://user:pass@host/service", "oracle"),
        ("duckdb:///tmp/warehouse.duckdb", "duckdb"),
        ("redshift+psycopg2://user:pass@cluster.region.redshift.amazonaws.com:5439/dev", "postgres"),
        ("awsathena+rest://@athena.us-east-1.amazonaws.com:443/default?s3_staging_dir=s3://bucket/", "athena"),
    ],
)
def test_database_uri_detection(uri: str, engine: str):
    assert is_database_uri(uri)
    profile = parse_database_uri(uri)
    assert profile.engine == engine


def test_describe_external_source_notes():
    info = describe_external_source("postgresql://localhost/db")
    assert info.kind == "external"
    assert info.engine == "postgres"
    assert any("SQLAlchemy" in n for n in info.notes)


def test_normalize_engine_aliases():
    assert normalize_engine_name("postgresql") == "postgres"
    assert normalize_engine_name("mariadb") == "mysql"
    assert normalize_engine_name("redshift+psycopg2") == "postgres"
