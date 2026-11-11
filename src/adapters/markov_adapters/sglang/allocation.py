"""Declare the installed allocator's structural branches, without allocating resources."""


class AllocationABI:
    def __init__(self, implementation: str) -> None:
        if implementation != "sglang.srt.hardware_backend.npu.allocator_npu.NPUPagedTokenToKVPoolAllocator":
            raise ValueError(f"allocation structure has no declared implementation: {implementation}")
        self.implementation = implementation

    def branch(self, members, page_size, decode):
        if page_size == 1:
            return "token"
        pages = sum(
            (member["context_tokens"] + member["new_tokens"] + page_size - 1) // page_size
            - (member["context_tokens"] + page_size - 1) // page_size
            for member in members
        )
        if decode:
            return "decode_pages" if pages else "decode_tail"

        return self.extend_branch(pages)

    @staticmethod
    def extend_branch(pages):

        # NPUPagedTokenToKVPoolAllocator.alloc_extend selects its native kernel below 200 new pages.
        return "extend_kernel" if pages < 200 else "extend_naive"
