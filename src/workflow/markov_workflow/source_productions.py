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
        jobs = self.plan["sources"]
        if (not len(jobs) == len(publication["productions"]) == len(publication["captures"])
                or self.jobs.keys() != {str(job["id"]) for job in jobs}):
            raise ValueError("source publication does not match the planned source jobs")
        self.paths = {job["id"]: collection / name for job, name in zip(jobs, publication["productions"], strict=True)}

        # Explicit observations keep their original owners, which may live outside this collection.
        self.captures = {job["id"]: collection / name for job, name in zip(jobs, publication["captures"], strict=True)}

    def prepare(self, source, *, modeled=False):
        primary = SourcePlanning.primary(self.plan, source)
        local_sources = [job["id"] for job in self.plan["sources"]
                         if any(consumer["source"] == primary for consumer in job.get("local_consumers", []))]
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
        document = SGLangTask(productions[0], productions[1:]).document(source, modeled=modeled)
        return templates, productions[0], document
