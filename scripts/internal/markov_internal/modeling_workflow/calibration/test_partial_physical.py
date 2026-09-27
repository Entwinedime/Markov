"""Partial stage measurement must not run unrelated storage or DMA experiments."""

import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest.mock import patch

from ...common.io import load_json, write_json
from ...common.paths import ROOT_DIR
from .. import io_calibration, runtime_dma_calibration
from .. import physical_capture
from ..group import PhysicalBudget
from . import bundle
from . import host_storage, prefetch_service
from . import runtime_anchors
from .options import parse_args


class PartialPhysicalTests(unittest.TestCase):
    def test_shared_batch_keeps_completed_services_when_later_capture_stops(self):
        from ..group import GroupRequest

        initial = SimpleNamespace(
            path=Path("group.json"),
            raw={"physical_capture": {}},
            physical={},
            physical_budget=PhysicalBudget(100, 3, 100),
        )
        initial.raw["physical_capture"] = {"page_token_sizes": [64]}
        updated = SimpleNamespace(path=initial.path, physical={"load": "measured"})
        requested = {
            "physical/load",
            "physical/write_device_to_host",
            "physical/prefetch_stages",
            "physical/write_host_to_storage_existing",
        }
        for stopped in (False, True):
            attempts = []

            def capture(group, service, **kwargs):
                if service == "dma":
                    self.assertEqual(kwargs["directions"], ("host_to_device", "device_to_host"))
                else:
                    self.assertEqual(group.physical, {"load": "measured"})
                size = 1000 if stopped and service == "prefetch" else 20
                _, limits = initial.physical_budget.available_for(
                    physical_capture.physical_usage(attempts), container_starts=1, logical_io_bytes=size
                )
                if not limits:
                    attempts.append({"wall_seconds": 1, "reserved_logical_io_bytes": size})
                return {
                    "status": "physical_budget_exhausted" if limits else "physical_captured",
                    "physical_stages": [{"stage": service}],
                }

            with (
                self.subTest(stopped=stopped),
                patch.object(physical_capture, "physical_attempts", return_value=attempts),
                patch.object(physical_capture, "_capture_physical_service", side_effect=capture),
                patch.object(GroupRequest, "load", return_value=updated),
            ):
                result = physical_capture.capture_physical(initial, dry_run=False, required_components=requested)

            expected = requested - {"physical/prefetch_stages"} if stopped else requested
            self.assertEqual(set(result["completed_components"]), expected)
            self.assertEqual(result["status"], "physical_budget_exhausted" if stopped else "physical_captured")
            self.assertEqual(result["physical_capture_usage"]["logical_io_bytes"], 40 if stopped else 60)
            self.assertEqual(
                result["physical_stages"],
                [{"stage": "dma"}, {"stage": "prefetch"}, {"stage": "existing_write"}],
            )

    def test_cpu_sampling_changes_do_not_discard_completed_io(self):
        from .. import group as group_module

        with TemporaryDirectory() as directory:
            root = Path(directory)
            report = root / "report.json"
            write_json(report, {"measurement_sources": ["independent.json"]})
            sampling = {"page_token_sizes": [128], "cpu_sets": "0|1", "eviction_cpu": {"heap_sizes": [1, 8]}}
            group = SimpleNamespace(raw={"physical_capture": sampling}, sources=[SimpleNamespace()], output_dir=root)
            with patch.object(group_module, "source_environment", return_value={"model": "base"}):
                context = physical_capture.physical_context(group)
                # Existing records can contain CPU sampling settings. They are
                # irrelevant to I/O applicability, but placement remains relevant.
                context["sampling"] = dict(sampling)
                row = dict(
                    stage="new_write", status="completed", report=str(report), definition={"input_context": context}
                )
                with patch.object(physical_capture, "physical_attempts", return_value=[row]):
                    for key, value in (("eviction_cpu", {"heap_sizes": [1, 64]}), ("budget", {"wall_seconds": 200})):
                        sampling[key] = value
                        self.assertEqual(physical_capture.captured_physical_declaration(group)["report"], str(report))
                    sampling["cpu_sets"] = "2|3"
                    self.assertIsNone(physical_capture.captured_physical_declaration(group))
                    del sampling["page_token_sizes"]
                    self.assertIsNone(physical_capture.captured_physical_declaration(group))

    def test_prefetch_plan_needs_no_token_or_bulk_transfer_inputs(self):
        from .. import group as group_module

        with TemporaryDirectory(dir=ROOT_DIR) as directory:
            root = Path(directory)
            write_json(root / "config.json", {})
            geometry = dict(
                kv_bytes_per_token_per_rank=4,
                kv_element_bytes=2,
                model_config_path="model/config.json",
                tensor_parallel_size=2,
            )
            group = SimpleNamespace(
                raw={"physical_capture": {"page_token_sizes": [32, 64], "payload_bytes": 1}},
                sources=[SimpleNamespace(run_dir=root, config_path=root / "config.json")],
                output_dir=root,
            )
            # No workload_ids/token_plan or observation assets: prefetch work
            # comes entirely from the declared platform grid.
            flags = dict(
                hicache_io_backend="kernel_ascend",
                hicache_mem_layout="page_first_direct",
                hicache_storage_backend="file",
                tp_size="2",
                model_path="model",
            )
            with (
                patch.object(physical_capture, "parse_server_command_flags", return_value=flags),
                patch.object(physical_capture, "derive_kv_geometry", return_value=geometry),
                patch.object(physical_capture, "deployment_cpu_sets", return_value=["0", "1"]),
                patch.object(physical_capture, "storage_batch_pages", return_value=128),
                patch.object(group_module, "source_environment", return_value={"model": "base"}),
            ):
                definition = physical_capture.physical_definition(group, service="prefetch")
                self.assertEqual(physical_capture.prepare_platform(group)["status"], "platform_ready")
                metadata = physical_capture.platform_inputs(group)
                self.assertEqual(metadata["service_models"], {})
                self.assertEqual(metadata["measurement_sources"], [])
                self.assertEqual(metadata["kv_geometry"], geometry)
                group.raw["physical_capture"]["cpu_sets"] = "2|3"
                self.assertEqual(physical_capture.platform_inputs(group), metadata)
                del group.raw["physical_capture"]["cpu_sets"]
                sampling = group.raw["physical_capture"]
                group.raw["physical_capture"] = {}
                self.assertEqual(physical_capture.prepare_platform(group)["status"], "platform_ready")
                self.assertEqual(physical_capture.platform_inputs(group), metadata)
                group.raw.pop("physical_capture")
                self.assertEqual(physical_capture.prepare_platform(group)["status"], "platform_ready")
                self.assertEqual(physical_capture.platform_inputs(group), metadata)
                group.raw["physical_capture"] = {}
                with self.assertRaisesRegex(ValueError, "page_token_sizes"):
                    physical_capture.physical_definition(group, service="prefetch")
                group.raw["physical_capture"]["devices"] = [2, 3]
                self.assertIsNone(physical_capture.platform_inputs(group))
                group.raw["physical_capture"] = sampling
                with patch.object(physical_capture, "derive_kv_geometry", side_effect=FileNotFoundError):
                    unavailable = physical_capture.prepare_platform(group)
                    self.assertEqual(unavailable["status"], "needs_physical_calibration")
                    self.assertIn("Model metadata unavailable", unavailable["limitations"][0])
                # Base-derived costs need geometry, not permission to launch
                # an independent measurement. Only sampling needs placement.
                with patch.object(physical_capture, "deployment_cpu_sets", return_value=None) as placement:
                    self.assertEqual(physical_capture.prepare_platform(group)["status"], "platform_ready")
                    placement.assert_not_called()
                    blocked = physical_capture.physical_definition(group, service="prefetch")
                    self.assertIn("CPU placement is unresolved", blocked["limitations"][0])
                write_json(root / "config.json", {"env": {"SGLANG_NUMA_BIND_V2": "1"}})
                self.assertEqual(physical_capture.prepare_platform(group)["status"], "platform_ready")
                blocked = physical_capture.physical_definition(group, service="prefetch")
                self.assertIn("numactl membind", blocked["limitations"][0])
            self.assertEqual(set(definition["logical_io_bytes"]), {"prefetch"})
            self.assertNotIn("payload_bytes", definition)
            self.assertEqual(definition["peak_storage_payload_bytes"], 2 * 128 * 129)
            definition["base_report"] = "base.json"
            command = physical_capture.physical_command(definition, "prefetch", Path("capture"))
            self.assertIn("--base-report", command)
            self.assertFalse(
                {
                    "--storage-existing-operation-pages",
                    "--storage-new-write-queue-bytes-per-scope",
                }
                & set(command)
            )
            report = root / "result" / "calibration_report.json"
            write_json(report, {})
            row = dict(
                stage="prefetch",
                status="completed",
                report=str(report),
                definition=dict(definition),
                command=physical_capture.physical_command(definition, "prefetch", root),
            )
            # Another service has extended the shared report. This must not
            # launch the identical prefetch experiment again, even with no budget.
            definition["base_report"] = str(root / "with_dma.json")
            write_json(root / "with_dma.json", {"measurement_sources": [str(report)]})
            self.assertIs(physical_capture._completed([row], definition, "prefetch"), row)
            self.assertEqual(definition["base_report"], str(root / "with_dma.json"))
            self.assertIsNone(physical_capture._completed([row], {**definition, "repeats": 9}, "prefetch"))
            write_json(root / "with_dma.json", {"measurement_sources": []})
            self.assertIsNone(physical_capture._completed([row], definition, "prefetch"))

            definition.update(new_operation_bytes=[1024, 2048], queue_bytes=4096)
            command = physical_capture.physical_command(definition, "new_write", Path("capture"))
            self.assertEqual(command[command.index("--service") + 1], "new_write")
            self.assertNotIn("--storage-existing-operation-pages", command)

    def test_no_demand_does_not_require_sampling_environment(self):
        group = SimpleNamespace(raw={}, physical_budget=None, physical=None)
        attempts = [dict(wall_seconds=12, reserved_logical_io_bytes=100, status="failed")]
        with (
            patch.object(physical_capture, "physical_attempts", return_value=attempts),
            patch.object(physical_capture, "physical_definition") as definition,
            patch.object(physical_capture, "run_container_attempt") as launch,
        ):
            for dry in (True, False):
                result = physical_capture.capture_physical(group, dry_run=dry, required_components=set())
                self.assertEqual(result["status"], "physical_captured")
                self.assertEqual(result["physical_stages"], [])
                self.assertEqual(result["physical_capture_usage"]["container_starts"], 1)
            missing = {"physical/load"}
            for raw in ({}, {"physical_capture": {}}, {"physical_capture": {"eviction_cpu": {"heap_sizes": [8]}}}):
                group.raw = raw
                result = physical_capture.capture_physical(group, dry_run=False, required_components=missing)
                self.assertEqual(result["status"], "needs_physical_calibration")
                self.assertEqual(result["stop_reason"], "physical_capture_declaration_required")
                self.assertIn("physical_capture.page_token_sizes", result["requirements"][0])
            definition.assert_not_called()
            launch.assert_not_called()

    def test_dma_reuse_depends_on_its_command_not_storage_requirements(self):
        from copy import deepcopy

        with TemporaryDirectory(dir=ROOT_DIR) as directory:
            report = Path(directory) / "result/dma.json"
            write_json(report, {})
            definition = dict(
                input_context={"environment": {"model": "base"}},
                devices=[0, 1],
                cpu_sets=["0", "1"],
                numa_nodes=[0, 1],
                page_token_sizes=[128],
                warmup=1,
                repeats=2,
                payload_bytes=4096,
                geometry=dict(
                    num_hidden_layers=2,
                    num_key_value_heads_per_rank=2,
                    head_dim=8,
                    kv_element_bytes=2,
                    kv_bytes_per_token_per_rank=128,
                    tensor_parallel_size=2,
                ),
                base_report=str(Path(directory) / "platform_inputs.json"),
                existing_operation_pages=[1, 8],
                directions=("host_to_device",),
            )
            row = dict(
                stage="dma",
                status="completed",
                definition=definition,
                report=str(report),
                command=physical_capture.physical_command(definition, "dma", Path(directory)),
            )
            write_json(
                Path(definition["base_report"]),
                dict(
                    kv_geometry=definition["geometry"],
                    measurement_scope=dict(storage_scope_devices=[0, 1], storage_scope_numa_nodes=[0, 1]),
                    target_workload_trace_used=False,
                    target_e2e_used=False,
                ),
            )
            plan = runtime_dma_calibration.parse_args(row["command"][3:])
            self.assertEqual((plan.devices, plan.numa_nodes), ([0, 1], [0, 1]))
            self.assertEqual((plan.layer_count, plan.kv_heads_per_rank, plan.head_dim), (2, 2, 8))
            self.assertEqual(plan.directions, ("host_to_device",))
            # CLI refactoring does not invalidate a measured sampling domain.
            row["command"] = ["historical command spelling"]
            changed = deepcopy(definition)
            changed["existing_operation_pages"] = [1, 16]
            with patch.object(physical_capture, "require_repo_path", side_effect=Path):
                self.assertIs(physical_capture._completed([row], changed, "dma"), row)
                for key, value in [
                    ("devices", [2, 3]),
                    ("cpu_sets", ["2", "3"]),
                    ("payload_bytes", 8192),
                    ("page_token_sizes", [64]),
                    ("repeats", 3),
                    ("numa_nodes", [1, 0]),
                    ("directions", ("device_to_host",)),
                    ("input_context", {"environment": {"model": "another"}}),
                ]:
                    self.assertIsNone(physical_capture._completed([row], {**changed, key: value}, "dma"))
                self.assertIsNone(physical_capture._completed([{**row, "status": "failed"}], changed, "dma"))
                report.unlink()
                self.assertIsNone(physical_capture._completed([row], changed, "dma"))

    def test_fit_failure_preserves_raw_measurements_without_publishing_report(self):
        capture = dict(
            service_models={},
            target_workload_trace_used=False,
            target_e2e_used=False,
            storage_batch_pages=128,
            prefetch_service_observations=[{"raw": "measured"}],
            kv_geometry={"kv_bytes_per_token_per_rank": 4},
        )
        with TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            with (
                patch.object(bundle, "build_prefetch_service_model", side_effect=ValueError("unidentified parameters")),
                patch.object(bundle, "repo_relative_path", side_effect=Path),
                self.assertRaisesRegex(ValueError, "unidentified"),
            ):
                bundle.complete_service_bundle(
                    capture,
                    Path(directory) / "base.json",
                    {"prefetch_service_observations": capture["prefetch_service_observations"]},
                    output,
                    12.0,
                    service="prefetch",
                )
            self.assertEqual(
                load_json(output / "physical_observations.json")["prefetch_service_observations"],
                capture["prefetch_service_observations"],
            )
            self.assertFalse((output / "calibration_report.json").exists())

    def test_only_requested_service_is_measured_and_workers_close_on_failure(self):
        with TemporaryDirectory() as directory:
            for service in ("existing_write", "new_write", "prefetch"):
                plan = SimpleNamespace(service=service, page_sizes=(128,), warmup=1, repeats=2)
                path = Path(directory) / f"{service}.json"
                with (
                    self.subTest(service=service),
                    patch.object(host_storage, "StorageScopeProcesses") as workers,
                    patch.object(host_storage, "calibrate_existing_key_curves", return_value=[{"old": 1}]) as existing,
                    patch.object(host_storage, "calibrate_new_write_only", return_value=[{"new": 1}]) as write,
                    patch.object(prefetch_service, "capture_prefetch_timings", return_value=[{"read": 1}]) as prefetch,
                ):
                    samplers = dict(existing_write=existing, new_write=write, prefetch=prefetch)
                    result = host_storage.capture_host_storage(plan, observations_path=path)
                    for name, sampler in samplers.items():
                        self.assertEqual(sampler.call_count, int(name == service))
                    self.assertEqual(load_json(path), {"status": "captured", **result})
                    workers.return_value.close.assert_called_once()

                    samplers[service].side_effect = RuntimeError("sampling failed")
                    with self.assertRaisesRegex(RuntimeError, "sampling failed"):
                        host_storage.capture_host_storage(plan, observations_path=path)
                    self.assertEqual(workers.return_value.close.call_count, 2)
                    self.assertEqual(load_json(path), {"status": "captured", **result})

    def test_existing_dma_domain_can_cover_a_smaller_supplement(self):
        report = dict(
            geometry={"kv_bytes_per_token_per_rank": 4},
            parameters={"devices": [0, 1]},
            environment={"ranks": []},
            target_workload_trace_used=False,
            target_e2e_used=False,
        )
        model = {"page_bandwidth_points": [{"page_bytes": size} for size in [128, 256, 512]]}
        with (
            patch.object(runtime_anchors, "load_json", return_value=report),
            patch.object(runtime_anchors, "_validate_runtime"),
            patch.object(runtime_anchors, "_service_model", return_value=model),
        ):
            projection = runtime_anchors.load_runtime_service_models(
                Path("report.json"),
                expected_kv_bytes_per_token_per_rank=4,
                expected_page_bytes=[128, 512],
                expected_concurrent_scope_count=2,
            )
            self.assertEqual(projection["load"], model)
            selected = runtime_anchors.load_runtime_service_models(
                Path("report.json"),
                expected_kv_bytes_per_token_per_rank=4,
                expected_page_bytes=[128, 512],
                expected_concurrent_scope_count=2,
                directions=("host_to_device",),
            )
            self.assertEqual(selected, {"load": model})
            with self.assertRaisesRegex(ValueError, "does not cover"):
                runtime_anchors.load_runtime_service_models(
                    Path("report.json"),
                    expected_kv_bytes_per_token_per_rank=4,
                    expected_page_bytes=[64],
                    expected_concurrent_scope_count=2,
                )

    def test_group_selects_requested_service_and_charges_previous_failed_attempts(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            group = SimpleNamespace(
                raw={
                    "physical_capture": {"page_token_sizes": [128]},
                    "physical_calibration": {"report": str(root / "base.json"), "measurement_sources": []},
                },
                physical={
                    "service_models": {"prefetch": {}},
                    "measurement_scope": {"storage_scope_devices": [0, 1], "storage_scope_numa_nodes": [0, 1]},
                },
                output_dir=root,
                physical_budget=PhysicalBudget(wall_seconds=100, container_starts=2, logical_io_bytes=1000),
            )
            definition = dict(
                limitations=[],
                logical_io_bytes={"prefetch": 200, "dma": 200, "existing_write": 200, "new_write": 200},
                geometry=dict(
                    num_hidden_layers=2,
                    num_key_value_heads_per_rank=2,
                    head_dim=8,
                    kv_element_bytes=2,
                    tensor_parallel_size=2,
                ),
                page_token_sizes=[128],
                devices=[0, 1],
                numa_nodes=[0, 1],
            )
            group.physical["kv_geometry"] = dict(definition["geometry"])
            attempts = [dict(stage="storage", status="failed", wall_seconds=10, reserved_logical_io_bytes=900)]
            with (
                patch.object(physical_capture, "physical_definition", return_value=definition),
                patch.object(physical_capture, "physical_attempts", return_value=attempts),
                patch.object(physical_capture, "captured_physical_declaration", return_value=None),
                patch.object(physical_capture, "require_repo_path", side_effect=Path),
                patch.object(physical_capture, "run_container_attempt") as launch,
            ):
                for component, stage in (
                    ("physical/load", "dma"),
                    ("physical/write_host_to_storage_existing", "existing_write"),
                    ("service/write_host_to_storage_new", "new_write"),
                    ("physical/prefetch_stages", "prefetch"),
                ):
                    preview = physical_capture.capture_physical(group, dry_run=True, required_components={component})
                    self.assertEqual(preview["physical_stages"], [{"stage": stage, "logical_io_bytes": 200}])
                result = physical_capture.capture_physical(group, dry_run=False, required_components={component})
                group.physical_budget = None
                with patch.object(physical_capture, "_completed", return_value={"report": "already_measured.json"}):
                    for dry in (False, True):
                        reused = physical_capture.capture_physical(group, dry_run=dry, required_components={component})
                        self.assertEqual(reused["status"], "physical_captured")
                        self.assertEqual(reused["physical_stages"], [])
                        self.assertEqual(reused["completed_components"], [component])
                definition["geometry"]["head_dim"] = 16
                incompatible = physical_capture.capture_physical(group, dry_run=False, required_components={component})
                self.assertEqual(incompatible["stop_reason"], "existing_physical_geometry_differs_from_base")
                definition["geometry"]["head_dim"] = 8
                definition["devices"] = [2, 3]
                mismatch = physical_capture.capture_physical(group, dry_run=False, required_components={component})
            self.assertEqual(result["status"], "physical_budget_exhausted")
            self.assertEqual(result["attempt"]["limits"], ["logical_io_bytes"])
            self.assertEqual(result["physical_capture_usage"]["logical_io_bytes"], 900)
            self.assertEqual(mismatch["stop_reason"], "existing_storage_placement_missing_or_different")
            launch.assert_not_called()

    def test_service_supplement_preserves_unrelated_costs_and_raw_samples(self):
        samples = tuple(
            dict(
                direction="host_to_storage",
                resource_state="sustained",
                page_bytes=512,
                operation_bytes_per_scope=size,
                operation_count=2,
                bytes=size * 2,
                duration_ns=1000 + size * 2,
                service_duration_ns=1000 + size * 2,
            )
            for size in (512, 1024)
        )
        existing_samples = tuple(
            dict(row, resource_state="existing_key", operation_pages_per_scope=count)
            for count, row in enumerate(samples, 1)
        )
        for service in ("prefetch", "existing_write", "new_write"):
            with self.subTest(service=service), TemporaryDirectory() as directory:
                root = Path(directory)
                base = dict(
                    kv_geometry={"kv_bytes_per_token_per_rank": 4, "model_name": "test", "tensor_parallel_size": 2},
                    storage_batch_pages=64,
                    service_models={
                        "load": {"measured": 2},
                        "prefetch": {"old": 3},
                        "write_host_to_storage": {"direction": "host_to_storage", "existing_key_bandwidth_points": [7]},
                    },
                    resource_lanes={"storage_read": "scope"},
                    measurement_sources=["independent-source"],
                    measurement_scope={"storage_scope_devices": [0, 1], "storage_scope_numa_nodes": [0, 1]},
                    target_workload_trace_used=False,
                    target_e2e_used=False,
                )
                write_json(root / "base.json", base)
                args = parse_args(
                    [
                        "--output-dir",
                        str(root / "out"),
                        "--storage-dir",
                        str(root / "storage"),
                        "--base-report",
                        str(root / "base.json"),
                        "--service",
                        service,
                        "--page-token-sizes",
                        "128",
                        "--storage-existing-operation-pages",
                        "1,2" if service == "existing_write" else "unused",
                        "--storage-new-write-operation-bytes-per-scope",
                        "512,1024",
                        "--storage-new-write-queue-bytes-per-scope",
                        "2048",
                    ]
                )
                measured = {
                    "host_storage": {
                        "samples": list(
                            samples
                            if service == "new_write"
                            else existing_samples
                            if service == "existing_write"
                            else ()
                        )
                    },
                    "prefetch_service_observations": [{"measured": "stage"}] if service == "prefetch" else [],
                }
                with (
                    patch.dict(
                        sys.modules, {"sglang.srt.mem_cache.hicache_storage": SimpleNamespace(STORAGE_BATCH_SIZE=64)}
                    ),
                    patch.object(io_calibration, "require_repo_path", side_effect=Path),
                    patch.object(io_calibration, "capture_host_storage", return_value=measured) as storage,
                    patch.object(bundle, "repo_relative_path", side_effect=Path),
                    patch.object(bundle, "build_prefetch_service_model", return_value={"stages": {"measured": 1}}),
                ):
                    result = io_calibration.capture_service(args, root / "out")
                    self.assertEqual(load_json(root / "base.json"), base)
                    with self.assertRaises(FileExistsError):
                        io_calibration.capture_service(args, root / "out")
                    write_json(root / "base.json", {**base, "target_e2e_used": True})
                    with self.assertRaisesRegex(ValueError, "independent"):
                        io_calibration.capture_service(args, root / "rejected")
                    write_json(root / "base.json", base)

                storage.assert_called_once()
                plan = storage.call_args.args[0]
                self.assertEqual(plan.devices, (0, 1))
                self.assertEqual(plan.numa_nodes, (0, 1))
                self.assertEqual(plan.service, service)
                self.assertEqual(plan.existing_operation_pages, (1, 2) if service == "existing_write" else ())
                self.assertEqual(plan.new_write_operations, (512, 1024) if service == "new_write" else ())
                self.assertEqual(
                    result["sample_counts"],
                    dict(
                        host_storage=len(measured["host_storage"]["samples"]),
                        prefetch_service=len(measured["prefetch_service_observations"]),
                    ),
                )
                report = load_json(result["report_path"])
                observations = load_json(result["observations_path"])
                if service == "new_write":
                    points = report["service_models"]["write_host_to_storage"].pop("new_operation_points")
                    self.assertGreater(points[0]["bandwidth_bytes_per_sec"], 0)
                    self.assertEqual(observations["host_storage"]["samples"], list(samples))
                elif service == "existing_write":
                    points = report["service_models"]["write_host_to_storage"].pop("existing_key_bandwidth_points")
                    self.assertEqual([point["operation_pages"] for point in points], [1, 2])
                    base["service_models"]["write_host_to_storage"].pop("existing_key_bandwidth_points")
                    self.assertEqual(observations["host_storage"]["samples"], list(existing_samples))
                else:
                    self.assertEqual(
                        observations["prefetch_service_observations"], measured["prefetch_service_observations"]
                    )
                    self.assertEqual(report["service_models"].pop("prefetch"), {"stages": {"measured": 1}})
                    base["service_models"].pop("prefetch")
                self.assertEqual(report["service_models"], base["service_models"])
                self.assertEqual(report["resource_lanes"], base["resource_lanes"])
                self.assertIn(str(root / "base.json"), report["measurement_sources"])
