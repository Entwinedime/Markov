"""Run native TP participants while applying each shared resource action once."""

import json
from contextlib import ExitStack
from multiprocessing import Pipe
from multiprocessing.connection import wait
from pathlib import Path
from tempfile import TemporaryDirectory

import torch.multiprocessing as multiprocessing
from markov_control.adapters.runtimes.cpu import CpuDeclarations
from markov_control.peer import ControlPeer


class WorkerPeer(ControlPeer):
    def __init__(self, connection) -> None:
        self.connection = connection

    def exchange(self, operation, message=None):
        self.connection.send((operation, message))
        return self.connection.recv()

    def send(self, message) -> None:
        self.exchange("send", message)

    def receive(self):
        return self.exchange("receive")

    def call(self, kind: str, **parameters):
        return self.exchange("call", {"kind": kind, **parameters})


class NativeWorkers:
    @classmethod
    def run(cls, peer: ControlPeer, kernel_python: Path) -> None:
        source, configuration = peer.receive(), peer.receive()
        if source["kind"] != "source" or configuration["kind"] != "configure":
            raise ValueError("Native control requires template metadata and core configuration first")

        with TemporaryDirectory(prefix="markov-control-") as directory, ExitStack() as resources:

            # Prediction consumes metadata; weights and device work stay in the simulated graph.
            Path(directory, "config.json").write_text(json.dumps(source["model_config"]))
            connections, children = [], []
            for _ in range(source["tp"]):
                parent, child = Pipe()
                connections.append(resources.enter_context(parent))
                children.append(resources.enter_context(child))
            context = multiprocessing.start_processes(
                cls.worker,
                args=(source, configuration["settings"], directory, kernel_python, children),
                nprocs=source["tp"],
                start_method="spawn",
                join=False,
            )
            for child in children:
                child.close()
            try:
                cls.coordinate(peer, connections, context)
            finally:
                for process in context.processes:
                    if process.is_alive():
                        process.terminate()
                for process in context.processes:
                    process.join(timeout=5)
                    if process.is_alive():
                        process.kill()
                        process.join()

    @staticmethod
    def coordinate(peer, connections, context) -> None:
        channels = {connection: rank for rank, connection in enumerate(connections)}
        live = {process.sentinel for process in context.processes}
        pending = {}
        while live:
            for channel in wait([*channels, *live]):
                if channel in live:
                    live.remove(channel)
                    context.join(timeout=0)
                    continue
                try:
                    operation, message = channel.recv()
                except EOFError:
                    del channels[channel]
                    continue

                if operation == "receive":
                    if channels[channel] != 0:
                        raise ValueError("Only the native input rank receives core arrivals")
                    channel.send(peer.receive())
                elif message["kind"] in ("capacity", "free_slots"):

                    # Native diagnostics can query on one rank only; reads never need group agreement.
                    channel.send(peer.call(message["kind"]))
                else:
                    pending[channel] = (operation, message)
                    if len(pending) != len(connections):
                        continue
                    if any(item != (operation, message) for item in pending.values()):
                        raise ValueError(f"Native TP workers disagree on a core action: {list(pending.values())}")

                    # One agreed action owns the logical allocation; each rank keeps its native tensor view.
                    peer.send(message)
                    response = peer.receive() if operation == "call" else None
                    for participant in pending:
                        participant.send(response)
                    pending.clear()

    @staticmethod
    def worker(rank, source, settings, directory, kernel_python, connections) -> None:
        for index, connection in enumerate(connections):
            if index != rank:
                connection.close()
        CpuDeclarations.install(kernel_python)

        from markov_control.adapters.frameworks.sglang.scheduler import NativeScheduler
        from sglang.srt.distributed import parallel_state

        peer = WorkerPeer(connections[rank])
        try:
            scheduler = NativeScheduler(peer, settings, directory, source, rank)
            peer.send({"kind": "ready"})
            scheduler.run_event_loop()
        finally:
            parallel_state.destroy_model_parallel()
            parallel_state.destroy_distributed_environment()
            connections[rank].close()
