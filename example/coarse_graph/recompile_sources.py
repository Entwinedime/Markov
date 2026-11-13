"""Recompile explicit source graph facts without native collection, SDK export or target input."""

import argparse
import copy
import fcntl
import hashlib
import json
from pathlib import Path

from markov_adapters.artifacts import write_json

from matrix import MatrixRun


class SourceRecompilation(MatrixRun):
    def __init__(self, root, matrix):
        super().__init__(root, matrix)
        self.catalog = json.loads((self.root / self.matrix['source_catalog']).read_text())
        self.compiler = self.root / self.matrix['template_binary']
        self.batch = self.matrix['local_batch']
        self.recompiled = {}

    def compile_job(self, domain, job):
        key = (job['capture'], tuple((item['rank'], item['sha256']) for item in job['libraries']))
        if key in self.recompiled:
            return copy.deepcopy(self.recompiled[key])
        identity = f'{self.batch}/sources/{domain}/job-{job["id"]}'
        output = self.root / 'work' / self.name / 'source-batches' / self.batch / domain / str(job['id'])
        (output / 'work').mkdir(parents=True, exist_ok=True)
        ranks = [item['rank'] for item in job['libraries']]
        inputs = output / 'inputs.json'
        if not inputs.exists():
            write_json(inputs, [f'/production/work/{rank}.json' for rank in ranks], replace=False)
        command = ['docker', 'run', '--rm', '--runtime', 'runc', '--network', 'none', '--entrypoint', '/compiler',
                   '--mount', f'type=bind,src={self.compiler},dst=/compiler,readonly',
                   '--mount', f'type=bind,src={output},dst=/production']
        for rank in ranks:
            graph = Path(job['graphs']) / f'{rank}-graph.json'
            compile_ = command + ['--mount', f'type=bind,src={graph},dst=/graph.json,readonly', self.images['modeling'],
                                  'templates', '/graph.json', f'/production/work/{rank}.json']
            if self.process(f'{identity}/{rank}-compile', compile_) != 0:
                return {'status': 'recompilation-failed', 'production': str(output)}
        link = command + [self.images['modeling'], 'link-templates', '/production/inputs.json', '/production/assets']
        if self.process(f'{identity}/link', link) != 0:
            return {'status': 'recompilation-failed', 'production': str(output)}

        # Successful compilation publishes assets; independent review decides their validation qualification.
        libraries = []
        for previous in job['libraries']:
            path = output / 'assets' / f'{previous["rank"]}.json'
            libraries.append({'rank': previous['rank'], 'path': str(path),
                              'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'previous': previous['path']})
        records = [json.loads(p.read_text()) for p in (self.steps / identity).rglob('*.step.json')]
        spent = sum(row['charges']['processing_seconds'] for row in records)
        write_json(output / 'result.json', {
            'templates': [f'assets/{rank}.json' for rank in ranks], 'observed_roles': ranks,
            'postprocessing_ns': round(spent * 1_000_000_000), 'qualification': 'compiled_source_templates',
            'source_graphs': job['graphs'], 'compiler': str(self.compiler),
        }, indent=2)
        result = {'status': 'compiled_source_templates', 'production': str(output), 'libraries': libraries}
        self.recompiled[key] = result
        return result

    def run(self):
        if self.matrix['status'] != 'frozen':
            raise ValueError('source compilation matrix is not frozen')
        with (self.steps / f'.{self.batch}-sources.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            for domain in self.catalog['domains']:
                if domain['status'] != 'source_scope_checked':
                    continue
                for job in domain['jobs']:
                    result = self.compile_job(domain['id'], job)
                    job['previous_production'] = job['production']
                    job.update(result)
                domain['status'] = ('compiled_source_templates' if all(job['status'] == 'compiled_source_templates' for job in domain['jobs'])
                                    else 'source-recompilation-unqualified')
        failed = any(domain['status'] == 'source-recompilation-unqualified' for domain in self.catalog['domains'])
        self.catalog['source_evidence_summary'] = self.catalog.pop('summary')
        self.catalog['compilation'] = {'compiler': str(self.compiler), 'source_catalog': self.matrix['source_catalog'],
                                       'status': 'unqualified' if failed else 'compiled', 'unique_compilations': len(self.recompiled)}
        output = self.root / 'results' / self.name / 'source-batches' / self.batch / 'compiled-source-catalog.json'
        write_json(output, self.catalog, indent=2)
        print(f'Catalog: {output}', flush=True)
        return int(failed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--matrix', type=Path, required=True)
    args = parser.parse_args()
    raise SystemExit(SourceRecompilation(args.root, args.matrix).run())


if __name__ == '__main__':
    main()
