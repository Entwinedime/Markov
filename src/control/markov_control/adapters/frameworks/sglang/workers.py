"""Run native TP participants while applying each shared resource action once."""

import json
from collections import deque
from contextlib import ExitStack
from dataclasses import dataclass
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
                NativeCalls(peer, connections, context).run()
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


@dataclass
class WorkerCall:
    operation: str
    message: dict
    participants: tuple


class NativeCalls:
    """Keep worker calls suspended independently until their core response arrives."""

    def __init__(self, peer, connections, context) -> None:
        self.peer = peer
        self.connections = connections
        self.context = context
        self.pending = {}
        self.ready = deque()
        self.awaiting = {}
        self.sequence = 0
        self.polled = False

    def receive_worker(self, channel, rank, operation, message) -> None:
        if operation == "receive":
            if rank != 0:
                raise ValueError("Only the native input rank receives core arrivals")
            self.ready.append(WorkerCall(operation, {"kind": "input"}, (channel,)))
        elif message["kind"] in ("capacity", "free_slots"):

            # Rank-local diagnostics are reads, not collective control decisions.
            self.ready.append(WorkerCall(operation, message, (channel,)))
        else:
            self.pending[channel] = (operation, message)
            if len(self.pending) == len(self.connections):
                if any(item != (operation, message) for item in self.pending.values()):
                    raise ValueError(f"Native TP workers disagree on a core action: {list(self.pending.values())}")
                self.ready.append(WorkerCall(operation, message, tuple(self.pending)))
                self.pending.clear()

    def receive_core(self) -> None:
        message = self.peer.receive()
        if message.get("kind") == "poll":
            if self.polled:
                raise ValueError("Core issued an overlapping native poll")
            self.polled = True
        else:
            call = self.awaiting.pop(message["reply"])
            for channel in call.participants:
                channel.send(message["result"])

    def dispatch(self) -> None:
        if not self.polled:
            return
        if self.ready:
            call = self.ready.popleft()
            identity = self.sequence
            self.sequence += 1
            self.peer.send({"kind": "call", "id": identity, "message": call.message})
            if call.operation == "send":
                for channel in call.participants:
                    channel.send(None)
            else:
                self.awaiting[identity] = call
        else:
            blocked = set(self.pending)
            for call in self.awaiting.values():
                blocked.update(call.participants)

            # At native input, the other TP ranks wait inside the native request broadcast.
            input_wait = any(call.operation == "receive" for call in self.awaiting.values())
            if not input_wait and len(blocked) != len(self.connections):
                return
            self.peer.send({"kind": "waiting"})
        self.polled = False

    def run(self) -> None:
        channels = {connection: rank for rank, connection in enumerate(self.connections)}
        live = {process.sentinel for process in self.context.processes}
        core_open = True
        while live:
            readers = [*channels, *live]
            if core_open:
                readers.append(self.peer.reader)
            readable = wait(readers, timeout=0 if core_open and self.peer.buffered() else None)
            if core_open and self.peer.buffered() and self.peer.reader not in readable:
                readable.append(self.peer.reader)
            for channel in readable:
                if channel in live:
                    live.remove(channel)
                    self.context.join(timeout=0)
                elif channel is self.peer.reader:
                    try:
                        self.receive_core()
                    except EOFError:
                        if self.awaiting or self.pending or self.ready:
                            raise
                        core_open = False
                        self.polled = False
                else:
                    try:
                        request = channel.recv()
                    except EOFError:
                        del channels[channel]
                        continue
                    if not core_open:
                        raise EOFError("Native worker requested work after core exit")
                    self.receive_worker(channel, channels[channel], *request)
            self.dispatch()
