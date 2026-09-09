"""Run fixed experiments, preserve provenance/logs, fail closed on missing results."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import platform
from validate_results import validate

ROOT=Path(__file__).resolve().parents[2]

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--binary',type=Path,default=ROOT/'build/npu_sim')
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--cases',type=Path,default=ROOT/'experiments/mvp5/cases.json')
    args=p.parse_args(); out=args.output.resolve(); out.mkdir(parents=True,exist_ok=False)
    raw=out/'raw'; raw.mkdir()
    binary=args.binary.resolve()
    def git(*cmd): return subprocess.check_output(['git',*cmd],cwd=ROOT,text=True)
    patch=git('diff','HEAD')
    (out/'source.patch').write_text(patch)
    # Source hashes identify dirty and untracked implementation files as well.
    files=[p for base in ('include','src','scripts/mvp5','experiments/mvp5') for p in (ROOT/base).rglob('*')
           if p.is_file() and '__pycache__' not in p.parts]
    files += [ROOT/'CMakeLists.txt']
    manifest=dict(commit=git('rev-parse','HEAD').strip(),status=git('status','--short'),
                  binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  source_sha256={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
                  runner_command=sys.argv, python_version=platform.python_version(), platform=platform.platform(),
                  cmake_cache=(binary.parent/'CMakeCache.txt').read_text() if (binary.parent/'CMakeCache.txt').exists() else None,
                  timeout_s=60, cases=json.loads(args.cases.read_text()), runs=[])
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
    rows=[]
    for case in manifest['cases']:
        result=raw/(case['id']+'.json')
        cmd=[str(binary),*case['args'],'--output-json',str(result)]
        try:
            proc=subprocess.run(cmd,cwd=ROOT,capture_output=True,text=True,timeout=60)
            (raw/(case['id']+'.stdout')).write_text(proc.stdout)
            (raw/(case['id']+'.stderr')).write_text(proc.stderr)
            manifest['runs'].append(dict(case_id=case['id'],command=cmd,returncode=proc.returncode))
            if proc.returncode: raise RuntimeError(f"{case['id']} exited {proc.returncode}")
            r=json.loads(result.read_text())
            r.update(case_id=case['id'],group=case['group'])
            rows.append(r)
        except subprocess.TimeoutExpired as e:
            (raw/(case['id']+'.stdout')).write_text((e.stdout or b'').decode(errors='replace') if isinstance(e.stdout, bytes) else (e.stdout or ''))
            (raw/(case['id']+'.stderr')).write_text(str(e))
            manifest['runs'].append(dict(case_id=case['id'],command=cmd,error='timeout'))
            raise
        except Exception as e:
            manifest['runs'].append(dict(case_id=case['id'], command=cmd, error=str(e)))
            raise
        finally:
            (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
    (out/'results.json').write_text(json.dumps(rows,indent=2))
    with (out/'results.csv').open('w',newline='') as f:
        w=csv.DictWriter(f,fieldnames=rows[0].keys()); w.writeheader(); w.writerows(rows)
    n=validate(rows,out/'validation.csv')
    print(f'{len(rows)} runs; {n} checks passed; {out}')

if __name__=='__main__': main()
