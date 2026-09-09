"""Validate exact counts, restricted timing, bounds, and experimental controls."""
import argparse
import csv
import json
from pathlib import Path
from analytical_model import reference


def validate(rows, destination):
    checks = []
    def check(case, metric, actual, expected, ok):
        checks.append(dict(case=case, metric=metric, actual=actual, expected=expected, passed=bool(ok)))
    for r in rows:
        check(r['case_id'], 'schema', r['schema_version'], 1, r['schema_version'] == 1)
        check(r['case_id'], 'positive runtime', r['sim_ticks'], '>0', r['sim_ticks'] > 0)
        ref = reference(r)
        for k, expected in ref.items():
            if k == 'pe_ticks':
                check(r['case_id'], 'PE lower bound', r['sim_ticks'], expected, r['sim_ticks'] >= expected)
            else:
                check(r['case_id'], k, r[k], expected, r[k] == expected)
        bound = r['bytes'] / (r['hbm_bw_GBps'] * 1e9)
        check(r['case_id'], 'HBM lower bound', r['sim_s'], bound,
              r['sim_s'] + r['time_resolution_s'] >= bound)
        check(r['case_id'], 'grants conserved', sum(r['grants']), r['requests'], sum(r['grants']) == r['requests'])
    base = next(r for r in rows if r['case_id'] == 'v_single_serial')
    check('hand_anchor', '363 ns', base['sim_ticks'], 363000, base['sim_ticks'] == 363000)
    paired = next(r for r in rows if r['case_id'] == 'v_single_paired')
    check('hand_anchor', 'paired 262 ns', paired['sim_ticks'], 262000, paired['sim_ticks'] == 262000)
    for group in ('E1', 'E2', 'E5'):
        selected = [r for r in rows if r['group'] == group]
        buckets = {}
        for r in selected:
            key = (r['M'],r['K'],r['N'],r['array_n'])
            buckets.setdefault(key, []).append(r)
        for bucket in buckets.values():
            for r in bucket:
                for k in ('bytes', 'executed_ops', 'requests') + (('sim_ticks',) if group == 'E5' else ()):
                    check(r['case_id'], group+' invariant '+k,r[k],bucket[0][k],r[k]==bucket[0][k])
    first = next(r for r in rows if r['case_id'] == 'v_multi_double')
    repeat = next(r for r in rows if r['case_id'] == 'v_repeat')
    for k in ('bytes','requests','executed_ops','sim_ticks','grants','avg_queue_ns','stall_ns'):
        check('determinism', k, repeat[k], first[k], repeat[k] == first[k])
    with Path(destination).open('w', newline='') as f:
        w=csv.DictWriter(f, fieldnames=checks[0].keys()); w.writeheader(); w.writerows(checks)
    failures = [c for c in checks if not c['passed']]
    if failures:
        raise RuntimeError(json.dumps(failures, indent=2))
    return len(checks)

if __name__ == '__main__':
    p=argparse.ArgumentParser(); p.add_argument('directory',type=Path); a=p.parse_args()
    print(validate(json.loads((a.directory/'results.json').read_text()),a.directory/'validation.csv'), 'checks passed')
