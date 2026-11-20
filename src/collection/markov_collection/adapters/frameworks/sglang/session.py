"""Coordinate native instances with one workload clock and independent engine lifecycles."""

import multiprocessing
import os
import signal
import socket
import time
import traceback
from contextlib import contextmanager
from multiprocessing.connection import wait
from pathlib import Path

from markov_adapters.sglang.configuration import CaptureInput, NativeConfiguration
from markov_collection.adapters.frameworks.sglang.instance import NativeServingInstance
from markov_adapters.artifacts import write_json


class ArrivalClock:
    def __init__(self, context):
        self.origin = context.Value("q", 0)

    def __call__(self):

        # Worker receipts and profiler host anchors use the same machine's unadjusted monotonic clock.
        self.origin.value = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)


class NativeRendezvous:
    host = "127.0.0.1"

    @classmethod
    @contextmanager
    def loopback_listener(cls):

        # Port zero excludes other listeners but can still select a host NAT destination.
        for _ in range(4):
            with socket.socket() as listener:
                listener.bind((cls.host, 0))
                listener.listen(1)
                listener.settimeout(0.25)
                try:
                    with socket.create_connection(listener.getsockname(), timeout=0.25) as probe:
                        accepted, peer = listener.accept()
                        with accepted:
                            if peer != probe.getsockname():
                                continue

                            # The client closes first so the native server can rebind the checked port.
                            probe.shutdown(socket.SHUT_WR)
                            accepted.settimeout(0.25)
                            accepted.recv(1)
                except (ConnectionError, TimeoutError):
                    continue

                listener.settimeout(None)
                yield listener
                return
        raise RuntimeError("no reachable owned loopback listener for the native session")

    def __init__(self) -> None:
        from torch.distributed import TCPStore

        # TCPStore takes ownership of the checked socket until every Engine worker exits.
        with self.loopback_listener() as listener:
            self.store = TCPStore(self.host, listener.getsockname()[1], is_master=True,
                                  wait_for_workers=False, master_listen_fd=listener.detach())
        self.environment = {
            "MASTER_ADDR": self.host, "MASTER_PORT": str(self.store.port),
            "TORCHELASTIC_USE_AGENT_STORE": "True", "SGLANG_DISTRIBUTED_INIT_METHOD_OVERRIDE": "env://",
        }


class NativeServingSession:
    def __init__(self, source, output: Path, *, resource_trace: bool = False, latency_only: bool = False,
                 host_timing: bool = False, service_reference: bool = False) -> None:
        self.framework_revision = NativeConfiguration.upstream_revision(os.environ.get("MARKOV_SGLANG_REVISION"))
        serving = CaptureInput(source)
        self.output = output.resolve()
        latency_only = latency_only or host_timing
        self.instances = []
        offset = 0
        for index, description in enumerate(serving.instances):
            instance = NativeServingInstance(
                serving, description, self.output, index, offset,
                resource_trace=resource_trace or service_reference, latency_only=latency_only,
                host_timing=host_timing, service_reference=service_reference,
            )
            self.instances.append(instance)
            offset += len(instance.roles)
        if latency_only and resource_trace:
            raise ValueError("light observation cannot include resource tracing")
        if latency_only and not host_timing and len(serving.submission_groups) != 1:
            raise ValueError("latency-only measurement requires one submission group")

    @staticmethod
    def execute(instance, barrier, clock, bootstrap, rendezvous, connection):

        # Each Engine owns descendants; cancellation must also stop that instance's native workers.
        try:
            os.setsid()
            connection.send((True, instance.run(barrier, clock, bootstrap, rendezvous)))
        except BaseException:

            # Keep the traceback available before EOF lets the owner cancel this process group.
            connection.send((False, traceback.format_exc()))
            raise
        finally:
            connection.close()

    def run(self) -> None:
        self.output.mkdir(parents=True)
        (self.output / "instances").mkdir()
        (self.output / "runtime").mkdir()
        context = multiprocessing.get_context("spawn")
        clock = ArrivalClock(context)
        barrier = context.Barrier(len(self.instances), action=clock)
        processes, pending, results, rendezvous = [], {}, {}, []
        try:
            for _ in self.instances:
                rendezvous.append(NativeRendezvous())

            # Select the native HTTP bootstrap only after all store ports are already occupied.
            bootstrap = None
            if len(self.instances) > 1:
                with NativeRendezvous.loopback_listener() as reservation:
                    host, port = reservation.getsockname()
                    bootstrap = {"host": host, "port": port}

            for index, instance in enumerate(self.instances):
                reader, writer = context.Pipe(duplex=False)
                process = context.Process(target=self.execute, args=(instance, barrier, clock, bootstrap, rendezvous[index].environment, writer))
                try:
                    process.start()
                except BaseException:
                    reader.close()
                    raise
                finally:
                    writer.close()
                processes.append(process)
                pending[reader] = index

            while pending:
                for connection in wait(pending):
                    index = pending[connection]
                    try:
                        success, result = connection.recv()
                    except EOFError as error:
                        raise RuntimeError(f"native capture instance {index} exited without reporting a result") from error
                    if not success:
                        raise RuntimeError(f"native capture instance {index} failed:\n{result}")
                    results[index] = result
                    del pending[connection]
                    connection.close()
            for process in processes:
                process.join()
                if process.exitcode != 0:
                    raise RuntimeError(f"native capture worker exited with {process.exitcode}")
            write_json(self.output / "capture.json", {
                "framework_revision": self.framework_revision, "instances": [results[index] for index in range(len(results))],
            }, indent=2, replace=False)
        finally:
            for connection in pending:
                connection.close()
            for process in processes:
                if process.exitcode != 0:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        if process.is_alive():
                            process.kill()
                process.join()
            rendezvous.clear()
