"""Convert a complete native serving observation into control-boundary feedback."""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from collections.abc import Iterable
from pathlib import Path
from typing import Any

from markov_collection.adapters.frameworks.sglang.source import SGLangSource
from markov_collection.artifacts import write_json


class WorkerCompletions:
    cache_services = {
        "cache_match_prefix": "match",
        "cache_empty": "empty_match",
        "cache_insert": "insert",
        "cache_insert_helper": "insert_path",
        "cache_split_node": "split",
        "cache_inc_lock_ref": "acquire",
        "cache_dec_lock_ref": "release",
        "cache_cache_unfinished_req": "retain_request",
        "cache_cache_finished_req": "release_request",
        "cache_evict": "evict",
    }

    def __init__(self, source: dict, requests: dict[str, str]) -> None:
        SGLangSource.control_interfaces(source)
        self.source = source
        self.requests = requests
        environment = source["environment"]
        self.stage = environment["pp_rank"]
        self.role = environment["implementation"]["disaggregation_mode"]
        instance = "model" if self.role == "null" else self.role
        self.group = (
            instance if self.role != "null" and environment["logical_pp"] == 1 else f"{instance}.tp-{self.stage}"
        )
        self.regions = source["regions"]
        self.children: dict[int, list[int]] = defaultdict(list)
        self.batches: list[dict | None] = []
        self.request_contexts: list[dict | None] = []
        self.batch_scopes: dict[tuple[int, str], list[int]] = defaultdict(list)
        for region in self.regions:
            parent, metadata = region["parent"], region["metadata"]
            self.batches.append(
                source["batches"][metadata["batch"]]
                if "batch" in metadata
                else self.batches[parent] if parent is not None else None
            )
            self.request_contexts.append(
                metadata if "request" in metadata
                else self.request_contexts[parent] if parent is not None else None
            )
            if parent is not None:
                self.children[parent].append(region["id"])

            batch = self.batches[-1]
            if batch is not None and "entrypoint" in metadata:
                self.batch_scopes[batch["id"], metadata["entrypoint"]].append(region["id"])

    def related(self, region: dict, *entrypoints: str) -> list[int]:
        batch = self.batches[region["id"]]
        return [identity for entry in entrypoints for identity in self.batch_scopes[batch["id"], entry]]

    def ancestors(self, region: dict):
        parent = region["parent"]
        while parent is not None:
            region = self.regions[parent]
            yield region
            parent = region["parent"]

    def descendants(self, identity: int):
        for child in self.children[identity]:
            yield self.regions[child]
            yield from self.descendants(child)

    def members(self, batch: dict, selected: list[str] | None = None) -> list[dict]:
        members = batch["members"]
        if selected is not None:
            by_request = {member["id"]: member for member in members}
            members = [by_request[request] for request in selected]
        return [
            {
                "id": self.requests[member["id"]],
                **{name: member[name] for name in ("phase", "context_tokens", "new_tokens")},
            }
            for member in members
        ]

    def action(
        self, region: dict, step: str, *, completed: int | None = None,
        published: int | None = None, lane: int | None = None, related: Iterable[int] = (), **identity: Any
    ) -> dict:
        scopes = [region["id"], *related]
        for scope in scopes:
            if not self.regions[scope]["completed"]:
                raise ValueError(f"native scope {scope} did not complete")

        batch = self.batches[region["id"]]
        request = self.request_contexts[region["id"]]
        if lane is None:
            lane = request["lane"] if request is not None else batch["lane"]
        work = {"group": self.group, "step": step, "lane": lane, "stage": self.stage, **identity}
        return {
            "work": work,
            "completed": region["ended_ns"] if completed is None else completed,
            "published": published,
            "observations": [
                {"rank": self.source["environment"]["rank"], "region": scope} for scope in scopes
            ],
        }

    def selection_completion(self, region: dict) -> int:
        metadata = region["metadata"]
        if metadata["selection_view"] != "previous" or not metadata["requests"]:
            return region["ended_ns"]
        schedule = next(parent for parent in self.ancestors(region) if parent["role"] == "Schedule")
        slots = metadata["ports"]["outputs"]["request_slots"]
        merged = [
            child for child in self.descendants(schedule["id"])
            if child["metadata"].get("entrypoint") == "batch_merge"
            and child["metadata"]["ports"]["inputs"].get("right.request_slots") == slots
            and child["started_ns"] >= region["ended_ns"]
        ]

        # Previous-batch selection may include the following merge; connect it by the actual column value.
        if len(merged) > 1:
            raise ValueError(f"native selection {region['id']} feeds more than one batch merge")
        return merged[0]["ended_ns"] if merged else region["ended_ns"]

    def cache_action(self, region: dict, service: str) -> dict:
        metadata = region["metadata"]
        identity: dict[str, Any] = {"services": [service]}
        if service in ("match", "empty_match") and "requests" in metadata:
            identity["prefix_request"] = self.requests[metadata["requests"][0]]
        elif service == "retain_request":
            request = self.requests[metadata["requests"][0]]
            identity.update(binding={"request": request, "tokens": metadata["retained_tokens"]}, prefix_request=request)
        elif service == "release_request":
            identity["release_request"] = self.requests[metadata["requests"][0]]
        return self.action(region, "indices", **identity)

    def records(self) -> list[dict]:
        records = self.endpoint_events()
        for region in self.regions:
            if region["role"] == "KvPoll":
                metadata = region["metadata"]
                records.append(self.action(
                    region, "poll", lane=metadata["lane"],
                    requests=[self.requests[request] for request in metadata["requests"]],
                ))
                continue
            batch = self.batches[region["id"]]
            request = self.request_contexts[region["id"]]
            if (batch is not None and (batch["observation"] != "source" or batch.get("kind") == "primitive")) or (
                batch is None and (request is None or request["request"] not in self.requests)
            ):
                continue
            metadata = region["metadata"]
            entry = metadata.get("entrypoint")
            role = region["role"]
            ancestors = list(self.ancestors(region))

            # Nested cache calls belong to the outer native service, just as in IndexTrace.
            if entry in self.cache_services:
                if not any(parent["metadata"].get("entrypoint") in self.cache_services for parent in ancestors):
                    records.append(self.cache_action(region, self.cache_services[entry]))
            elif role in ("KvReserve", "KvAdvertise", "KvReceived", "KvSend", "KvReleased"):
                if role == "KvReleased" and self.role != "prefill" and any(
                    parent["role"] in ("ResultProcessed", "PrebuiltResult") for parent in ancestors
                ):
                    continue
                parameters = metadata["parameters"]
                identity: dict[str, Any] = {}
                completed = None
                phase, context, tokens = "prefill", 0, parameters["M"]
                if role == "KvSend":
                    submissions = [child for child in self.descendants(region["id"]) if child["role"] == "KvSubmit"]
                    if not submissions:
                        continue
                    [submitted] = submissions
                    if not submitted["completed"]:
                        raise ValueError("native KV submission marker did not complete")
                    completed = submitted["ended_ns"]
                    context = parameters["send_begin"]
                    identity["final_transfer"] = bool(parameters["last_chunk"])
                elif role == "KvReleased":
                    phase, context, tokens = "decode", parameters["resident_tokens"], 0
                member = {
                    "id": self.requests[metadata["request"]], "phase": phase,
                    "context_tokens": context, "new_tokens": tokens,
                }
                step = {
                    "KvReserve": "reserve", "KvAdvertise": "advertise", "KvReceived": "receive",
                    "KvSend": "send", "KvReleased": "release",
                }[role]
                records.append(self.action(region, step, completed=completed, members=[member], **identity))
            elif entry == "batch_preparation":
                members = self.members(batch, metadata["requests"])
                records.append(self.action(
                    region, "prepare_inputs", completed=metadata["preparation_inputs_finished_ns"], members=members
                ))

                # Preparation owns its allocator seam; allocations inside PD reservation stay in that operation.
                for child in self.descendants(region["id"]):
                    if child["metadata"].get("entrypoint") == "kv_allocate":
                        records.append(self.action(child, "allocate", members=members))
                records.append(self.action(region, "prepare_outputs", members=members))
            elif entry == "batch_filter" and "selection_view" in metadata:
                records.append(self.action(
                    region, "select", completed=self.selection_completion(region), view=metadata["selection_view"],
                    requests=[self.requests[request] for request in metadata["requests"]],
                ))
            elif entry == "batch_coordination":
                records.append(self.action(region, "schedule", operation="coordinate"))
            elif role in ("Prefill", "Decode", "Mixed"):
                records.append(self.action(region, "compute", members=self.members(batch)))
            elif role == "PipelineSend":
                channel = entry.removeprefix("pipeline_send_")
                identity = {"members": self.members(batch), "channel": channel}

                # Deferred waits retain the sender's batch and actual wire field, not the later active batch.
                waits = self.related(region, *(
                    f"pipeline_wait_{channel}_{port.removeprefix('wire.')}"
                    for port in metadata["ports"]["outputs"] if port.startswith("wire.")
                ))
                records.extend((
                    self.action(region, "send", **identity),
                    self.action(region, "wait", completed=metadata["wait_completed_ns"], related=waits, **identity),
                ))
            elif role == "PipelineReceive":
                records.append(self.action(
                    region, "receive", members=self.members(batch), channel=entry.removeprefix("pipeline_receive_")
                ))
            elif entry == "pipeline_ready_record":
                records.append(self.action(
                    region, "prepare_result", members=self.members(batch),
                    related=self.related(region, "pipeline_event_record", "pipeline_result_tokens"),
                ))
            elif entry == "result_tokens":
                records.append(self.action(
                    region, "read_result", members=self.members(batch),
                    related=self.related(region, "pipeline_ready_synchronize"),
                ))
            elif role in ("ResultProcessed", "PrebuiltResult"):
                publications = [child for child in self.descendants(region["id"]) if child["role"] == "RequestOutput"]
                if len(publications) > 1:
                    raise ValueError(f"result scope {region['id']} has multiple publication boundaries")
                if publications and not publications[0]["completed"]:
                    raise ValueError(f"result scope {region['id']} has an incomplete publication")
                published = publications[0]["ended_ns"] if publications else None

                # Native admission prepares the prebuilt batch before publishing its result in the same action.
                records.append(self.action(
                    region, "accept" if role == "PrebuiltResult" else "result", published=published,
                    members=self.members(batch),
                    related=self.related(region, "batch_accept") if role == "PrebuiltResult" else (),
                ))
            elif entry in ("request_cache_retain", "request_cache_release"):
                owners = {parent["role"] for parent in ancestors}
                if owners & {"PrebuiltResult", "BatchAccepted", "KvReleased"} or (
                    "ResultProcessed" in owners and self.role != "prefill"
                ):
                    continue
                retain = entry == "request_cache_retain"
                member = {
                    "id": self.requests[metadata["requests"][0]],
                    "phase": "prefill" if retain else "decode",
                    "context_tokens": metadata["parameters"]["resident_tokens"],
                    "new_tokens": 0,
                }
                records.append(self.action(region, "retain" if retain else "release", members=[member]))
            elif role == "Schedule":
                boundaries = metadata["control_boundaries"]
                if batch["members"]:
                    records.append(self.action(
                        region, "prepare", completed=boundaries["prepared_ns"], members=self.members(batch)
                    ))
                records.append(self.action(
                    region, "schedule", completed=boundaries["scheduled_ns"], operation="complete"
                ))
        return sorted(records, key=lambda row: row["completed"])

    def endpoint_events(self) -> list[dict]:
        records = []
        observed = {"KvReady": set(), "KvComplete": set()}
        for region in self.regions:
            metadata = region["metadata"]
            role = region["role"]
            if role not in observed or metadata["request"] not in self.requests:
                continue
            observed[role].add(metadata["request"])
            records.append(self.action(
                region, "handshake" if role == "KvReady" else "notify", group="kv", lane=0, stage=None,
                endpoint={"group": self.group, "request": self.requests[metadata["request"]]},
            ))
        if self.role == "prefill" and observed["KvReady"] != self.requests.keys():
            raise ValueError("native PD observation lacks per-request metadata readiness boundaries")
        if self.role != "null" and observed["KvComplete"] != self.requests.keys():
            raise ValueError("native PD observation lacks per-request endpoint success publications")
        return records

    def transfers(self) -> dict[str, list[dict]]:
        fragments: dict[tuple[int, str], list[dict]] = defaultdict(list)
        for region in self.regions:
            metadata = region["metadata"]
            if region["role"] in ("KvTransfer", "KvMetadataTransfer") and metadata["request"] in self.requests:
                fragments[metadata["send_region"], metadata["destination_session"]].append(region)

        routes: dict[str, list[dict]] = defaultdict(list)
        for (_, session), parts in fragments.items():
            metadata = parts[0]["metadata"]
            parameters = metadata["parameters"]
            final = bool(parameters["last_chunk"])
            expected = ["KvMetadataTransfer", "KvTransfer"] if final else ["KvTransfer"]
            if sorted(part["role"] for part in parts) != expected:
                raise ValueError("native KV fragment lacks a unique data/metadata completion pair")
            if any(not part["completed"] or part["metadata"]["status"] != 0 for part in parts):
                raise ValueError("native KV transfer did not complete successfully")
            member = {
                "id": self.requests[metadata["request"]], "phase": "prefill",
                "context_tokens": parameters["send_begin"], "new_tokens": parameters["M"],
            }

            # The final fragment completes after both KV and metadata; transport sessions identify actual rank pairs.
            record = self.action(
                parts[0], "transfer", completed=max(part["ended_ns"] for part in parts),
                group="kv", lane=0, stage=None, members=[member], final_transfer=final,
                transfer={"source": self.group}, related=[part["id"] for part in parts[1:]],
            )
            routes[session].append(record)
        return {route: sorted(rows, key=lambda row: row["completed"]) for route, rows in routes.items()}


class SGLangCompletionTimes:
    @staticmethod
    def group_records(workers: list[list[dict]]) -> list[dict]:

        # Stage-local TP participants share work; PP stages progress independently on the same observation clock.
        indexed = []
        for rows in workers:
            index: dict[str, list[dict]] = defaultdict(list)
            for row in rows:
                index[json.dumps(row["work"], sort_keys=True)].append(row)
            indexed.append(index)
        keys = indexed[0].keys()
        if any(index.keys() != keys for index in indexed[1:]):
            raise ValueError("native TP workers disagree on their observed control work")

        observations = []
        for key in keys:
            for participants in zip(*(index[key] for index in indexed), strict=True):
                publications = [row["published"] for row in participants if row["published"] is not None]
                if len(publications) > 1:
                    raise ValueError("native TP group has more than one publishing worker")
                observations.append({
                    "work": participants[0]["work"],
                    "completed": max(row["completed"] for row in participants),
                    "published": publications[0] if publications else None,
                    "observations": [origin for row in participants for origin in row["observations"]],
                })
        return observations

    @classmethod
    def read(cls, directory: Path) -> dict:
        pools = [("prefill", directory / "prefill"), ("decode", directory / "decode")] if (
            directory / "prefill"
        ).is_dir() else [("null", directory)]
        serving = json.loads((pools[0][1] / "session.json").read_text())["serving_input"]
        delivery = json.loads((directory / "request-delivery.json").read_text())["capture"]
        if len(delivery) != 1:
            raise ValueError("completion feedback requires one continuous serving workload, not discovery rounds")
        origin_ns = delivery[0]["origin_ns"]
        groups: dict[str, list[list[dict]]] = defaultdict(list)
        copies = []
        destinations = {}
        domains = []
        for role, pool in pools:
            session = json.loads((pool / "session.json").read_text())
            if "shard" in session:
                raise ValueError("completion feedback requires the real worker group, not isolated shard observations")
            if session["serving_input"] != serving or session["rank_offset"] != len(domains):
                raise ValueError("native instances disagree on their workload or rank binding")
            requests = dict(zip(
                session["source_requests"], (request["id"] for request in serving["requests"]), strict=True,
            ))
            paths = sorted(
                (path for path in pool.glob("rank-*.json") if path.stem.removeprefix("rank-").isdigit()),
                key=lambda path: int(path.stem.removeprefix("rank-")),
            )
            geometry = None
            for local_rank, path in enumerate(paths):
                source = json.loads(path.read_text())
                environment = source["environment"]
                if environment["observation_window"]["start"] != "prewarmed_idle_ingress":
                    raise ValueError("completion feedback requires the prewarmed native idle-ingress boundary")
                dimensions = environment["logical_tp"], environment["logical_pp"], environment["logical_ep"]
                if geometry is None:
                    geometry = dimensions
                tp, pp, _ = geometry
                if (
                    dimensions != geometry or tp < 1 or pp < 1 or len(paths) != tp * pp
                    or environment["rank"] != len(domains) or environment["tp_rank"] != local_rank % tp
                    or environment["pp_rank"] != local_rank // tp
                    or environment["implementation"]["disaggregation_mode"] != role
                ):
                    raise ValueError("completion observations must cover every real TP/PP participant in rank order")

                worker = WorkerCompletions(source, requests)
                try:
                    groups[worker.group].append(worker.records())
                    copies.append((environment["rank"], worker.group, worker.transfers()))
                except KeyError as error:
                    raise ValueError(f"{path}: missing native boundary fact {error}") from error
                if role == "decode":
                    transport = json.loads(path.with_name("transport-" + path.name).read_text())
                    if transport["rank"] != environment["rank"] or transport["session"] in destinations:
                        raise ValueError("native transport registration repeats or misidentifies its receiver")
                    destinations[transport["session"]] = environment["rank"], worker.group

                domains.append({
                    **{name: environment[name] for name in (
                        "framework", "torch", "torch_npu", "cann", "device", "rank", "tp_rank", "pp_rank", "ep_rank",
                        "logical_tp", "logical_pp", "logical_ep", "observation_window",
                    )},
                    "implementation": {
                        name: value for name, value in environment["implementation"].items()
                        if name != "chunked_prefill_size"
                    },
                })

                # Keep boundary records across ranks, not every worker's full raw graph.
                del source, worker
            if not paths:
                raise ValueError(f"native instance {pool} contains no worker observations")

        observations = [record for group in groups.values() for record in cls.group_records(group)]

        # Actual transport endpoints define participation; topology combinations do not get separate converters.
        transfers = defaultdict(dict)
        for rank, source_group, routes in copies:
            for session, rows in routes.items():
                target, group = destinations[session]
                for row in rows:
                    row["work"]["transfer"]["destination"] = group
                transfers[source_group, group][rank, target] = rows
        for routes in transfers.values():
            observations.extend(cls.group_records(list(routes.values())))
        if not observations:
            raise ValueError("native observation contains no serving completion boundaries")

        events = []
        for row in observations:
            for kind in ("published", "completed"):
                if row[kind] is not None:
                    events.append((row[kind], kind, row))
        events.sort(key=lambda item: (item[0], item[1] == "completed"))
        for order, (time_ns, kind, row) in enumerate(events):
            if time_ns < origin_ns:
                raise ValueError("observed control work precedes the recorded workload origin")
            row[kind] = {"time_ns": time_ns - origin_ns, "order": order}
        return {
            "source": str(directory.resolve()),
            "domains": domains,
            "input": {
                name: serving[name]
                for name in ("configuration", "requests", "prefixes", "ep_routes") if name in serving
            },
            "actions": observations,
        }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    directory, output = arguments.capture.resolve(strict=True), arguments.output.resolve()
    if output.is_relative_to(directory):
        parser.error("completion feedback must be written outside its immutable native observation")
    write_json(output, SGLangCompletionTimes.read(directory), indent=2)


if __name__ == "__main__":
    main()
