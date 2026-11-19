"""Measure same-host Gloo completion with the receiver already waiting."""

import json
from datetime import timedelta
from pathlib import Path
from tempfile import TemporaryDirectory
from time import CLOCK_MONOTONIC_RAW, clock_gettime_ns

from markov_adapters.artifacts import write_json
from markov_adapters.pytorch import CpuProtocolWork


class GlooSamples:
    observations = 5

    @staticmethod
    def geometry(identity, features):
        return identity["dtype"], features["axes"]["parameter.elements"]

    @classmethod
    def run(cls, records, output):
        work = {}
        for identity, features in CpuProtocolWork(records).wires():
            key = (identity["source"], identity["target"], identity["channel"], *cls.geometry(identity, features))
            work[key] = identity, features
        geometries = tuple(dict.fromkeys(cls.geometry(*item) for item in work.values()))
        if not geometries:
            write_json(output, [], replace=False)
            return

        from torch.multiprocessing import get_context, spawn

        context = get_context("spawn")
        with TemporaryDirectory(dir=output.parent) as temporary:
            directory = Path(temporary)
            ready, exchanged = context.Event(), context.Barrier(2)
            started = context.Value("q", 0, lock=False)
            spawn(cls.measure, args=(directory, geometries, ready, exchanged, started), nprocs=2)
            measurements = json.loads((directory / "samples.json").read_text())

        durations = dict(zip(geometries, measurements, strict=True))
        write_json(output, [
            {"identity": identity, "features": features, "observations_ns": durations[cls.geometry(identity, features)]}
            for identity, features in work.values()
        ], replace=False)

    @classmethod
    def measure(cls, rank, directory, geometries, ready, exchanged, started):
        import torch
        import torch.distributed as distributed

        distributed.init_process_group(
            "gloo", init_method=(directory / "rendezvous").as_uri(), world_size=2, rank=rank, timeout=timedelta(seconds=20),
        )
        observations = []
        try:
            for dtype, elements in geometries:
                payload = torch.zeros(elements, dtype=getattr(torch, dtype.removeprefix("torch.")))
                samples = []
                for trial in range(cls.observations + 1):
                    if rank == 0:
                        ready.wait()
                        sent = clock_gettime_ns(CLOCK_MONOTONIC_RAW)
                        work = distributed.isend(payload, 1)
                        started.value = sent
                        work.wait()
                    else:
                        work = distributed.irecv(payload, 0)
                        ready.set()
                        waiting = clock_gettime_ns(CLOCK_MONOTONIC_RAW)
                        work.wait()
                        visible = clock_gettime_ns(CLOCK_MONOTONIC_RAW)
                        ready.clear()
                    exchanged.wait()

                    # An already-posted, waiting receiver observes completion without scheduler polling.
                    if rank == 1 and trial and waiting <= started.value <= visible:
                        samples.append(visible - started.value)
                observations.append(samples)
        finally:
            distributed.destroy_process_group()
        if rank == 1:
            write_json(directory / "samples.json", observations, replace=False)
