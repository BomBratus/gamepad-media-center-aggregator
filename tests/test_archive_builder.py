#!/usr/bin/env python3
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='gmca-host-builder-') as directory:
    root = Path(directory)
    fixtures = {
        'title.ratings.tsv.gz': 'tconst\taverageRating\tnumVotes\ntt1\t8.0\t1000\n',
        'title.basics.tsv.gz': 'tconst\ttitleType\tprimaryTitle\toriginalTitle\tisAdult\tstartYear\tendYear\truntimeMinutes\tgenres\ntt1\tmovie\tTest\tTest\t0\t2020\t\\N\t90\tDrama\n',
        'title.akas.tsv.gz': 'titleId\tordering\ttitle\tregion\tlanguage\ttypes\tattributes\tisOriginalTitle\n',
    }
    for name, body in fixtures.items():
        with gzip.open(root / name, 'wt') as output:
            output.write(body)
    path = root / 'index.sqlite'
    built = json.loads(subprocess.check_output([sys.argv[1], str(path), str(root)], text=True))
    assert built['schema'] == 2 and built['titles'] == built['browsable'] == 1
    assert built['bytes'] == path.stat().st_size
    assert built['sha256'] == hashlib.sha256(path.read_bytes()).hexdigest()
    checked = json.loads(subprocess.check_output([sys.argv[1], '--validate', str(path)], text=True))
    assert checked == built
    assert all((root / name).exists() for name in fixtures)
    invalid = root / 'invalid.sqlite'
    invalid.write_bytes(b'corrupt')
    assert subprocess.run([sys.argv[1], '--validate', str(invalid)], capture_output=True).returncode != 0
    print('host Archive builder tests passed')
