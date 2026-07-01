"""HiCache probe 运行期上下文。"""

from __future__ import annotations

_TOKEN_PATHS_EMITTED_BY_SCOPE: dict[str, set[str]] = {}
_HICACHE_SEQUENCE_BY_SCOPE: dict[str, int] = {}
_TOKEN_HASH_ALGO = "sglang_radix_sha256_v1"
