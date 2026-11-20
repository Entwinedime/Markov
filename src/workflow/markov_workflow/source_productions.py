"""Resolve published source collections for prediction and cost acquisition."""

import json

from markov_workflow.adapters.sglang import SGLangTask
from markov_workflow.adapters.sglang_planning import SourcePlanning


class SourceProductions:
    """Own the published source plan, native inputs and primary/local assembly."""

    def __init__(self, collection):
        collection = collection.resolve(strict=True)
        inputs = json.loads((collection / "input.json").read_text())
        self.plan, self.jobs = inputs["plan"], inputs["jobs"]
        publication = json.loads((collection / "result.json").read_text())
        self.unavailable = publication.get("unavailable", {})
        planned = {str(job["id"]) for job in self.plan["sources"]}
        jobs = [job for job in self.plan["sources"] if str(job["id"]) not in self.unavailable]
        if (not len(jobs) == len(publication["productions"]) == len(publication["captures"])
                or self.jobs.keys() != planned or self.unavailable.keys() - planned):
            raise ValueError("source publication does not match the planned source jobs")
        self.paths = {job["id"]: collection / name for job, name in zip(jobs, publication["productions"], strict=True)}

        # Explicit observations keep their original owners, which may live outside this collection.
        self.captures = {job["id"]: collection / name for job, name in zip(jobs, publication["captures"], strict=True)}

    def select(self, source):
        return SourcePlanning.primary(self.plan, source, available=self.paths)

    def failure(self, source):
        primary = SourcePlanning.primary(self.plan, source, available={int(key) for key in self.unavailable})
        if primary is None:
            return {"status": "source_missing", "message": "target implementation has no planned primary source"}
        return {"job": primary, **self.unavailable[str(primary)]}

    def prepare(self, source):
        primary = self.select(source)
        if primary is None:
            raise ValueError("target implementation has no published primary source")
        local_sources = [job["id"] for job in self.plan["sources"]
                         if job["id"] in self.paths
                         and any(consumer["source"] == primary for consumer in job.get("local_consumers", []))]
        templates = []
        productions = []
        for identity in [primary, *local_sources]:
            directory = self.paths[identity].resolve(strict=True)
            publication = json.loads((directory / "result.json").read_text())
            libraries = []
            for name in publication["templates"]:
                path = (directory / name).resolve(strict=True)
                if not path.is_relative_to(directory):
                    raise ValueError("template publication refers outside its production directory")
                templates.append(path)
                libraries.append(json.loads(path.read_text()))
            productions.append(libraries)
        document = SGLangTask(productions[0], productions[1:]).document(source)
        return templates, productions[0], document
