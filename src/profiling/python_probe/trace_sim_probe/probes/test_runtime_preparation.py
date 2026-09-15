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

    def test_prepare_path_uses_ir_entry_not_elapsed_time(self):
        class ASTSource:
            def __init__(self, fn):
                self.fn = fn

            def make_ir(self):
                return "ir"

        class JITFunction:
            fn = staticmethod(lambda: None)

            def _do_compile(self, path):
                source = self.ASTSource(self)
                if path == "compile":
                    source.make_ir()
                if path == "fail":
                    source.make_ir()
                    raise ValueError("compile failed")
                if path == "nested":
                    JITFunction()._do_compile("compile")
                return None if path == "hook" else types.SimpleNamespace(src=source)

        JITFunction.ASTSource = staticmethod(lambda fn: ASTSource(fn))
        compiler = types.ModuleType("triton.compiler.compiler")
        compiler.ASTSource = ASTSource
        jit = types.ModuleType("triton.runtime.jit")
        jit.JITFunction = JITFunction
        mode = Mock(return_value=None)
        jit._async_compile = types.SimpleNamespace(active_mode=types.SimpleNamespace(get=mode))
        probe.install(compiler)
        probe.install(jit)
        writer = Mock()
        writer.now_us.return_value = 10  # Identical timings cannot classify paths.
        original_entry = ASTSource.make_ir
        ASTSource.make_ir = lambda self: original_entry(self)  # Lazy backend wrapper, no copied marker.
        with patch.object(probe, "get_writer", return_value=writer), \
                patch.dict("sys.modules", {compiler.__name__: compiler}):
            for path in ("compile", "cached", "nested", "hook"):
                JITFunction()._do_compile(path)
            with self.assertRaisesRegex(ValueError, "compile failed"):
                JITFunction()._do_compile("fail")
            mode.return_value = object()
            JITFunction()._do_compile("compile")
        fields = [call.args[4] for call in writer.duration_event.call_args_list]
        self.assertEqual([row["path"] for row in fields],
                         ["compiled", "disk_cache", "compiled", "disk_cache", "unknown", "compiled", "async_submit"])
        self.assertEqual(fields[-2]["status"], "raised")
        self.assertEqual(fields[-1]["execution_mode"], "async")
        self.assertIsNone(probe._PREPARATION.get())

    def test_unobserved_ir_path_stays_unknown(self):
        class JITFunction:
            fn = staticmethod(lambda: None)

            def _do_compile(self):
                return types.SimpleNamespace(src=types.SimpleNamespace(fn=self))

        module = types.ModuleType("triton.runtime.jit")
        module.JITFunction = JITFunction
        module._async_compile = types.SimpleNamespace(active_mode=types.SimpleNamespace(get=lambda: None))
        probe.install(module)
        writer = Mock()
        with patch.object(probe, "get_writer", return_value=writer):
            JITFunction()._do_compile()
        self.assertEqual(writer.duration_event.call_args.args[4]["path"], "unknown")

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
