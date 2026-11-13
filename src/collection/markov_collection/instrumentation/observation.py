"""Walk common observation references without depending on a framework or device runtime."""


class ObservationReferences:
    def __init__(self, observation):
        self.observation = observation

    def __iter__(self):
        for edge in self.observation["dependencies"]:
            yield edge["source"]
            yield edge["target"]

        for scope in self.observation["scopes"]:
            for port in scope["ports"]:
                yield port["at"]
                if "wait" in port:
                    yield port["wait"]["begin"]
                if "rendezvous" in port:
                    yield port["rendezvous"]

            for field in ("kv_accesses", "slot_accesses"):
                for access in scope[field]:
                    yield access["start"]
                    yield access["finish"]
