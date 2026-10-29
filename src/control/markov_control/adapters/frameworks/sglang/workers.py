"""Run independent native groups while applying each group's resource action once."""

import json
from collections import defaultdict, deque
from contextlib import ExitStack
from dataclasses import dataclass
from multiprocessing import Pipe
from multiprocessing.connection import wait
from pathlib import Path
from tempfile import TemporaryDirectory

import torch.multiprocessing as multiprocessing
from markov_control.adapters.runtimes.cpu import CpuDeclarations
from markov_control.peer import ControlPeer


class NativeStopped(Exception):
    """The core ended serving at a native request boundary."""


@dataclass
class StopInput:
    pass


class WorkerPeer(ControlPeer):
    def __init__(self, connection) -> None:
        self.connection = connection
        self.context = {}

    def exchange(self, operation, message=None):
        self.connection.send((operation, message))
        return self.connection.recv()

    def send(self, message) -> None:
        self.exchange("send", {**self.context, **message})

    def receive(self):
        return self.exchange("receive")

    def call(self, kind: str, **parameters):
        return self.exchange("call", {**self.context, "kind": kind, **parameters})


class NativeWorkers:
    @classmethod
    def run(cls, peer: ControlPeer, kernel_python: Path) -> None:
        source, configuration = peer.receive(), peer.receive()
        if source["kind"] != "source" or configuration["kind"] != "configure":
            raise ValueError("Native control requires template metadata and core configuration first")

        with TemporaryDirectory(prefix="markov-control-") as directory, ExitStack() as resources:
            configurations = {item["id"]: item for item in configuration["instances"]}
            workers, groups = [], []
            for instance in source["instances"]:
                configured = configurations[instance["id"]]
                model = Path(directory, instance["id"])
                model.mkdir()

                # Each native world reads its own metadata and rendezvous; no model weights are loaded.
                (model / "config.json").write_text(json.dumps(instance["model_config"]))
                first = len(workers)
                for rank in range(instance["tp"] * instance["pp"]):
                    workers.append((instance, configured["settings"], str(model), rank))
                for group in configured["groups"]:
                    begin = first + group["stage"] * instance["tp"]
                    groups.append({**group, "instance": instance["id"],
                                   "members": tuple(range(begin, begin + instance["tp"]))})
            connections, children = [], []
            for _ in workers:
                parent, child = Pipe()
                connections.append(resources.enter_context(parent))
                children.append(resources.enter_context(child))
            context = multiprocessing.start_processes(
                cls.worker,
                args=(workers, kernel_python, children),
                nprocs=len(workers),
                start_method="spawn",
                join=False,
            )
            for child in children:
                child.close()
            try:
                NativeCalls(peer, connections, context, groups).run()
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
    def worker(index, workers, kernel_python, connections) -> None:
        source, settings, directory, rank = workers[index]
        for position, connection in enumerate(connections):
            if position != index:
                connection.close()
        CpuDeclarations.install(kernel_python)

        from markov_control.adapters.frameworks.sglang.scheduler import NativeScheduler
        from sglang.srt.distributed import parallel_state

        peer = WorkerPeer(connections[index])
        try:
            from markov_control.adapters.frameworks.sglang.pipeline import PipelineScheduler

            if source["mode"] != "null":
                from markov_control.adapters.frameworks.sglang.disaggregated import (
                    DisaggregatedScheduler,
                    PipelineDisaggregatedScheduler,
                )

                implementation = PipelineDisaggregatedScheduler if source["pp"] > 1 else DisaggregatedScheduler
            else:
                implementation = PipelineScheduler if source["pp"] > 1 else NativeScheduler
            scheduler = implementation(peer, settings, directory, source, rank)
            peer.send({"kind": "ready"})
            try:
                scheduler.run_event_loop()
            except NativeStopped:
                pass
            peer.send({"kind": "stopped"})
        finally:
            parallel_state.destroy_model_parallel()
            parallel_state.destroy_distributed_environment()
            connections[index].close()


@dataclass
class WorkerCall:
    operation: str
    message: dict
    participants: tuple


class NativeCalls:
    """Keep worker calls suspended independently until their core response arrives."""

    def __init__(self, peer, connections, context, groups) -> None:
        self.peer = peer
        self.connections = connections
        self.context = context
        self.groups = groups
        self.membership = {rank: group for group in groups for rank in group["members"]}
        self.packets = defaultdict(deque)
        self.receivers = {}
        self.closing = False
        self.stopped = set()
        self.pending = {}
        self.ready = deque()
        self.awaiting = {}
        self.sequence = 0
        self.polled = False

    def receive_worker(self, channel, rank, operation, message) -> None:
        group = self.membership[rank]
        members = tuple(self.connections[index] for index in group["members"])
        if operation in ("send_object", "receive_object"):
            key = (group["instance"], message["source"], message["target"], message["channel"])
            if operation == "send_object":
                if key in self.receivers:
                    self.receivers.pop(key).send(message["value"])
                else:
                    self.packets[key].append(message["value"])
                channel.send(None)
            elif self.closing and message["channel"] == "requests":
                channel.send([StopInput()])
            elif self.packets[key]:
                channel.send(self.packets[key].popleft())
            else:
                self.receivers[key] = channel
            return
        if operation == "receive":
            if group["stage"] != 0 or rank != group["members"][0]:
                raise ValueError("Only the native input rank receives core arrivals")
            if self.closing:
                channel.send({"kind": "stop"})
            else:
                self.ready.append(WorkerCall(operation, {"kind": "input", "group": group["id"]}, (channel,)))
            return
        message = {**message, "group": group["id"]}
        if message["kind"] in ("capacity", "free_slots"):
            self.ready.append(WorkerCall(operation, message, (channel,)))
            return
        if message["kind"] == "ready":

            # Initialization completes across the world before any participant starts its native loop.
            members = tuple(self.connections)
            message = {"kind": "ready"}
        self.pending[channel] = (operation, message)
        if all(participant in self.pending for participant in members):
            if any(self.pending[participant] != (operation, message) for participant in members):
                raise ValueError("Native TP workers disagree on a core action")
            self.ready.append(WorkerCall(operation, message, members))
            for participant in members:
                del self.pending[participant]

    def receive_core(self) -> None:
        message = self.peer.receive()
        if message.get("kind") == "poll":
            if self.polled:
                raise ValueError("Core issued an overlapping native poll")
            self.polled = True
        elif message.get("kind") == "shutdown":
            self.closing = True
            for identity, call in list(self.awaiting.items()):
                if call.operation == "receive":
                    call.participants[0].send({"kind": "stop"})
                    del self.awaiting[identity]
            for key, channel in list(self.receivers.items()):
                if key[3] == "requests":
                    channel.send([StopInput()])
                    del self.receivers[key]
        else:
            call = self.awaiting.pop(message["reply"])
            for channel in call.participants:
                channel.send(message["result"])

    def dispatch(self) -> None:
        if not self.polled:
            return
        if self.ready:
            call = self.ready.popleft()
            if self.closing and call.operation == "receive":
                call.participants[0].send({"kind": "stop"})
                return
            identity = self.sequence
            self.sequence += 1
            self.peer.send({"kind": "call", "id": identity, "message": call.message})
            if call.operation == "send":
                if call.message["kind"] == "stopped":
                    self.stopped.update(call.participants)
                for channel in call.participants:
                    channel.send(None)
            else:
                self.awaiting[identity] = call
        else:
            blocked = self.stopped | set(self.pending)
            for call in self.awaiting.values():
                blocked.update(call.participants)

            blocked.update(self.receivers.values())
            input_waiters = {call.participants[0] for call in self.awaiting.values() if call.operation == "receive"}
            input_waiters.update(channel for key, channel in self.receivers.items() if key[3] == "requests")

            # Input rank waits represent only that stage's native TP broadcast, never all PP participants.
            for group in self.groups:
                members = [self.connections[index] for index in group["members"]]
                if members[0] in input_waiters:
                    blocked.update(members)
            if len(blocked) != len(self.connections):
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
