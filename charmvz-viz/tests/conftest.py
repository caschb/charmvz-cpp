"""Shared test fixtures — synthetic Parquet datasets for unit tests.

Creates a tiny but structurally complete trace dataset (4 PEs, a handful of
executions, messages, and idle intervals) that exercises all entity tables
without needing real trace files.
"""

from __future__ import annotations

import tempfile
from pathlib import Path

import polars as pl
import pyarrow as pa
import pyarrow.parquet as pq
import pytest


def _write_pq(path: Path, schema: pa.Schema, data: dict) -> None:
    """Write a Parquet file from a dict of column data."""
    table = pa.table(data, schema=schema)
    pq.write_table(table, path)


@pytest.fixture
def tiny_trace(tmp_path: Path) -> Path:
    """Create a minimal synthetic trace dataset and return its directory path.

    Layout:
    - 4 PEs (0–3)
    - 2 chare collections (0: "Main", 1: "Worker[1D]")
    - 3 entry methods
    - 4 chare instances
    - 12 executions (~3 per PE)
    - 6 messages
    - 8 idle intervals
    - 1 migration episode
    """
    out = tmp_path / "trace"
    out.mkdir()

    # ── processing_element.parquet ───────────────────────────────────────
    _write_pq(
        out / "processing_element.parquet",
        pa.schema([
            ("pe_id", pa.int32()),
            ("total_pes", pa.int32()),
            ("begin_time_us", pa.int64()),
            ("end_time_us", pa.int64()),
            ("global_start_us", pa.int64()),
            ("computation_duration_us", pa.int64()),
            ("aligned_begin_us", pa.int64()),
        ]),
        {
            "pe_id": [0, 1, 2, 3],
            "total_pes": [4, 4, 4, 4],
            "begin_time_us": [1000, 1100, 1200, 1050],
            "end_time_us": [10_000_000, 10_000_100, 10_000_200, 10_000_050],
            "global_start_us": [1000, 1000, 1000, 1000],
            "computation_duration_us": [9_999_000, 9_999_000, 9_999_000, 9_999_000],
            "aligned_begin_us": [0, 100, 200, 50],
        },
    )

    # ── chare_collection.parquet ─────────────────────────────────────────
    _write_pq(
        out / "chare_collection.parquet",
        pa.schema([
            ("collection_id", pa.int32()),
            ("name", pa.utf8()),
            ("ndims", pa.int32()),
        ]),
        {
            "collection_id": [0, 1],
            "name": ["Main", "Worker"],
            "ndims": [-1, 1],
        },
    )

    # ── entry_method.parquet ─────────────────────────────────────────────
    _write_pq(
        out / "entry_method.parquet",
        pa.schema([
            ("ep_id", pa.int32()),
            ("name", pa.utf8()),
            ("collection_id", pa.int32()),
            ("msg_idx", pa.int32()),
        ]),
        {
            "ep_id": [0, 1, 2],
            "name": ["Main::main", "Worker::compute", "Worker::reduce"],
            "collection_id": [0, 1, 1],
            "msg_idx": [0, 1, 2],
        },
    )

    # ── chare_instance.parquet ───────────────────────────────────────────
    _write_pq(
        out / "chare_instance.parquet",
        pa.schema([
            ("instance_id", pa.int64()),
            ("collection_id", pa.int32()),
            ("index_0", pa.int32()),
            ("index_1", pa.int32()),
            ("index_2", pa.int32()),
            ("index_3", pa.int32()),
        ]),
        {
            "instance_id": [0, 1, 2, 3],
            "collection_id": [0, 1, 1, 1],
            "index_0": [0, 0, 1, 2],
            "index_1": [0, 0, 0, 0],
            "index_2": [0, 0, 0, 0],
            "index_3": [0, 0, 0, 0],
        },
    )

    # ── execution.parquet ────────────────────────────────────────────────
    n_exec = 12
    pe_ids = [0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3]
    events = list(range(1, n_exec + 1))
    ep_ids = [0, 1, 2, 1, 2, 1, 1, 2, 1, 1, 1, 2]
    start_times = [
        100_000, 500_000, 1_500_000,
        200_000, 800_000, 2_000_000,
        150_000, 600_000, 1_800_000,
        300_000, 900_000, 2_500_000,
    ]
    durations = [
        300_000, 800_000, 400_000,
        500_000, 600_000, 700_000,
        450_000, 350_000, 550_000,
        600_000, 400_000, 300_000,
    ]
    end_times = [s + d for s, d in zip(start_times, durations)]

    _write_pq(
        out / "execution.parquet",
        pa.schema([
            ("pe_id", pa.int32()),
            ("event", pa.int32()),
            ("instance_id", pa.int64()),
            ("ep_id", pa.int32()),
            ("src_pe", pa.int32()),
            ("msg_idx", pa.int32()),
            ("msg_len", pa.int32()),
            ("start_time_us", pa.int64()),
            ("recv_time_us", pa.int64()),
            ("start_cpu_us", pa.int64()),
            ("end_time_us", pa.int64()),
            ("end_cpu_us", pa.int64()),
            ("papi_begin_0", pa.int64()),
            ("papi_begin_1", pa.int64()),
            ("papi_begin_2", pa.int64()),
            ("papi_begin_3", pa.int64()),
            ("papi_begin_4", pa.int64()),
            ("papi_begin_5", pa.int64()),
            ("papi_end_0", pa.int64()),
            ("papi_end_1", pa.int64()),
            ("papi_end_2", pa.int64()),
            ("papi_end_3", pa.int64()),
            ("papi_end_4", pa.int64()),
            ("papi_end_5", pa.int64()),
            ("wall_duration_us", pa.int64()),
            ("cpu_duration_us", pa.int64()),
            ("queue_wait_us", pa.int64()),
            ("papi_delta_0", pa.int64()),
            ("papi_delta_1", pa.int64()),
            ("papi_delta_2", pa.int64()),
            ("papi_delta_3", pa.int64()),
            ("papi_delta_4", pa.int64()),
            ("papi_delta_5", pa.int64()),
        ]),
        {
            "pe_id": pe_ids,
            "event": events,
            "instance_id": [0, 1, 1, 2, 2, 2, 3, 3, 3, 1, 2, 3],
            "ep_id": ep_ids,
            "src_pe": [0, 0, 0, 0, 1, 0, 0, 2, 1, 0, 3, 2],
            "msg_idx": [0] * n_exec,
            "msg_len": [100, 200, 150, 250, 180, 300, 220, 160, 280, 240, 190, 210],
            "start_time_us": start_times,
            "recv_time_us": [None] * n_exec,
            "start_cpu_us": start_times,
            "end_time_us": end_times,
            "end_cpu_us": end_times,
            "papi_begin_0": [None] * n_exec,
            "papi_begin_1": [None] * n_exec,
            "papi_begin_2": [None] * n_exec,
            "papi_begin_3": [None] * n_exec,
            "papi_begin_4": [None] * n_exec,
            "papi_begin_5": [None] * n_exec,
            "papi_end_0": [None] * n_exec,
            "papi_end_1": [None] * n_exec,
            "papi_end_2": [None] * n_exec,
            "papi_end_3": [None] * n_exec,
            "papi_end_4": [None] * n_exec,
            "papi_end_5": [None] * n_exec,
            "wall_duration_us": durations,
            "cpu_duration_us": durations,
            "queue_wait_us": [None] * n_exec,
            "papi_delta_0": [None] * n_exec,
            "papi_delta_1": [None] * n_exec,
            "papi_delta_2": [None] * n_exec,
            "papi_delta_3": [None] * n_exec,
            "papi_delta_4": [None] * n_exec,
            "papi_delta_5": [None] * n_exec,
        },
    )

    # ── message.parquet ──────────────────────────────────────────────────
    _write_pq(
        out / "message.parquet",
        pa.schema([
            ("message_id", pa.int64()),
            ("src_pe", pa.int32()),
            ("event", pa.int32()),
            ("ep_id", pa.int32()),
            ("msg_idx", pa.int32()),
            ("msg_len", pa.int32()),
            ("send_time_us", pa.int64()),
            ("enqueue_time_us", pa.int64()),
            ("is_broadcast", pa.bool_()),
            ("broadcast_fanout", pa.int32()),
            ("dst_pe", pa.int32()),
            ("recv_time_us", pa.int64()),
            ("exec_start_time_us", pa.int64()),
            ("send_to_enqueue_us", pa.int64()),
            ("enqueue_to_exec_us", pa.int64()),
            ("end_to_end_us", pa.int64()),
        ]),
        {
            "message_id": list(range(6)),
            "src_pe": [0, 0, 1, 0, 2, 3],
            "event": [1, 2, 3, 4, 5, 6],
            "ep_id": [1, 1, 2, 1, 1, 2],
            "msg_idx": [1, 1, 2, 1, 1, 2],
            "msg_len": [200, 250, 180, 300, 220, 190],
            "send_time_us": [50_000, 400_000, 700_000, 1_400_000, 1_700_000, 2_400_000],
            "enqueue_time_us": [None] * 6,
            "is_broadcast": [False, False, False, False, False, False],
            "broadcast_fanout": [None] * 6,
            "dst_pe": [1, 2, 3, 3, 1, 0],
            "recv_time_us": [None] * 6,
            "exec_start_time_us": [200_000, 150_000, 900_000, 300_000, 900_000, 2_500_000],
            "send_to_enqueue_us": [None] * 6,
            "enqueue_to_exec_us": [None] * 6,
            "end_to_end_us": [150_000, None, 200_000, None, None, 100_000],
        },
    )

    # ── idle_interval.parquet ────────────────────────────────────────────
    _write_pq(
        out / "idle_interval.parquet",
        pa.schema([
            ("pe_id", pa.int32()),
            ("start_time_us", pa.int64()),
            ("end_time_us", pa.int64()),
            ("duration_us", pa.int64()),
        ]),
        {
            "pe_id": [0, 0, 1, 1, 2, 2, 3, 3],
            "start_time_us": [
                400_000, 1_900_000,
                700_000, 2_700_000,
                1_000_000, 2_350_000,
                50_000, 2_800_000,
            ],
            "end_time_us": [
                500_000, 2_500_000,
                800_000, 3_000_000,
                1_200_000, 2_500_000,
                300_000, 3_200_000,
            ],
            "duration_us": [
                100_000, 600_000,
                100_000, 300_000,
                200_000, 150_000,
                250_000, 400_000,
            ],
        },
    )

    # ── migration_episode.parquet ────────────────────────────────────────
    _write_pq(
        out / "migration_episode.parquet",
        pa.schema([
            ("migration_id", pa.int64()),
            ("src_pe", pa.int32()),
            ("pack_start_us", pa.int64()),
            ("pack_end_us", pa.int64()),
            ("dst_pe", pa.int32()),
            ("unpack_start_us", pa.int64()),
            ("unpack_end_us", pa.int64()),
            ("instance_id", pa.int64()),
            ("ambiguous", pa.bool_()),
            ("pack_duration_us", pa.int64()),
            ("unpack_duration_us", pa.int64()),
            ("total_migration_us", pa.int64()),
            ("network_transit_us", pa.int64()),
        ]),
        {
            "migration_id": [0],
            "src_pe": [0],
            "pack_start_us": [1_300_000],
            "pack_end_us": [1_310_000],
            "dst_pe": [2],
            "unpack_start_us": [1_320_000],
            "unpack_end_us": [1_330_000],
            "instance_id": [1],
            "ambiguous": [False],
            "pack_duration_us": [10_000],
            "unpack_duration_us": [10_000],
            "total_migration_us": [30_000],
            "network_transit_us": [10_000],
        },
    )

    return out


@pytest.fixture
def ds(tiny_trace: Path):
    """Return a TraceDataset loaded from the synthetic trace."""
    from charmvz_vis import TraceDataset

    return TraceDataset(tiny_trace)
