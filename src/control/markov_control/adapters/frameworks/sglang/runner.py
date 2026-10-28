"""Preserve native initialization while replacing physical model and pool construction."""

import torch
from markov_control.adapters.frameworks.sglang.resources import PageAllocator, RequestSlots
from sglang.srt.distributed import parallel_state
from sglang.srt.layers.dp_attention import get_attention_tp_group, initialize_dp_attention
from sglang.srt.mem_cache.common import get_req_to_token_extra_context_len
from sglang.srt.model_executor.model_runner import ModelRunner
from sglang.srt.model_executor.pool_configurator import MemoryPoolConfig


class UnexecutedModel(torch.nn.Module):
    def forward(self, *args, **kwargs):
        raise RuntimeError("Model execution belongs to the core graph")


class MetadataRunner(ModelRunner):
    def __init__(self, *args, peer, rendezvous: str, **kwargs):
        self.peer = peer
        self.rendezvous = rendezvous
        super().__init__(*args, **kwargs)

    def load_model(self):
        self.model = UnexecutedModel()
        self.dtype = self.model_config.dtype
        self.sliding_window_size = self.model_config.sliding_window_size
        self.weight_load_mem_usage = 0
        self.load_config = None

    def init_threads_binding(self):
        torch.set_num_threads(1)

    def init_torch_distributed(self):

        # A single native worker uses a real local process group without physical collectives.
        parallel_state.init_distributed_environment(
            backend="gloo", world_size=1, rank=0, local_rank=0, distributed_init_method=self.rendezvous
        )
        parallel_state.initialize_model_parallel(
            tensor_model_parallel_size=1, pipeline_model_parallel_size=1, expert_model_parallel_size=1
        )
        initialize_dp_attention(self.server_args, self.model_config)
        self.tp_group = parallel_state.get_tp_group()
        self.pp_group = parallel_state.get_pp_group()
        self.attention_tp_group = get_attention_tp_group()
        return 0

    def _resolve_memory_pool_config(self, pre_model_load_memory):
        return MemoryPoolConfig(
            max_total_num_tokens=self.server_args.max_total_tokens,
            max_running_requests=self.server_args.max_running_requests,
        )

    def _init_pools(self):
        context = self.model_config.context_len + get_req_to_token_extra_context_len(self.server_args)
        self.req_to_token_pool = RequestSlots(self.peer, self.max_running_requests, context)
        self.token_to_kv_pool_allocator = PageAllocator(
            self.peer, self.max_total_num_tokens, self.server_args.page_size
        )
        self.token_to_kv_pool = self.token_to_kv_pool_allocator.storage

    def init_attention_backends(self):
        self.prefill_attention_backend_str = "torch_native"
        self.decode_attention_backend_str = "torch_native"

    def init_cuda_graphs(self, capture_decode_cuda_graph=True):
        self.eager_runner = None
        self.prefill_cuda_graph_runner = None
        self.decode_cuda_graph_runner = None
        self.graph_mem_usage = 0
