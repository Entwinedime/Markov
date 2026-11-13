"""Run a frozen independent matrix through formal source acquisition and prediction.

Invoke from the repository root inside the control image, with
PYTHONPATH=src/workflow:src/adapters:src/collection:src/workload.
Capture and compare run separately; comparison consumes source_catalog and explicit target observation/production/source_evidence.
Terminal failures are retained without retries or implicit target production.
"""

import argparse
import fcntl
import json
import time
from pathlib import Path

from markov_adapters.artifacts import write_json
from markov_workflow.capture import CaptureProcess
from markov_workflow.containers import OwnedContainers
from markov_workflow.limits import OperationLimit

from run import WorkflowCommands


class MatrixRun:
    def __init__(self, root, matrix):
        self.root = root.resolve(strict=True)
        self.repository = Path(__file__).resolve().parents[2]
        self.matrix = json.loads(matrix.read_text())
        self.name = self.matrix['id']
        self.images = json.loads((matrix.parent / 'environment.json').read_text())['images']
        self.binary = (matrix.parent / 'markov').resolve(strict=True)
        self.budget = self.matrix['budget']
        self.commands = WorkflowCommands(self.repository, self.images)
        self.steps = self.root / 'logs' / self.name
        self.steps.mkdir(parents=True, exist_ok=True)
        self.storage = OperationLimit(self.root, self.budget['retained_bytes'])

    def collection(self, domain):
        return self.root / 'sources' / self.name / domain['id']

    def step(self, name, command, charges, timeout, acquisition_status=None, capture_status=None):
        container = f"markov-matrix-{self.name}-{name.replace('/', '--')}"
        command = command[:2] + ['--name', container] + command[2:]
        path = self.steps / f'{name}.step.json'
        if path.exists():
            record = json.loads(path.read_text())
            if record['command'] != command:
                raise ValueError(f'frozen step command changed: {name}')
            if record['exit_code'] is None:
                raise RuntimeError(f'unfinished step requires process/container inspection: {path}')
            return record['exit_code']

        # Acquisition reserves both budgets; one shared lock also protects the independent CPU lane.
        with (self.steps / '.budget.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            records = [json.loads(p.read_text()) for p in self.steps.rglob('*.step.json')]
            for category, amount in charges.items():
                used = sum(record['charges'].get(category, 0) for record in records)
                if used + amount > self.budget[category]:
                    raise RuntimeError(f'{category} allowance cannot reserve operation {name}')
            self.storage.check()
            path.parent.mkdir(parents=True, exist_ok=True)
            record = {'command': command, 'charges': charges, 'charge_basis': 'reserved', 'exit_code': None}
            write_json(path, record, indent=2, replace=False)

        print(f'{name}: starting', flush=True)
        started = time.monotonic()
        try:
            with path.with_suffix('.log').open('x') as log:
                code = self.storage.run(command, log, timeout)
        except BaseException as error:
            record['error'] = f'{type(error).__name__}: {error}'
            write_json(path, record, indent=2)
            OwnedContainers([container]).close()
            raise

        elapsed = time.monotonic() - started
        if acquisition_status is None and capture_status is None:
            category, = (key for key in charges if key.endswith('_seconds'))
            record['charges'][category] = elapsed
            record['charge_basis'] = 'elapsed'
        elif acquisition_status is not None and acquisition_status.exists():
            status = json.loads(acquisition_status.read_text())
            native = status['native_charged_ns'] / 1_000_000_000
            record['charges'] = {
                'native_seconds': native,
                'processing_seconds': max(status['postprocessing_ns'] / 1_000_000_000, elapsed - native),
                'native_attempts': status['native_attempts'],
            }
            record['charge_basis'] = 'owner'
        elif capture_status is not None and capture_status.exists():
            attempts = json.loads(capture_status.read_text())['attempts']
            record['charges'] = {'native_seconds': max(elapsed, sum(row['charged_ns'] for row in attempts) / 1_000_000_000),
                                 'native_attempts': len(attempts)}
            record['charge_basis'] = 'owner'

        # Missing owner status keeps its reservation; a failed launcher cannot manufacture unused credit.
        record.update(exit_code=code, elapsed_seconds=elapsed)
        write_json(path, record, indent=2)
        print(f'{name}: exit {code}', flush=True)
        return code

    def process(self, name, command):
        allowance = self.budget['operation_seconds']
        return self.step(name, command, {'processing_seconds': allowance}, allowance)

    def evaluate(self, name, script, *arguments):
        command = [
            'docker', 'run', '--rm', '--runtime', 'runc', '--network', 'none',
            '--mount', f'type=bind,src={self.repository},dst={self.repository}',
            '--env', f'PYTHONPATH={self.repository}/src/collection:{self.repository}/src/adapters',
            '--workdir', str(self.repository), '--entrypoint', 'timeout', self.images['control'],
            '--signal=KILL', str(self.budget['task_seconds']), 'python3',
            str(self.repository / 'example/coarse_graph' / script), *map(str, arguments),
        ]
        return self.process(name, command)

    def acquire(self, domain):
        plan = self.root / domain['plan']
        jobs = json.loads((plan / 'plan.json').read_text())['native_jobs']
        allowance = jobs * self.budget['task_seconds']
        output = self.collection(domain)
        command = self.commands.command(
            'acquire-sources', '--plan', plan, '--deployment', self.root / domain['deployment'], '--output', output,
            '--binary', self.binary, '--source-image', self.images['source'], '--modeling-image', self.images['modeling'],
            '--timeout', self.budget['task_seconds'], '--native-budget-seconds', allowance,
            '--postprocessing-budget-seconds', allowance, '--attempt-limit', jobs,
            '--storage-bytes', self.budget['retained_bytes'], '--resource-trace', '--keep-intermediates',
            *(['--observations', self.root / domain['observations']] if 'observations' in domain else []),
            model=json.loads((self.root / domain['exploration']).read_text())['model']['path'],
        )
        reserve = jobs * self.budget['operation_seconds']
        return self.step(f"{domain['id']}/sources", command,
                         {'native_seconds': reserve, 'processing_seconds': reserve, 'native_attempts': jobs},
                         2 * reserve, output / 'status.json')

    def capture(self, name, input_path, deployment, output):
        command = self.commands.command(
            'capture', '--input', input_path, '--deployment', deployment, '--output', output,
            '--source-image', self.images['source'], '--timeout', self.budget['task_seconds'],
            '--budget-seconds', self.budget['task_seconds'], '--attempt-limit', 1, '--resource-trace',
            model=json.loads(input_path.read_text())['model']['path'],
        )
        allowance = self.budget['operation_seconds']
        return self.step(name, command, {'native_seconds': allowance, 'native_attempts': 1}, allowance,
                         capture_status=output / 'execution.json')

    def read_truth(self, case):
        if 'native_prerequisite' in case:
            return None, 'native-prerequisite-unresolved'
        native = CaptureProcess.published(self.root / case['observation'],
                                          json.loads((self.root / case['capture_input']).read_text()))
        production = (self.root / case['production']).resolve(strict=True)
        state = json.loads((production / 'production.json').read_text())
        publication = json.loads((production / 'result.json').read_text())
        evidence = json.loads((self.root / case['source_evidence']).read_text())

        # A qualified target is a read-only input, independent of the current predictor executable.
        if (state['running'] is not None or state['inputs']['capture_path'] != str(native)
                or publication['qualification'] != 'observed_coarse_graphs'
                or Path(evidence['production']).resolve() != production or Path(evidence['capture']).resolve() != native
                or sorted(publication['observed_roles']) != sorted(row['rank'] for row in evidence['ranks'])):
            raise ValueError(f"target publication/evidence ownership differs: {case['id']}")
        view = {'native_case_status': evidence['native_case']['status'], 'ranks': evidence['ranks']}
        return ((native, production), 'published') if self.mapping_verified(view) else (None, 'mapping-unverified')

    @staticmethod
    def mapping_verified(report):
        ranks = report.get('ranks', ())
        return (report.get('native_case_status') in {'consistent', 'incomplete'} and bool(ranks)
                and all(row['mapping'] == 'consistent' for row in ranks))

    def capture_domain(self, domain, include_sources):
        name = domain['id']
        rows = []
        if include_sources:
            rows = ([{'id': name, 'status': 'native-prerequisite-unresolved'}] if 'native_prerequisite' in domain
                    else [{'id': name, 'exit_code': self.acquire(domain)}])

        # Source eligibility never decides whether an independent target can be observed.
        for case in domain['targets']:
            identity = case['id']
            if 'native_prerequisite' in case:
                rows.append({'id': identity, 'status': 'native-prerequisite-unresolved'})
                continue
            if 'observation' in case:
                capture = self.root / case['observation']
                CaptureProcess.published(capture, json.loads((self.root / case['capture_input']).read_text()))
                rows.append({'id': identity, 'exit_code': 0, 'status': 'reused', 'capture': str(capture)})
                continue
            rows.append({'id': identity, 'exit_code': self.capture(
                f'{identity}/target-capture', self.root / case['capture_input'], self.root / domain['deployment'],
                self.root / 'targets' / self.name / identity,
            )})
        write_json(self.root / 'results' / self.name / name / 'capture.json', rows, indent=2)

    def compare_domain(self, domain):
        source = self.sources[domain['id']]
        rows = []
        for case in domain['targets']:
            identity = case['id']
            output = self.root / 'results' / self.name / identity
            row = {'id': identity}
            if source['status'] != 'source_scope_checked':
                rows.append(row | {'status': 'source-prerequisite-unqualified'})
                continue
            truth, status = self.read_truth(case)
            if truth is None:
                rows.append(row | {'status': 'target-' + status})
                continue
            native, production = truth
            row['target_evidence'] = case['source_evidence']

            # Only independently extracted time conditions enter prediction; target bodies and edges stay in evaluation.
            self.evaluate(f'{identity}/timing', 'execution_inputs.py', '--capture', native, '--graphs', production / 'work',
                          '--output', output / 'timing')
            timing_report = output / 'timing/execution-inputs-report.json'
            row['timing'] = json.loads(timing_report.read_text())['status'] if timing_report.exists() else 'evaluation-error'
            timing = output / 'timing/execution-inputs.json'
            if not timing.exists():
                rows.append(row | {'status': 'timing-unavailable'})
                continue

            primary = case['source_domain']
            productions = {job['id']: job['production'] for job in source['jobs']}
            arguments = ['--production', productions[primary]]
            arguments.extend(value for job, path in productions.items() if job != primary for value in ('--local-production', path))
            prediction = output / 'prediction'
            command = self.commands.command(
                'predict', *arguments, '--input', self.root / case['prediction_input'], '--output', prediction,
                '--binary', self.binary, '--modeling-image', self.images['modeling'], '--control-image', self.images['control'],
                '--timeout', self.budget['task_seconds'], '--execution-trace', '--execution-inputs', timing,
            )
            row['prediction_exit'] = self.process(f'{identity}/predict', command)
            execution = prediction / ('execution.jsonl' if row['prediction_exit'] == 0 else 'execution.partial.jsonl')
            if not execution.exists():
                rows.append(row | {'status': 'prediction-no-trace'})
                continue
            row['execution_trace'] = str(execution.relative_to(self.root))

            # A stopped run still has a checked prefix; compare it without inventing an unexecuted tail.
            self.evaluate(f'{identity}/compare', 'compare_actions.py', '--capture', native, '--graphs', production / 'work',
                          '--execution', execution, '--output', output / 'comparison')
            report = output / 'comparison/actions.json'
            if report.exists():
                document = json.loads(report.read_text())
                row.update(status='compared', comparison=document['status'], counts=document['counts'],
                           actions_compared=document['actions_compared'], report=str(report.relative_to(self.root)))
            else:
                row['status'] = 'comparison-error'
            rows.append(row)
        write_json(self.root / 'results' / self.name / domain['id'] / 'comparison.json',
                   {'source_status': source['status'], 'targets': rows}, indent=2)

    def run(self, phase, domain_name):
        if self.matrix['status'] != 'frozen':
            raise ValueError('matrix is not frozen; no execution started')
        if phase == 'compare':
            catalog = json.loads((self.root / self.matrix['source_catalog']).read_text())
            self.sources = {domain['id']: domain for domain in catalog['domains']}
        domains = self.matrix['domains']
        if domain_name is not None:
            domains = [next(domain for domain in domains if domain['id'] == domain_name)]

        with (self.steps / f'.{phase}.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            for domain in domains:
                if phase in {'capture', 'targets'}:
                    self.capture_domain(domain, phase == 'capture')
                else:
                    self.compare_domain(domain)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True, help='existing window data root')
    parser.add_argument('--matrix', type=Path, required=True, help='frozen matrix beside environment.json and markov')
    parser.add_argument('--phase', choices=['capture', 'targets', 'compare'], required=True)
    parser.add_argument('--domain', help='one named domain; omit to process the complete matrix')
    arguments = parser.parse_args()
    MatrixRun(arguments.root, arguments.matrix).run(arguments.phase, arguments.domain)


if __name__ == '__main__':
    main()
