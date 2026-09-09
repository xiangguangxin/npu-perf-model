"""Hand constants and executable integration tests, independent of batch plotting."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from analytical_model import reference

binary=Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as directory:
    for mode, ticks in [('serial',363000),('paired',262000),('double',262000)]:
        p=Path(directory)/(mode+'.json')
        subprocess.run([str(binary),'--schedule',mode,'16','16','16','16','256','--output-json',str(p)],check=True,capture_output=True)
        r=json.loads(p.read_text())
        assert r['bytes']==768 and r['requests']==3 and r['executed_ops']==8192
        assert r['sim_ticks']==ticks,(mode,r['sim_ticks'])
        for key,value in reference(r).items():
            if key!='pe_ticks': assert r[key]==value,(key,r[key],value)
    for mode,ticks in [('serial',621000),('paired',419000)]:
        p=Path(directory)/'multi.json'
        subprocess.run([str(binary),'--schedule',mode,'16','32','16','16','256','--output-json',str(p)],check=True,capture_output=True)
        r=json.loads(p.read_text()); assert r['sim_ticks']==ticks,(mode,r['sim_ticks'])
    for args in [ ['--serial','--schedule','double'],['--schedule','bad'],['--schedule'],
                  ['--interconnect-bw','inf'],['--interconnect-bw','nan'],
                  ['--output-json',str(Path(directory)/'missing'/'out.json')]]:
        proc=subprocess.run([str(binary),*args],capture_output=True)
        assert proc.returncode!=0,args
print('MVP5 hand anchors, paired sequencing and CLI failure tests passed')
