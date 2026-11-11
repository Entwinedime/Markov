"""Transport declared expressions through semantic call arguments, never through observed values."""

from dataclasses import dataclass


@dataclass(frozen=True)
class Parameter:
    scope: int
    name: str


class ScopeParameters:
    def __init__(self, scopes) -> None:
        self.scopes = scopes

    def resolve(self, expression, scope):
        if isinstance(expression, int):
            return expression
        if isinstance(expression, str):
            record = self.scopes[scope]
            if expression in record["constants"]:
                return record["constants"][expression]
            if expression in record["arguments"]:
                return self.resolve(record["arguments"][expression], record["parent"])
            if expression not in record["parameters"]:
                raise ValueError(f"shape references undeclared parameter: {scope}:{expression}")
            return Parameter(scope, expression)
        return expression[0], *(self.resolve(value, scope) for value in expression[1:])

    def bind(self, rule, scope):
        return tuple(self.resolve(axis, scope) for axis in rule)

    def local(self, rule, scope):
        names = {}
        for name in self.scopes[scope]["parameters"]:
            expression = self.resolve(name, scope)

            # A literal dimension must not become a variable merely because its observed value matches.
            if not isinstance(expression, int):
                names.setdefault(expression, name)
        return [self.render(axis, names) for axis in rule]

    def render(self, expression, names):
        if isinstance(expression, int):
            return expression
        if expression in names:
            return names[expression]
        if isinstance(expression, Parameter):
            raise ValueError(f"producer shape parameter is not bound in the consumer: {expression}")
        return [expression[0], *(self.render(value, names) for value in expression[1:])]
