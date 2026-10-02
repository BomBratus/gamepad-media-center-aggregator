"""Private isolated-profile construction for the real UI smoke runner."""
import json
from pathlib import Path
import tempfile
import unittest

from run import profile


class ProfileContracts(unittest.TestCase):
    def test_fresh_fixture_profile_has_only_loopback_stremio(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            secrets = profile(root, 'http://127.0.0.1:41237')
            config = json.loads((root / 'GMCA' / 'config.json').read_text())

        self.assertEqual(secrets, [])
        self.assertEqual([server['type'] for server in config['servers']], ['stremio'])
        self.assertEqual(config['servers'][0]['urls'], ['http://127.0.0.1:41237'])
        self.assertEqual(config['user_id'], 'tvtest')
        self.assertEqual(config['setting']['request_threads'], 2)

    def test_live_profile_copies_one_stremio_account_only(self):
        source = Path(__file__).resolve().parent / 'fixtures' / 'multiple-accounts.json'
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            secrets = profile(root, None, source)
            config = json.loads((root / 'GMCA' / 'config.json').read_text())

        self.assertEqual([server['id'] for server in config['servers']], ['stremio-one'])
        self.assertEqual([user['id'] for user in config['users']], ['user-one'])
        self.assertEqual(config['user_id'], 'user-one')
        self.assertEqual(secrets, ['fixture-stremio-token', 'fixture-user-token'])
        serialized = json.dumps(config)
        self.assertNotIn('fixture-plex-token', serialized)
        self.assertNotIn('fixture-second-token', serialized)

    def test_legacy_selected_profile_keeps_valid_stremio_account_but_starts_signed_out(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            profile(root, 'http://127.0.0.1:41237', startup='legacy-selected')
            config = json.loads((root / 'GMCA' / 'config.json').read_text())

        self.assertEqual(config['user_id'], 'legacy-selected')
        self.assertEqual([user['id'] for user in config['users']], ['legacy-selected', 'valid-stremio'])
        self.assertEqual([server['type'] for server in config['servers']], ['plex', 'stremio'])
        self.assertEqual(config['servers'][1]['urls'], ['http://127.0.0.1:41237'])

    def test_multiple_stremio_startup_profiles_select_each_account(self):
        for case, expected in [('multiple-one', 'tvtest-one'), ('multiple-two', 'tvtest-two')]:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                profile(root, 'http://127.0.0.1:41237', startup=case)
                config = json.loads((root / 'GMCA' / 'config.json').read_text())
            self.assertEqual(config['user_id'], expected)
            self.assertEqual([server['type'] for server in config['servers']], ['stremio', 'stremio'])

    def test_fresh_startup_profile_is_empty(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            profile(root, 'http://127.0.0.1:41237', startup='fresh')
            config = json.loads((root / 'GMCA' / 'config.json').read_text())

        self.assertEqual(config['user_id'], '')
        self.assertEqual(config['users'], [])
        self.assertEqual(config['servers'], [])


if __name__ == '__main__':
    unittest.main()
