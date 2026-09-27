"""Import dispatch may be cached; loaded module state must remain live."""

import importlib
import sys
import unittest
from types import ModuleType
from unittest.mock import Mock, patch

bootstrap = importlib.import_module("trace_sim_probe.bootstrap")


class RuntimeSelectionTests(unittest.TestCase):
    def test_consumer_and_diagnostics_select_observations_once(self):
        causal = ["runtime_preparation", "response_boundaries", "cpu_collectives", "layer_waits", "decode_allocation"]
        cases = (
            ("hicache_state_model,hicache_dag_patch", "off", causal),
            ("hicache_state_model", "off", []),
            ("", "off", []),
            ("", "timing", causal),
        )
        for consumers, level, selected in cases:
            with (
                self.subTest(consumers=consumers, diagnostics=level),
                patch.object(bootstrap, "_PROBES", None),
                patch.dict(
                    "os.environ",
                    {
                        "TRACE_SIM_PYTHON_PROBE_CONSUMERS": consumers,
                        "TRACE_SIM_PYTHON_PROBE_DIAGNOSTICS": level,
                    },
                ),
                patch.object(bootstrap.importlib, "import_module") as load,
            ):
                bootstrap._probes()
                bootstrap._probes()
                self.assertEqual(
                    [call.args[0] for call in load.call_args_list],
                    ["trace_sim_probe.probes." + name for name in ["generic_callable", *selected]],
                )


class ImportDispatchTests(unittest.TestCase):
    def setUp(self):
        self.probe = ModuleType("test_probe")
        self.probe.TARGET_MODULES = ("probe_test_pkg.child",)
        self.probe.install = Mock()
        self.probes = patch.object(bootstrap, "_PROBES", (self.probe,))
        self.probes.start()
        bootstrap._matching_probes.cache_clear()

    def tearDown(self):
        bootstrap._matching_probes.cache_clear()
        self.probes.stop()

    def test_ancestors_exact_and_descendants_match(self):
        for name in ("probe_test_pkg", "probe_test_pkg.child", "probe_test_pkg.child.deep"):
            self.assertEqual(bootstrap._matching_probes(name), (self.probe,))
        for name in ("math", "probe_test_pkg_extra", "probe_test_pkg.child_extra"):
            self.assertEqual(bootstrap._matching_probes(name), ())

    def test_unrelated_import_skips_install_and_reuses_dispatch(self):
        with patch.object(bootstrap, "_probes", wraps=bootstrap._probes) as probes:
            bootstrap._import_hook("math")
            bootstrap._import_hook("math")
            probes.assert_called_once_with()
        self.probe.install.assert_not_called()

    def test_install_failure_is_not_hidden_by_cache(self):
        name = self.probe.TARGET_MODULES[0]
        with patch.dict(sys.modules, {name: ModuleType(name)}):
            self.probe.install.side_effect = RuntimeError("incomplete target")
            with self.assertRaisesRegex(RuntimeError, "incomplete target"):
                bootstrap._post_import_apply("probe_test_pkg")
            self.probe.install.side_effect = None
            bootstrap._post_import_apply("probe_test_pkg")
            self.assertEqual(self.probe.install.call_count, 2)

    def test_real_callable_install_tracks_replacement_not_module_name(self):
        from trace_sim_probe.probes import generic_callable

        name = self.probe.TARGET_MODULES[0]
        target = generic_callable._parse_target(
            {
                "id": "reload",
                "module": name,
                "target": "call",
                "events": {"end": "observed"},
                "fields": [],
                "fact": {
                    "class": "source_actual",
                    "role": "writeback_enqueue_observed",
                    "consumers": ["hicache_dag_patch"],
                },
            }
        )
        self.probe.install = generic_callable.install
        writer = Mock()
        writer.now_us.side_effect = range(20)
        with (
            patch.object(generic_callable, "_TARGETS", [target]),
            patch.object(generic_callable, "get_writer", return_value=writer),
            patch.dict(sys.modules),
        ):
            sys.modules.pop(name, None)
            bootstrap._post_import_apply(name)
            for returned in (1, 2):
                module = sys.modules[name] = ModuleType(name)
                bootstrap._post_import_apply(name)
                module.call = lambda: returned
                bootstrap._post_import_apply(name)
                installed = module.call
                bootstrap._post_import_apply(name)
                self.assertIs(module.call, installed)
                self.assertEqual(module.call(), returned)
        self.assertEqual(writer.duration_event.call_count, 2)


if __name__ == "__main__":
    unittest.main()
