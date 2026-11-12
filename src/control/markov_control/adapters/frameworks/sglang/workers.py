"""Keep native worker returns independent and expose their control waits to the broker."""

import json
import os
from collections import deque
from contextlib import ExitStack
from dataclasses import dataclass
from multiprocessing import Pipe
from multiprocessing.connection import wait
from pathlib import Path
from tempfile import TemporaryDirectory

import torch
import torch.multiprocessing as multiprocessing
from markov_adapters.sglang.configuration import NativeConfiguration
from markov_control.adapters.runtimes.collectives import CollectiveCalls, CpuCollectives
from markov_control.adapters.runtimes.cpu import CpuDeclarations
from markov_control.adapters.runtimes.point_to_point import CpuPointToPoint, PointToPointCalls
from markov_control.peer import ControlPeer


class NativeStopped(Exception):
    """The core ended drained serving at a suspended native control boundary."""


class WorkerPeer(ControlPeer):
    def __init__(self, connection) -> None:
        self.connection = connection

    def exchange(self, operation, message=None):
        self.connection.send((operation, message))
        result = self.connection.recv()
        if isinstance(result, NativeStopped):
            raise result
        return result

    def send(self, message) -> None:
        self.exchange("send", message)

    def call(self, kind: str, **parameters):
        return self.exchange("call", {"kind": kind, **parameters})


class NativeWorkers:
    @classmethod
    def run(cls, peer: ControlPeer, kernel_python: Path) -> None:
        source, configuration = peer.receive(), peer.receive()
        if source["kind"] != "source" or configuration["kind"] != "configure":
            raise ValueError("Native control requires template metadata and core configuration first")
        runtime = {
            "framework_revision": NativeConfiguration.upstream_revision(os.environ.get("MARKOV_SGLANG_REVISION")),
            "torch": str(torch.__version__),
        }
        for instance in source["instances"]:
            if instance["runtime"] != runtime:
                raise ValueError(f"native control runtime {runtime} differs from source {instance['runtime']}")

        with TemporaryDirectory(prefix="markov-control-") as directory, ExitStack() as resources:
            configurations = {item["id"]: item for item in configuration["instances"]}
            workers, groups = [], []
            for instance in source["instances"]:
                configured = configurations[instance["id"]]
                settings = {
                    **configured["settings"], "observe": configuration["observe"], "clock_epoch_ns": configuration["clock_epoch_ns"],
                }

                model = Path(directory, f"instance-{len(workers)}")
                model.mkdir()

                # Each native world reads its own metadata and rendezvous; no model weights are loaded.
                (model / "config.json").write_text(json.dumps(instance["model_config"]))
                first = len(workers)
                for rank in range(instance["tp"] * instance["pp"]):
                    workers.append((instance, settings, str(model), rank))
                ranks = [worker["rank"] for worker in instance["workers"]]
                for group in configured["groups"]:
                    local = ranks.index(group["rank"])
                    groups.append(
                        {**group, "instance": instance["id"], "members": (first + local,), "ingress": local == 0}
                    )
            connections, children = [], []
            for _ in workers:
                parent, child = Pipe()
                connections.append(resources.enter_context(parent))
                children.append(resources.enter_context(child))
            context = multiprocessing.start_processes(
                cls.worker,
                args=(workers, configuration["workload"], kernel_python, children),
                nprocs=len(workers),
                start_method="spawn",
                join=False,
            )
            for child in children:
                child.close()
            try:
                NativeCalls(peer, connections, context, groups, observe=configuration["observe"]).run()
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
    def worker(index, workers, workload, kernel_python, connections) -> None:
        source, settings, directory, rank = workers[index]
        for position, connection in enumerate(connections):
            if position != index:
                connection.close()
        CpuDeclarations.install(kernel_python)

        from markov_control.adapters.frameworks.sglang.scheduler import NativeScheduler
        from markov_adapters.sglang.metadata_collectives import MetadataCollectives
        from markov_adapters.sglang.metadata_transfers import MetadataTransfers
        from sglang.srt.distributed import parallel_state
        from sglang.srt.plugins.hook_registry import HookRegistry

        peer = WorkerPeer(connections[index])
        try:
            scheduler = NativeScheduler(peer, settings, directory, source, rank, workload, workers, index)
            world = tuple(position for position, worker in enumerate(workers) if worker[0]["id"] == source["id"])
            CpuCollectives.install(peer, world)

            # Wrap the installed CPU transport, otherwise its replacement would discard the graph boundary hook.
            MetadataCollectives(scheduler.calls).install()
            if source["pp"] > 1:
                from markov_control.adapters.frameworks.sglang.transfers import PipelineTransfers

                PipelineTransfers(scheduler, world, [worker["rank"] for worker in source["workers"]]).install()
            else:
                CpuPointToPoint(peer, world, scheduler.calls.communicating).install()
            MetadataTransfers(scheduler.calls).install()

            # Apply once after every serving adapter registers; early application silently skips later hooks.
            HookRegistry.apply_hooks()
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
    deliveries: tuple = ()


class NativeCalls:
    """Keep worker calls suspended independently until their core response arrives."""

    def __init__(self, peer, connections, context, groups, *, observe=False) -> None:
        self.peer = peer
        self.connections = connections
        self.context = context
        self.membership = {rank: group for group in groups for rank in group["members"]}
        self.collectives = CollectiveCalls()
        self.transfers = PointToPointCalls()
        self.messages = {rank: [] for rank in range(len(connections))}
        self.closing = False
        self.stopped = set()
        self.pending = {}
        self.ready = deque()
        self.awaiting = {}
        self.sequence = 0
        self.polled = False
        self.initialized = False
        self.next_rank = 0
        if observe:
            self.ready.append(WorkerCall("observe", {
                "kind": "protocol", "group": self.membership[0]["id"],
                "record": {"phase": "scope", "backend": "gloo", "operations": ["send", "recv", "isend", "irecv"],
                           "participants": [{"rank": rank, "group": group["id"]} for rank, group in self.membership.items()]},
            }, ()))

    def receive_worker(self, channel, rank, operation, message) -> None:
        group = self.membership[rank]

        if self.closing and (operation != "send" or message.get("kind") != "stopped"):
            raise ValueError("Native worker requested work after drained shutdown")

        if operation == "messages":
            deliveries = tuple({"source": rank, **delivery} for delivery in message["deliveries"])
            if message["completion"] is not None:

                # Keep the caller free to run; only publication waits for the core's actual completion events.
                self.ready.append(WorkerCall(
                    "delivery", {**message["completion"], "kind": "completion_notice", "group": group["id"]}, (), deliveries,
                ))
            else:
                self.deliver(deliveries)
            channel.send(self.messages[rank])
            self.messages[rank] = []
            return
        if operation == "collective_post":
            self.collectives.post(channel, rank, message)
            return
        if operation == "collective_wait":
            self.collectives.wait(channel, rank, message)
            return
        if operation == "metadata_post":
            key = self.transfers.post(rank, message)
            descriptor = {name: value for name, value in message.items() if name not in {"values", "execution"}}
            message = {
                "kind": "cpu_post", "group": group["id"], "key": key, "descriptor": descriptor,
                "execution": message["execution"],
                "source": self.membership[message["source"]]["id"], "target": self.membership[message["target"]]["id"],
            }
            self.ready.append(WorkerCall("cpu_post", message, (channel,)))
            return
        if operation == "metadata_wait":
            self.ready.append(WorkerCall("cpu_wait", {
                "kind": "cpu_wait", "group": group["id"], "key": message["key"], "execution": message["execution"],
            }, (channel,)))
            return
        message = {**message, "group": group["id"]}
        if message["kind"] == "input" and not group["ingress"]:
            raise ValueError("Only the native input rank receives core arrivals")

        if message["kind"] != "ready":
            self.ready.append(WorkerCall(operation, message, (channel,)))
            return

        # Only startup waits for the world; native CPU collectives own all later rank agreement.
        members = tuple(self.connections)
        message = {"kind": "ready"}
        self.pending[channel] = (operation, message)
        if all(participant in self.pending for participant in members):
            for participant in members:
                other_operation, other = self.pending[participant]
                if (other_operation, other) != (operation, message):
                    fields = sorted(key for key in other.keys() | message.keys() if other.get(key) != message.get(key))
                    raise ValueError(
                        f"Native initialization disagrees on {other['kind']} / {message['kind']}: {fields}"
                    )
            self.ready.append(WorkerCall(operation, message, members))
            for participant in members:
                del self.pending[participant]

    def deliver(self, deliveries, completion=None):
        if not deliveries:
            return
        for packet in deliveries:
            self.messages[packet["target"]].append({"source": packet["source"], "value": packet["value"], "completion": completion})

        # Publish the wakeup after queuing metadata; a receiver must never miss that change while parking.
        source = self.membership[deliveries[0]["source"]]["id"]
        recipients = sorted({self.membership[packet["target"]]["id"] for packet in deliveries})
        self.ready.append(WorkerCall("notify", {"kind": "wake", "group": source, "groups": recipients}, ()))

    def receive_core(self) -> None:
        message = self.peer.receive()
        if message.get("kind") == "poll":
            if self.polled:
                raise ValueError("Core issued an overlapping native poll")
            self.polled = True
        elif message.get("kind") == "shutdown":
            self.shutdown()
        else:
            call = self.awaiting.pop(message["reply"])
            if call.operation == "delivery":
                self.deliver(call.deliveries, message["result"]["event"])
            elif call.operation == "cpu_post":
                for channel in call.participants:
                    channel.send(message["result"]["key"])
            elif call.operation == "cpu_wait":
                for channel in call.participants:
                    rank = self.connections.index(channel)
                    channel.send(self.transfers.consume(rank, message["result"]["key"]))
            else:
                for channel in call.participants:
                    channel.send(message["result"])

    def shutdown(self):
        if self.ready or self.pending or not self.quiescent() or any(
            call.message["kind"] not in {"input", "idle"}
            and not (call.message["kind"] == "cpu_wait" and not call.message["execution"]["in_flow"])
            for call in self.awaiting.values()
        ):
            raise ValueError("Native shutdown requires suspended participants and no unfinished serving calls")
        self.closing = True

        # The core drained serving and its execution graph; cancel private CPU waits without injecting PP messages.
        self.awaiting.clear()
        self.collectives.pending.clear()
        self.transfers.pending.clear()
        for channel in self.connections:
            if channel not in self.stopped:
                channel.send(NativeStopped())

    def quiescent(self) -> bool:
        blocked = self.stopped | set(self.pending) | self.collectives.blocked()
        calls = [*self.ready, *self.awaiting.values()]
        for call in calls:
            blocked.update(call.participants)

        return len(blocked) == len(self.connections)

    def dispatch(self) -> None:
        if not self.polled:
            return
        if self.ready:
            if not self.initialized:
                call = self.ready.popleft()
            else:

                # Read-only replies and observations must not consume a native scheduling turn.
                call = next((item for item in self.ready if item.message["kind"] in {
                    "capacity", "free_slots", "describe", "completed", "completion_notice", "observed_completion",
                    "wake", "read_result", "action_values", "protocol", "clock",
                } or (item.message["kind"] == "state_slots" and item.message["operation"] == "available")), None)
                if call is None:
                    if not self.quiescent():
                        return

                    # Resolve a stable logical turn after native actors reach their visible boundaries.
                    call = min(
                        self.ready,
                        key=lambda item: (
                            (self.connections.index(item.participants[0]) - self.next_rank) % len(self.connections)
                        ),
                    )
                    self.next_rank = (self.connections.index(call.participants[0]) + 1) % len(self.connections)
                self.ready.remove(call)
            identity = self.sequence
            self.sequence += 1
            self.peer.send({"kind": "call", "id": identity, "message": call.message})
            if call.operation in {"send", "notify"}:
                if call.message["kind"] == "ready":
                    self.initialized = True
                if call.message["kind"] == "stopped":
                    self.stopped.update(call.participants)
                for channel in call.participants:
                    channel.send(None)
            else:
                self.awaiting[identity] = call

        else:
            if not self.quiescent():
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
                        if self.awaiting or self.pending or self.ready or self.collectives.pending or self.transfers.pending:
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

        if self.awaiting or self.pending or self.ready or self.collectives.pending or self.transfers.pending:
            raise RuntimeError("Native workers exited with unfinished calls or metadata transport")
