"""Bind SGLang batch and request operands to observed Ascend kernel work."""

from __future__ import annotations

from typing import Any

from markov_collection.adapters.runtimes.ascend.kernel_work import AscendKernelWork


class SGLangKernelWork(AscendKernelWork):
    _attention_roles = {
        "ReshapeAndCacheNdKernel": "cache_write",
        "UnpadFlashAttentionBF16PrefixCacheNdKernel": "prefill_attention",
        "PagedAttentionMaskNdKernel": "decode_attention",
    }

    def describe(self, interval: dict[str, Any], metadata: dict[str, Any], step: str) -> dict[str, Any] | None:
        work = super().describe(interval, metadata, step)
        if work is None:
            return None

        operands = self._operand_work(work, metadata, step)
        if operands is None:
            return None  # Raw kernels remain available when actual operand facts are missing.
        if operands:
            work["operand_work"] = operands

        role = self._attention_roles.get(work["operation"])
        if role is not None:
            attention = self._attention(work["input_shapes"], metadata, role)
            if attention is None:
                return None  # Keep raw data; incomplete operands are not physical-only cost samples.
            work["attention_work"] = attention
        return work

    def _operand_work(self, work: dict[str, Any], metadata: dict[str, Any], step: str) -> dict[str, Any] | None:
        name = work["name"]
        if name not in (
            "compute_position_kernel",
            "aclnnIndex_IndexAiCore_Index",
            "aclnnIndexPutImpl_IndexPutV2_IndexPutV2",
            "write_req_to_token_pool_triton",
        ):
            return {}

        inputs = self._shapes(work["input_shapes"])
        outputs = self._shapes(work["output_shapes"])
        if work["input_formats"] != ";".join(["ND"] * len(inputs)) or work["output_formats"] != ";".join(
            ["ND"] * len(outputs)
        ):
            return None

        if step == "filter":
            return self._batch_filter(work, metadata, inputs, outputs)

        requests = metadata["requests"]
        batch = len(requests)
        lengths = [request["input_tokens"] for request in requests]
        tokens = sum(lengths)
        if name == "compute_position_kernel":
            if (
                step != "prefill"
                or metadata["phase"] != "prefill"
                or work["operation"] != "compute_position_kernel"
                or work["input_dtypes"] != "INT32;INT32"
                or work["output_dtypes"] != "INT64;INT32"
                or inputs != [(batch,), (batch,)]
                or outputs != [(tokens,), (batch,)]
            ):
                return None
            return {"position_lengths": lengths}

        if name == "aclnnIndex_IndexAiCore_Index" and work["output_dtypes"] == "DT_BF16":
            # This is the native non-padded prefill hidden-state selection.
            # Same-shaped Sub calls have several roles and are not classified.
            if (
                step != "prefill"
                or metadata["phase"] != "prefill"
                or work["operation"] != "Index"
                or work["input_dtypes"] != "DT_BF16;INT64;INT64;INT64"
                or len(inputs) != 4
                or len(inputs[0]) != 2
                or inputs != [(tokens, inputs[0][1]), (1,), (2,), (batch,)]
                or outputs != [(batch, inputs[0][1])]
            ):
                return None

            rows = []
            end = 0
            for length in lengths:
                end += length
                rows.append(end - 1)
            return {"logits_rows": rows}

        return self._request_pool(work, metadata, step, inputs, outputs)

    @staticmethod
    def _batch_filter(
        work: dict[str, Any],
        metadata: dict[str, Any],
        inputs: list[tuple[int, ...]],
        outputs: list[tuple[int, ...]],
    ) -> dict[str, Any] | None:
        selection = metadata["batch_filter"]
        rows = selection["input_count"]
        kept = len(selection["retained_indices"])
        if (
            work["name"] != "aclnnIndex_IndexAiCore_Index"
            or work["operation"] != "Index"
            or work["output_dtypes"] not in ("FLOAT", "INT32", "INT64")
            or work["input_dtypes"] != work["output_dtypes"] + ";INT64;INT64;INT64"
            or not 0 < kept < rows
            or len(inputs) != 4
            or len(inputs[0]) not in (1, 2)
        ):
            return None

        # Every native member tensor uses the same row selection. Tail extents
        # and dtype remain physical facts, not separate per-shape rules.
        tail = inputs[0][1:]
        if inputs != [(rows, *tail), (1,), (len(inputs[0]),), (kept,)] or outputs != [(kept, *tail)]:
            return None

        return {"batch_filter": selection}

    def _request_pool(
        self,
        work: dict[str, Any],
        metadata: dict[str, Any],
        step: str,
        inputs: list[tuple[int, ...]],
        outputs: list[tuple[int, ...]],
    ) -> dict[str, Any] | None:
        requests = metadata["requests"]
        slots = [request.get("request_pool_slot") for request in requests]
        table = metadata.get("request_pool_shape")
        if table is None or any(slot is None for slot in slots):
            return None  # Historical shapes do not reveal native reservations.

        rows, columns = table
        batch = len(requests)
        name = work["name"]
        role = None
        expected_inputs = []
        expected_outputs = []
        expected_dtypes = ""
        output_dtype = "INT32"
        if (
            name == "write_req_to_token_pool_triton"
            and work["operation"] == "write_req_to_token_pool_triton"
            and step == "prepare"
            and metadata["phase"] == "prefill"
        ):
            role = "prefix_extend_write"
            expected_inputs = [(batch,)] * 4
            expected_outputs = [(rows, columns)]
            expected_dtypes = "INT64;UINT64;INT64;INT64"
        elif name == "aclnnIndex_IndexAiCore_Index" and work["operation"] == "Index":
            if work["output_dtypes"] == "INT64" and step == "decode":
                role = "relay_read"
                expected_inputs = [(rows,), (1,), (1,), (batch,)]
                expected_outputs = [(batch,)]
                expected_dtypes = "INT64;INT64;INT64;INT64"
                output_dtype = "INT64"
            elif step == "prepare" and metadata["phase"] == "decode":
                role = "token_read"
                expected_inputs = [(rows, columns), (2,), (1,), (batch,), (batch,)]
                expected_outputs = [(batch,)]
                expected_dtypes = "INT32;INT64;INT64;INT64;INT64"
            elif step in ("prefill", "decode"):
                role = "row_read"
                sequence = max(request["context_tokens"] + request["input_tokens"] for request in requests)
                expected_inputs = [(rows, sequence), (1,), (2,), (batch,)]
                expected_outputs = [(batch, sequence)]
                expected_dtypes = "INT32;INT64;INT64;INT64"
        elif name == "aclnnIndexPutImpl_IndexPutV2_IndexPutV2" and work["operation"] == "IndexPutV2":
            if work["output_dtypes"] == "INT64" and step in ("prefill", "decode"):
                role = "relay_write"
                expected_inputs = [(rows,), (batch,), (1,), (1,), (batch,)]
                expected_outputs = [(rows,)]
                expected_dtypes = "INT64;INT64;INT64;INT64;INT64"
                output_dtype = "INT64"
            elif step == "prepare" and metadata["phase"] == "decode":
                role = "token_write"
                expected_inputs = [(rows, columns), (batch,), (2,), (2,), (batch,), (batch,)]
                expected_outputs = [(rows, columns)]
                expected_dtypes = "INT32;INT32;INT64;INT64;INT64;INT64"

        if (
            role is None
            or inputs != expected_inputs
            or outputs != expected_outputs
            or work["input_dtypes"] != expected_dtypes
            or work["output_dtypes"] != output_dtype
        ):
            return None

        access: dict[str, Any] = {"role": role}
        if role == "prefix_extend_write":
            access["members"] = [
                {"slot": slot, "prefix_tokens": request["context_tokens"], "input_tokens": request["input_tokens"]}
                for slot, request in zip(slots, requests, strict=True)
            ]
        elif role in ("token_read", "token_write"):
            offset = -1 if role == "token_read" else 0
            access["members"] = [
                {"slot": slot, "column": request["context_tokens"] + offset}
                for slot, request in zip(slots, requests, strict=True)
            ]
        else:
            access["slots"] = slots
        return {"request_pool": access}

    def _attention(self, shape_text: str, metadata: dict[str, Any], role: str) -> dict[str, Any] | None:
        shapes = self._shapes(shape_text)
        cache_slot = 2 if role == "cache_write" else 1
        if len(shapes) <= cache_slot or len(shapes[cache_slot]) != 4:
            return None
        pages, page_size, kv_heads, head_dim = shapes[cache_slot]
        tokens = sum(request["input_tokens"] for request in metadata["requests"])
        query_heads = None

        # These primitive signatures are the observed ND backend path. Capacity
        # comes from the actual cache tensor; live lengths come from its batch.
        if role == "cache_write":
            if shapes[0] != (tokens, kv_heads, head_dim) or shapes[1] != shapes[0]:
                return None
        elif role == "prefill_attention":
            if metadata["phase"] != "prefill" or len(shapes[0]) != 2 or shapes[0][0] != tokens:
                return None
            width = shapes[0][1]
            if width % head_dim:
                return None
            query_heads = width // head_dim
        else:
            if (
                metadata["phase"] != "decode"
                or len(shapes[0]) != 3
                or shapes[0][0] != tokens
                or shapes[0][2] != head_dim
            ):
                return None
            query_heads = shapes[0][1]

        return {
            "role": role,
            "sequences": [
                {"query_tokens": request["input_tokens"], "prefix_tokens": request["context_tokens"]}
                for request in metadata["requests"]
            ],
            "query_heads": query_heads,
            "kv_heads": kv_heads,
            "head_dim": head_dim,
            "page_size_tokens": page_size,
            "cache_pages": pages,
        }
