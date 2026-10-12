"""Apply necessary Ascend runtime adaptations to the image-local official package."""

from __future__ import annotations

import importlib.util
from pathlib import Path


class AscendRuntime:
    def __init__(self, package: Path) -> None:
        self.package = package

    def apply(self) -> None:
        for relative, anchor in (
            (
                "server/backend/interfaces/balance_serve.py",
                "\nif use_torch_npu:\n    from ktransformers.models.ascend.",
            ),
            ("server/balance_serve/inference/model_runner.py", "\ndef pad_num_tokens(num_tokens):\n"),
        ):
            path = self.package / relative
            lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
            imports = [line for line in lines if line.startswith("from ktransformers.models.custom_modeling_")]
            text = "".join(line for line in lines if line not in imports)
            guarded = "if not use_torch_npu:\n" + "".join("    " + line for line in imports) + "\n"
            if not imports or text.count(anchor) != 1:
                raise ValueError(f"official CUDA import boundary changed: {relative}")
            path.write_text(text.replace(anchor, "\n" + guarded + anchor), encoding="utf-8")

        path = self.package / "server/balance_serve/inference/sampling/sampler.py"
        text = path.read_text(encoding="utf-8")
        begin = text.index("from flashinfer.sampling import (")
        end = text.index("\n)", begin) + len("\n)")
        imports = text[begin:end]
        text = text[:begin] + text[end:]
        anchor = "logger = logging.getLogger(__name__)"
        guarded = "if not use_torch_npu:\n" + "\n".join("    " + line for line in imports.splitlines()) + "\n"
        if text.count(anchor) != 1:
            raise ValueError("official sampler import boundary changed")
        path.write_text(text.replace(anchor, guarded + anchor), encoding="utf-8")

        path = self.package / "operators/linear.py"
        text = path.read_text(encoding="utf-8")
        path.write_text(
            text.replace("if not torch.xpu.is_available():", "if not torch.xpu.is_available() and not use_torch_npu:"),
            encoding="utf-8",
        )

        path = self.package / "util/vendors.py"
        text = path.read_text(encoding="utf-8")
        boundary = text.index("# Get devices\n")
        demonstration = text[boundary:]
        path.write_text(
            text[:boundary]
            + 'if __name__ == "__main__":\n'
            + "\n".join("    " + line for line in demonstration.splitlines())
            + "\n",
            encoding="utf-8",
        )

        path = self.package / "operators/base_operator.py"
        text = path.read_text(encoding="utf-8")
        text = text.replace("import ktransformers.util.utils as utils\n", "")
        text = text.replace(
            "    def load(self):\n", "    def load(self):\n        from ktransformers.util.utils import load_weights\n"
        )
        path.write_text(text.replace("utils.load_weights(child,", "load_weights(child,"), encoding="utf-8")

        path = self.package / "server/balance_serve/inference/model_runner.py"
        text = path.read_text(encoding="utf-8")
        begin = text.index("    def replay(")
        end = text.index("\n    def sync(", begin)
        replay = text[begin:end].replace("with torch.cuda.stream(self.stream):", "with self.stream_scope(self.stream):")
        path.write_text(text[:begin] + replay + text[end:], encoding="utf-8")


if __name__ == "__main__":
    package = Path(importlib.util.find_spec("ktransformers").origin).parent
    AscendRuntime(package).apply()
