"""Import native kernel declarations without loading their accelerator implementations."""

import sys
import types
from pathlib import Path


class DeclarationDriver:
    def get_benchmarker(self):
        return self.reject

    @staticmethod
    def reject(*args, **kwargs):
        raise RuntimeError("Native control must submit physical work to Markov execution")


class CpuDeclarations:
    @staticmethod
    def install(kernel_python: Path) -> None:
        import triton

        triton.runtime.driver.set_active(DeclarationDriver())

        # The fixed upstream Python declarations are retained; only their binary loader is replaced.
        sys.path.insert(0, str(kernel_python))
        loader = types.ModuleType("sgl_kernel.load_utils")
        loader.__package__ = "sgl_kernel"
        loader._load_architecture_specific_ops = CpuDeclarations.load_declarations
        loader._preload_cuda_library = CpuDeclarations.load_declarations
        sys.modules[loader.__name__] = loader

    @staticmethod
    def load_declarations() -> None:
        pass
