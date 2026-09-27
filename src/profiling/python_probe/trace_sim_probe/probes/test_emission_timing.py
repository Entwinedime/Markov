"""Optional probe timing must not change the original business observation."""

import unittest
import json
import asyncio
import inspect
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

from trace_sim_probe.probes import generic_callable as probe


class EmissionTimingTests(unittest.TestCase):
    def test_enqueue_catalog_only_reads_token_length(self):
        catalog = next(
            parent / "configs/profiling/hicache_probe_targets.json"
            for parent in Path(__file__).resolve().parents
            if (parent / "configs/profiling/hicache_probe_targets.json").exists()
        )
        raw = next(
            t for t in json.loads(catalog.read_text()) if t["id"] == "hicache_controller.writeback_enqueue_observed"
        )
        raw.pop("capture_emission_timing", None)
        raw["fact"]["consumers"] = ["hicache_dag_patch"]
        target = probe._parse_target(raw)

        class LengthOnly:
            def __len__(self):
                return 2944

            def __iter__(self):
                raise AssertionError("enqueue probe must not traverse token data")

        def write_storage(self, host_indices, token_ids, hash_value=None):
            return 7

        arguments = (SimpleNamespace(page_size=128), None, LengthOnly(), ["page"])
        bound = probe._bind_arguments(inspect.signature(write_storage), arguments, {})
        probe._bind_trace_context(bound, target, "end")
        fields, missing = probe._collect_fields(target, bound, arguments, 7)
        self.assertFalse(missing)
        self.assertEqual(fields["token_count"], 2944)
        self.assertEqual(fields["operation_id"], 7)
        self.assertNotIn("token_dictionary", fields)
        self.assertNotIn("full_path_span", fields)

    def setUp(self):
        self.raw = {
            "id": "test",
            "module": "test",
            "target": "call",
            "events": {"end": "test_end"},
            "fields": [],
            "fact": {
                "class": "source_actual",
                "role": "writeback_enqueue_observed",
                "consumers": ["hicache_dag_patch"],
            },
        }
        self.target = probe._parse_target(self.raw)

    def emit(self, target, writer):
        with patch.object(probe, "get_writer", return_value=writer):
            probe._emit_targets((target,), None, (), {}, None, "end", 10, 20, None)

    def test_default_has_no_extra_clocks_or_events(self):
        writer = Mock()
        with patch.object(probe, "_emission_clock", side_effect=AssertionError("unexpected clock")):
            self.emit(self.target, writer)
        self.assertEqual(writer.duration_event.call_count, 1)

    def test_installed_wrapper_resolves_signature_once_and_keeps_events(self):
        def call(value, *, flag=False):
            return value + 1

        target = replace(
            self.target,
            events={"start": "begin", "end": "end"},
            fields=(probe.FieldSpec("value", "arg:value"), probe.FieldSpec("flag", "arg:flag")),
        )
        writer = Mock()
        writer.now_us.side_effect = range(10, 30)
        with (
            patch.object(probe, "get_writer", return_value=writer),
            patch.object(probe.inspect, "signature", wraps=inspect.signature) as signature,
        ):
            wrapped = probe._wrap_callable((target, replace(target, id="second")), call)
            self.assertEqual(wrapped(3), 4)
            self.assertEqual(wrapped(value=5, flag=True), 6)
            self.assertEqual(signature.call_count, 1)
        records = writer.duration_event.call_args_list
        self.assertEqual([r.args[0] for r in records], ["begin", "begin", "end", "end"] * 2)
        self.assertEqual([(r.args[4]["value"], r.args[4]["flag"]) for r in records], [(3, False)] * 4 + [(5, True)] * 4)
        self.assertTrue(all(not r.args[4]["missing_required_fields"] for r in records))

    def test_field_property_is_read_once(self):
        class Observed:
            reads = 0

            @property
            def value(self):
                self.reads += 1
                return self.reads

        observed = Observed()
        self.assertEqual(probe._read_path(observed, "value"), (True, 1))
        self.assertEqual(observed.reads, 1)
        self.assertEqual(probe._read_path(observed, "missing"), (False, None))

    def test_async_wrapper_reuses_signature(self):
        async def call(value=4):
            return value

        writer = Mock()
        writer.now_us.side_effect = range(10, 30)
        with (
            patch.object(probe, "get_writer", return_value=writer),
            patch.object(probe.inspect, "signature", wraps=inspect.signature) as signature,
        ):
            wrapped = probe._wrap_callable((self.target,), call)
            self.assertEqual(asyncio.run(wrapped()), 4)
            self.assertEqual(asyncio.run(wrapped(value=7)), 7)
            self.assertEqual(signature.call_count, 1)
        self.assertEqual(writer.duration_event.call_count, 2)

    def test_uninspectable_unhashable_callable_keeps_positional_fallback(self):
        class Call:
            __hash__ = None

            @property
            def __signature__(self):
                raise ValueError("no signature")

            def __call__(self, value):
                return value

        target = replace(self.target, fields=(probe.FieldSpec("value", "arg:0"),))
        writer = Mock()
        writer.now_us.side_effect = range(10, 30)
        with patch.object(probe, "get_writer", return_value=writer):
            wrapped = probe._wrap_callable((target,), Call())
            self.assertEqual(wrapped(8), 8)
        self.assertEqual(writer.duration_event.call_args.args[4]["value"], 8)

    def test_stages_keep_business_event_and_measure_separate_intervals(self):
        original, diagnostic = Mock(), Mock()
        self.emit(self.target, original)
        with patch.object(
            probe, "_emission_clock", side_effect=[(20, 0), (21, 100), (25, 2100), (27, 3000), (30, 4000)]
        ):
            self.emit(replace(self.target, capture_emission_timing=True), diagnostic)
        calls = diagnostic.duration_event.call_args_list
        self.assertEqual(calls[0], original.duration_event.call_args)
        self.assertEqual(len(calls), 4)
        for call, stage, begin, end, cpu in (
            (calls[1], "binding", 20, 21, 100),
            (calls[2], "fields", 21, 25, 2000),
            (calls[3], "writer", 27, 30, 1000),
        ):
            self.assertEqual(call.args[:4], ("python_probe.emission_work", begin, end, "runtime_diagnostic"))
            self.assertEqual(call.args[4]["stage"], stage)
            self.assertEqual(call.args[4]["thread_cpu_ns"], cpu)
            self.assertEqual(call.args[4]["observed_end_us"], 20)
            self.assertNotIn("target_id", call.args[4])
            self.assertEqual(call.args[4]["observed_target_id"], "test")

    def test_flag_is_boolean_and_explicit(self):
        self.assertFalse(self.target.capture_emission_timing)
        self.assertTrue(probe._parse_target({**self.raw, "capture_emission_timing": True}).capture_emission_timing)
        with self.assertRaises(ValueError):
            probe._parse_target({**self.raw, "capture_emission_timing": "true"})

    def test_filtered_event_emits_no_diagnostics(self):
        writer = Mock()
        with (
            patch.object(probe, "_should_emit_target", return_value=False),
            patch.object(probe, "_emission_clock", return_value=(0, 0)) as clock,
        ):
            self.emit(replace(self.target, capture_emission_timing=True), writer)
        clock.assert_called_once_with()
        writer.duration_event.assert_not_called()
