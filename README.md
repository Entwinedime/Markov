# Markov

Markov predicts LLM serving performance for one model across a set of configurations and request workloads. It builds reusable execution templates and cost assets from representative SGLang runs on Ascend, then reports prediction status, TTFT, TPOT, throughput, and configuration comparisons. Each workload supplies output lengths and any expert routes required by the configurations.

## Build

Run these commands from the checkout on an Ascend host with Docker Compose, Docker BuildKit enabled, the Ascend container runtime, access to `/var/run/docker.sock`, and Python 3.9 or newer. The example needs one available physical NPU and Qwen3-8B model files at `/models/Qwen3-8B`. It runs with BF16 dummy weights.

```bash
git submodule update --init third_party/sglang
docker build -t markov-next-sglang-env:ubuntu22.04 docker/images/base/sglang
./docker/build_inference.sh
docker compose -f docker/compose/inference.yml build modeling
mkdir -p build/coarse-graph
docker run --rm --runtime runc --network none \
  -v "$PWD:/repo:ro" -v "$PWD/build/coarse-graph:/build" \
  markov-next-modeling:ubuntu24.04 bash -c \
  'cmake -S /repo -B /build -DCMAKE_BUILD_TYPE=Release && cmake --build /build --parallel 8'
```

You can reuse the base image if it is already available under the specified tag. For a build proxy listening on the host loopback interface, run `MARKOV_BUILD_NETWORK=host ./docker/build_inference.sh`.

## Run the example

Set `physical_devices` in the deployment JSON to the physical card reserved for this run. The default [example input](example/coarse_graph/input.json) combines two TP1 configurations with two short workloads.

```bash
mkdir -p data
printf '{"physical_devices": [0]}\n' > data/example-deployment.json
job="data/$(date -u +%Y%m%dT%H%M%SZ)_exploration"
python3 example/coarse_graph/run.py --deployment data/example-deployment.json --output "$job" \
  --timeout 420 --native-budget-seconds 2100 --postprocessing-budget-seconds 1200 \
  --attempt-limit 5 --storage-bytes 25769803776
```

Open `summary.md` in the job directory for per-combination results. A successful cold run reports four completed predictions and records the assets and evidence for each combination in `result.json`. Use `--details` to retain the source intermediates and target execution traces.

The second run uses the same configurations, deployment, and build. Keep the first job directory at its original path so the recorded assets remain available. The second workload set changes the request identities and arrival times and adds a later request with the same lengths to the single-request workload.

```bash
reuse_input="${job}-reuse-input.json"
python3 - > "$reuse_input" <<'PY'
import json
from pathlib import Path

task = json.loads(Path("example/coarse_graph/input.json").read_text())
for workload in task["workloads"]:
    workload["id"] += "-rerun"
    for request in workload["requests"]:
        request["id"] += "-rerun"
        request["arrival_ns"] += 4_000_000

    if workload["id"] == "single-rerun":
        first = workload["requests"][0]
        workload["requests"].append({**first, "id": "followup", "arrival_ns": first["arrival_ns"] + 2_000_000_000})
print(json.dumps(task, indent=2))
PY
python3 example/coarse_graph/run.py --deployment data/example-deployment.json \
  --input "$reuse_input" --reuse "$job" --output "${job}-reuse" \
  --timeout 420 --native-budget-seconds 0 --postprocessing-budget-seconds 0 \
  --attempt-limit 0 --storage-bytes 25769803776
```

The reuse run should also report four completed predictions. In its `result.json`, every target should have `asset_use` set to `reused`, and `usage.native_attempts`, `usage.native_charged_ns`, and `usage.postprocessing_ns` should all be zero. The results include the status and evidence for any combination that could not be predicted.

## Validated configurations

The single-host validation used SGLang on Ascend, dummy weights, and workloads with fixed output lengths. Each entry describes measured configurations and workload mechanisms.

| Model | Dtype | Configuration | Checked cases |
| --- | --- | --- | --- |
| Qwen3-8B | BF16 | TP1 with 16-token pages | Page boundaries and normal and burst request traffic |
| Qwen3-8B | BF16 | TP2 with 16-token pages | Dual-card serving, chunked prefill with mixed batches, and first-level prefix-cache hits and eviction |
| Qwen3-8B | BF16 | TP8 with 32-token pages | Two staggered requests on eight cards |
| Qwen3-8B | BF16 | Prefill TP1 and Decode TP2 with 16-token pages | Asymmetric disaggregation with KV and metadata handoffs |
| Mistral 7B | FP16 | PP3 with 32-token pages and TP2 × PP3 with 16-token pages | Pipeline batches and combined tensor and pipeline parallelism. Host transfer timing is checked at the endpoints. |
| Qwen3-30B-A3B | BF16 | TP4 × EP4 with 128-token pages | Expert parallelism with explicit routes in the workload |

See the [development and workflow guide](docs/design/development.md) for input, output, and verification details.
