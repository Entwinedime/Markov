"""Small, device-free checks for optional runtime preparation markers."""

import types
import unittest
from unittest.mock import Mock, patch

from trace_sim_probe import bootstrap
from trace_sim_probe.probes import runtime_preparation as probe


class RuntimePreparationCheck(unittest.TestCase):
    def test_default_off_and_explicit_diagnostics(self):
        for level, count in (("off", 1), ("timing", 3), ("full", 3)):
            with self.subTest(level=level), patch.object(bootstrap, "_PROBES", None), \
                    patch.dict("os.environ", {"TRACE_SIM_PYTHON_PROBE_DIAGNOSTICS": level}), \
                    patch.object(bootstrap.importlib, "import_module") as load:
                self.assertEqual(len(bootstrap._probes()), count)
                self.assertEqual(load.call_count, count)
                bootstrap._probes()
                self.assertEqual(load.call_count, count)

    def test_prepare_preserves_return_and_exception(self):
        class JITFunction:
            fn = staticmethod(lambda: None)

            def _do_compile(self, signature, constants, fail=False, attrs=None):
                if fail:
                    raise ValueError("compile failed")
                return constants

        module = types.ModuleType("triton.runtime.jit")
        module.JITFunction = JITFunction
        probe.install(module)
        wrapped = JITFunction._do_compile
        probe.install(module)
        self.assertIs(wrapped, JITFunction._do_compile)
        writer = Mock()
        writer.now_us.side_effect = [10, 30, 40, 70]
        with patch.object(probe, "get_writer", return_value=writer):
            constants = {"page_size": 64}
            attrs = types.SimpleNamespace(arg_properties={"tt.divisibility": [0, 1]})
            self.assertIs(JITFunction()._do_compile({"pages": "*i64"}, constants, attrs=attrs), constants)
            with self.assertRaisesRegex(ValueError, "compile failed"):
                JITFunction()._do_compile({}, {}, fail=True)
        first, second = writer.duration_event.call_args_list
        self.assertEqual(first.args[:4], ("runtime.triton.prepare", 10, 30, "runtime_diagnostic"))
        self.assertEqual(first.args[4]["constants"], constants)
        self.assertEqual(first.args[4]["argument_properties"], attrs.arg_properties)
        self.assertEqual(first.args[4]["status"], "returned")
        self.assertEqual(second.args[4]["status"], "raised")
        self.assertNotIn("fact", first.args[4])

    def test_only_unloaded_kernel_emits(self):
        class CompiledKernel:
            module = None
            name = "allocator"
            calls = 0

            def _init_handles(self):
                self.calls += 1
                self.module = "loaded"

        module = types.ModuleType("triton.compiler.compiler")
        module.CompiledKernel = CompiledKernel
        probe.install(module)
        kernel = CompiledKernel()
        writer = Mock()
        writer.now_us.side_effect = [10, 20]
        with patch.object(probe, "get_writer", return_value=writer):
            kernel._init_handles()
            kernel._init_handles()
        self.assertEqual(kernel.calls, 2)
        writer.duration_event.assert_called_once()
        self.assertEqual(writer.duration_event.call_args.args[0], "runtime.triton.load")

    def test_partial_import_is_not_marked_installed(self):
        module = types.ModuleType("triton.runtime.jit")
        probe.install(module)
        self.assertFalse(hasattr(module, "JITFunction"))

    def test_parent_import_installs_loaded_child(self):
        module = types.ModuleType("triton.runtime.jit")
        plugin = types.SimpleNamespace(TARGET_MODULES=(module.__name__,), install=Mock())
        with patch.object(bootstrap, "_PROBES", (plugin,)), \
                patch.dict("sys.modules", {module.__name__: module}):
            bootstrap._post_import_apply("triton")
        plugin.install.assert_called_once_with(module)


if __name__ == "__main__":
    unittest.main()
