"""Check explicit source/target products without running a simulated controller."""

import argparse
import json
from pathlib import Path

from markov_adapters.artifacts import write_json
from markov_workflow.adapters.sglang import SGLangTask

from matrix import MatrixRun


class LocalMatrixRun(MatrixRun):
    def __init__(self, root, matrix):
        super().__init__(root, matrix)
        self.local_binary = self.root / self.matrix['local_binary']
        self.local_batch = self.matrix['local_batch']

    def process(self, name, command):

        # A repair batch owns its command records; successful production steps still reuse their formal task owner.
        return super().process(f'{self.local_batch}/{name}', command)

    def expand(self, identity, rank, libraries, production, actions, expanded):
        inputs, bundle = actions / f'rank-{rank}.json', actions / f'rank-{rank}-sources.json'
        if not bundle.exists():
            write_json(bundle, [f'/libraries/{index}.json' for index in range(len(libraries))], replace=False)
        command = ['docker', 'run', '--rm', '--runtime', 'runc', '--network', 'none', '--entrypoint', '/local-graphs',
                   '--mount', f'type=bind,src={self.local_binary},dst=/local-graphs,readonly']
        export = command + ['--mount', f'type=bind,src={production / "work" / f"{rank}-graph.json"},dst=/graph.json,readonly',
                            '--mount', f'type=bind,src={actions},dst=/actions', self.images['modeling'],
                            'actions', '/graph.json', f'/actions/rank-{rank}.json']
        worker = {'rank': rank, 'actions_exit': self.process(f'{identity}/{rank}-actions', export)}
        if worker['actions_exit'] != 0:
            return worker

        # The generator gets invocation inputs and source candidates; target graphs stay in the extraction/evaluation lane.
        generate = command + ['--mount', f'type=bind,src={inputs},dst=/inputs.json,readonly',
                              '--mount', f'type=bind,src={bundle},dst=/sources.json,readonly',
                              '--mount', f'type=bind,src={expanded},dst=/output']
        for index, path in enumerate(libraries):
            generate.extend(['--mount', f'type=bind,src={path},dst=/libraries/{index}.json,readonly'])
        generate.extend([self.images['modeling'], 'expand', '/sources.json', '/inputs.json', f'/output/rank-{rank}.jsonl'])
        worker['expand_exit'] = self.process(f'{identity}/{rank}-expand', generate)
        return worker

    def compare_case(self, case, paths, libraries):
        identity = case['id']
        row = {'id': identity}
        truth, status = self.read_truth(case)
        if truth is None:
            return row | {'status': 'target-' + status}
        native, production = truth
        row['target_evidence'] = case['source_evidence']

        primary = case['source_domain']
        order = [primary, *(job for job in libraries if job != primary)]
        candidates = [path for job in order for path in paths[job]]
        task = SGLangTask(libraries[primary], [libraries[job] for job in order[1:]])

        # Use the formal source assembly: role tables belong to instances, while participant ranks span the deployment.
        document = task.document(json.loads((self.root / case['prediction_input']).read_text()))
        participants = {worker['rank']: worker['template'] for instance in document['instances'] for worker in instance['participants']}
        work = self.root / 'work' / self.name / 'local-batches' / self.local_batch / identity
        expanded, actions = work / 'local', work / 'actions'
        for directory in (expanded, actions):
            directory.mkdir(parents=True, exist_ok=True)
        row['workers'] = []
        for rank in json.loads((production / 'result.json').read_text())['observed_roles']:
            selected = participants[rank]
            sources = [candidates[selected], *(path for index, path in enumerate(candidates) if index != selected)]
            row['workers'].append(self.expand(identity, rank, sources, production, actions, expanded))
        output = self.root / 'results' / self.name / 'local-batches' / self.local_batch / identity
        self.evaluate(f'{identity}/local-compare', 'local_graphs.py', '--capture', native, '--graphs', production / 'work',
                      '--expanded', expanded, '--output', output / 'local-comparison')
        report = output / 'local-comparison/local-graphs.json'
        if not report.exists():
            return row | {'status': 'comparison-error'}
        comparison = json.loads(report.read_text())
        return row | {'status': comparison['status'], 'counts': comparison['counts'], 'report': str(report.relative_to(self.root))}

    def compare_domain(self, domain):
        source = self.sources[domain['id']]
        if source['status'] != 'source_scope_checked':
            rows = [{'id': case['id'], 'status': 'source-prerequisite-unqualified'} for case in domain['targets']]
        else:
            paths = {job['id']: [item['path'] for item in job['libraries']] for job in source['jobs']}
            libraries = {job: [json.loads(Path(path).read_text()) for path in files] for job, files in paths.items()}
            rows = [self.compare_case(case, paths, libraries) for case in domain['targets']]
        write_json(self.root / 'results' / self.name / 'local-batches' / self.local_batch / domain['id'] / 'local.json',
                   {'targets': rows}, indent=2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--matrix', type=Path, required=True)
    parser.add_argument('--domain')
    args = parser.parse_args()
    LocalMatrixRun(args.root, args.matrix).run('compare', args.domain)


if __name__ == '__main__':
    main()
