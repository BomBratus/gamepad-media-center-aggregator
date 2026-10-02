"""Protocol contracts needed by the real Stremio backend, without external I/O."""
import json
from pathlib import Path
import tempfile
import unittest
import urllib.error
import urllib.request
import urllib.error
from fixtures import FixtureServer


class FixtureContracts(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        media = Path(self.tmp.name) / 'media.mp4'
        media.write_bytes(b'0123456789')
        self.fixture = FixtureServer(media).start()

    def tearDown(self):
        self.fixture.close()
        self.tmp.cleanup()

    def get(self, path):
        with urllib.request.urlopen(self.fixture.base + path, timeout=3) as reply:
            return json.load(reply)

    def post(self, endpoint, body):
        request = urllib.request.Request(self.fixture.base + '/api/' + endpoint,
            json.dumps(body).encode(), {'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=3) as reply:
            return json.load(reply)

    def test_series_routing_and_catalog_path_extras(self):
        manifest = self.get('/manifest.json')
        self.assertTrue('tt9000010'.startswith(manifest['idPrefixes'][0]))
        self.assertTrue(any(e['name'] == 'search' for e in manifest['catalogs'][0]['extra']))
        meta = self.get('/meta/series/tt9000010.json')['meta']
        self.assertEqual([v['id'] for v in meta['videos']],
            ['tt9000010:1:1', 'tt9000010:1:2', 'tt9000010:2:1', 'tt9000010:2:2'])
        catalog = self.get('/catalog/movie/tvtest/search=Fixture%20Movie%20Two&genre=Comedy.json')
        self.assertEqual([m['id'] for m in catalog['metas']], ['tt9000002'])
        self.assertEqual(self.get('/catalog/movie/tvtest/skip=3.json')['metas'], [])

    def test_isolated_datastore_and_media_ranges(self):
        before = self.post('datastoreGet', {'authKey': 'fixture'})['result']
        self.assertEqual(before[1]['state']['videoId'], 'tt9000010:1:1')
        change = dict(before[0], state=dict(before[0]['state'], flaggedWatched=1))
        self.post('datastorePut', {'changes': [change]})
        self.assertEqual(self.post('datastoreGet', {})['result'][0]['state']['flaggedWatched'], 1)
        request = urllib.request.Request(self.fixture.base + '/video.mp4?source=private', headers={'Range': 'bytes=2-4'})
        with urllib.request.urlopen(request, timeout=3) as reply:
            self.assertEqual(reply.status, 206)
            self.assertEqual(reply.read(), b'234')
        self.assertTrue(all('?' not in path for path in self.fixture.requests))

    def test_error_propagation(self):
        self.fixture.mode = 'error'
        with self.assertRaises(urllib.error.HTTPError) as error:
            self.get('/catalog/movie/tvtest/genre=Drama.json')
        self.assertEqual(error.exception.code, 500)

    def test_fake_login_accepts_only_fixture_credentials(self):
        result = self.post('login', {'email': 'tvtest@example.invalid', 'password': 'fixture-password'})['result']
        self.assertEqual(result['authKey'], 'fixture-login-token')
        self.assertEqual(result['user']['_id'], 'tvtest-login')
        request = urllib.request.Request(self.fixture.base + '/api/login',
            json.dumps({'email': 'real@example.com', 'password': 'secret'}).encode(),
            {'Content-Type': 'application/json'})
        with self.assertRaises(urllib.error.HTTPError) as error:
            urllib.request.urlopen(request, timeout=3)
        self.assertEqual(error.exception.code, 401)


if __name__ == '__main__':
    unittest.main()
