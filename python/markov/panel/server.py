"""Loopback-only browser adapter for the same public workflows used by the CLI."""

from __future__ import annotations

from dataclasses import asdict
import csv
from http.server import BaseHTTPRequestHandler, HTTPServer
import io
import json
from pathlib import Path
import secrets
import shutil
import sqlite3
from urllib.parse import urlsplit

from markov.artifacts.performance import PerformanceData, maintenance_document
from markov.artifacts.request import PredictionRequest, read_json
from markov.artifacts.workload import Workload
from markov.artifacts.writing import write_json
from markov.workflow.planning import plan_prediction
from markov.workflow.search import SearchPolicy
from markov.workflow.tasks import Tasks, task_lock


ASSETS = Path(__file__).resolve().parent
ROOT = ASSETS.parents[2]


class Panel(HTTPServer):
    def __init__(self, directory: Path, port: int):
        self.tasks = Tasks(directory)
        self.token = secrets.token_urlsafe(32)
        super().__init__(("127.0.0.1", port), Handler)
        self.origin = f"http://127.0.0.1:{self.server_port}"

    def prepare(self, body: dict) -> tuple[PredictionRequest, dict]:
        raw = body["request"]
        request = PredictionRequest.parse(raw, ROOT)
        if "workload" in body:
            # Uploads are input artifacts, never interpreted as executable code.
            workload = Workload.parse(body["workload"])
            directory = self.tasks.directory / "inputs"
            directory.mkdir(exist_ok=True)
            path = directory / (secrets.token_hex(12) + ".json")
            write_json(path, asdict(workload))
            request = PredictionRequest.parse({**request.document(), "workload": str(path)}, ROOT)
        return request, request.document()


class Handler(BaseHTTPRequestHandler):
    server: Panel

    def log_message(self, format, *args):
        pass

    def send(
        self,
        value,
        status: int = 200,
        content_type: str = "application/json; charset=utf-8",
        filename: str | None = None,
    ):
        payload = (
            json.dumps(value, ensure_ascii=False, allow_nan=False).encode()
            if content_type.startswith("application/json")
            else value
        )
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self'; frame-ancestors 'none'; object-src 'none'",
        )
        if filename:
            self.send_header("Content-Disposition", f'attachment; filename="{filename}"')
        self.end_headers()
        self.wfile.write(payload)

    def do_GET(self):
        self.handle_request(False)

    def do_POST(self):
        self.handle_request(True)

    def download(self, path: Path):
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(path.stat().st_size))
        self.send_header("Content-Disposition", 'attachment; filename="result.json"')
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        with path.open("rb") as stream:
            shutil.copyfileobj(stream, self.wfile)

    def handle_request(self, mutation: bool):
        try:
            if self.headers.get("Host") != f"127.0.0.1:{self.server.server_port}":
                self.send({"error": "Use the displayed loopback URL"}, 403)
                return
            if self.headers.get("Origin") not in (None, self.server.origin):
                self.send({"error": "Cross-origin access is disabled"}, 403)
                return
            if mutation and not secrets.compare_digest(
                self.headers.get("X-Markov-Token", ""), self.server.token
            ):
                self.send({"error": "Refresh this panel before submitting"}, 403)
                return
            route = urlsplit(self.path).path
            if not mutation:
                self.get(route)
                return
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 16 * 1024**2:
                self.send(
                    {
                        "error": "JSON upload must be between 1 byte and 16 MiB; use a local workload path for larger inputs"
                    },
                    413,
                )
                return
            if self.headers.get_content_type() != "application/json":
                raise ValueError("Expected application/json")
            body = json.loads(self.rfile.read(length))
            if not isinstance(body, dict):
                raise ValueError("Expected a JSON object")
            self.post(route, body)
        except (OSError, ValueError, KeyError, TypeError, sqlite3.Error) as error:
            self.send({"error": str(error)}, 400)

    def get(self, route: str):
        if route in ("/", "/panel.js", "/panel.css"):
            name, kind = {
                "/": ("index.html", "text/html"),
                "/panel.js": ("panel.js", "text/javascript"),
                "/panel.css": ("panel.css", "text/css"),
            }[route]
            self.send((ASSETS / name).read_bytes(), content_type=kind + "; charset=utf-8")
        elif route == "/api/bootstrap":
            self.send(
                {
                    "token": self.server.token,
                    "directory": str(self.server.tasks.directory),
                    "request": PredictionRequest.load(ROOT / "configs/prediction.json").document(),
                }
            )
        elif route == "/api/tasks":
            self.send(self.server.tasks.list())
        elif route == "/api/assets":
            databases, bases = set(), set()
            for task in self.server.tasks.list():
                raw = read_json(self.server.tasks.path(task["id"]) / "request.json")
                if task["action"] != "maintain":
                    manifest = raw["base"].get("manifest")
                    state = Path(raw["execution"]["output"]) / "base/capture_state.json"
                    if manifest:
                        bases.add(manifest)
                    if state.exists() and read_json(state).get("manifest"):
                        bases.add(read_json(state)["manifest"])
                    database = raw.get("performance", {}).get("database")
                else:
                    database = raw["database"]
                if database:
                    databases.add(database)
            catalogs = []
            for path in sorted(databases):
                if Path(path).is_file():
                    store = PerformanceData(Path(path), readonly=True)
                    try:
                        catalogs.append({"path": path, **store.catalog()})
                    finally:
                        store.close()
            self.send({"bases": sorted(bases), "databases": catalogs})
        elif route.startswith("/api/tasks/"):
            parts = route.split("/")
            identity = parts[3]
            if len(parts) == 4:
                self.send(self.server.tasks.detail(identity))
                return
            export = parts[4]
            directory = self.server.tasks.path(identity)
            if export == "request":
                self.send(read_json(directory / "request.json"), filename="prediction-request.json")
            elif export == "policy":
                self.send(read_json(directory / "search.json"), filename="search-policy.json")
            else:
                path = self.server.tasks.result_path(identity)
                if path is None:
                    raise ValueError("This task has no result yet")
                if export == "result":
                    self.download(path)
                elif export == "csv":
                    result = self.server.tasks.detail(identity)["result"]
                    rows = result.get("candidates", result.get("cells", []))
                    fields = [
                        "name",
                        "status",
                        "formal_http_sec",
                        "predicted_sec",
                        "hourly_cost_units",
                        "window_cost_units",
                    ]
                    stream = io.StringIO()
                    writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
                    writer.writeheader()
                    writer.writerows(rows)
                    self.send(
                        stream.getvalue().encode("utf-8-sig"),
                        content_type="text/csv; charset=utf-8",
                        filename="results.csv",
                    )
                else:
                    self.send({"error": "Unknown export"}, 404)
        else:
            self.send({"error": "Not found"}, 404)

    def post(self, route: str, body: dict):
        if route == "/api/import":
            path = Path(body["path"]).resolve()
            if body.get("kind") == "policy":
                raw = read_json(path)
                SearchPolicy.parse(raw)
                self.send(
                    {
                        "policy": raw,
                        "request": PredictionRequest.load(
                            (path.parent / raw["prediction"]).resolve()
                        ).document(),
                    }
                )
            else:
                self.send({"request": PredictionRequest.load(path).document()})
        elif route == "/api/preview":
            request, document = self.server.prepare(body)
            self.send({"request": document, "plan": plan_prediction(request)})
        elif route == "/api/tasks":
            action = body["action"]
            if action == "maintain":
                # Maintenance owns its input paths and validation; preserve their document origin.
                path = Path(body["path"]).resolve()
                raw = maintenance_document(path)
            else:
                _, raw = self.server.prepare(body)
            self.send(self.server.tasks.create(action, raw, body["title"], body.get("policy")), 201)
        elif route.startswith("/api/tasks/"):
            _, _, _, identity, action = route.split("/")
            if action not in ("stop", "resume"):
                raise ValueError("Unknown task action")
            self.send(getattr(self.server.tasks, action)(identity))
        else:
            self.send({"error": "Not found"}, 404)


def serve(directory: Path, port: int) -> None:
    if not 0 <= port <= 65535:
        raise ValueError("Panel port must be between 0 and 65535")
    directory.mkdir(parents=True, exist_ok=True)
    with task_lock(directory / "panel.lock"), Panel(directory, port) as server:
        print(f"Markov panel: {server.origin} (local user only)", flush=True)
        try:
            server.serve_forever(poll_interval=0.5)
        except KeyboardInterrupt:
            print(
                "Panel stopped; running task workers continue. Restart with the same --directory.", flush=True
            )
