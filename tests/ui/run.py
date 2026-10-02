#!/usr/bin/env python3
"""Real Xorg/SDL/Borealis runtime regression runner. No direct focus/view commands."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from fixtures import FixtureServer

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get('GMCA_TEST_BUILD_DIR', ROOT / 'build-test-bench'))
RESULTS = ROOT / 'test-results'


def command(args, log, cwd=ROOT):
    print('TV test: ' + ' '.join(str(a) for a in args[:3]), flush=True)
    with log.open('a') as out:
        subprocess.run(args, cwd=cwd, stdout=out, stderr=subprocess.STDOUT, check=True)


def patch(name):
    path = ROOT / 'scripts/patches' / name
    cwd = ROOT / 'library/borealis'
    check = subprocess.run(['git', 'apply', '--check', str(path)], cwd=cwd, capture_output=True)
    if check.returncode == 0:
        subprocess.run(['git', 'apply', str(path)], cwd=cwd, check=True)
    elif subprocess.run(['git', 'apply', '--reverse', '--check', str(path)], cwd=cwd, capture_output=True).returncode:
        raise RuntimeError('Borealis patch drift: ' + name)


def xenv():
    return dict(os.environ, DISPLAY=os.environ.get('DISPLAY', ':0'),
                XAUTHORITY=os.environ.get('XAUTHORITY', '/home/michele/.Xauthority'))


def nodes(tree):
    yield tree
    for child in tree.get('children', []):
        yield from nodes(child)


def start_ticks(pid):
    return Path(f'/proc/{pid}/stat').read_text().split(') ', 1)[1].split()[19]


def stop_previous():
    owned = BUILD / 'runtime.json'
    if not owned.exists():
        return
    data = json.loads(owned.read_text())
    pid = int(data['pid'])
    try:
        executable = str(Path(f'/proc/{pid}/exe').resolve())
        if start_ticks(pid) != data['start_ticks'] or executable not in (str(BUILD / 'GMCA'), str(BUILD / 'GMCA') + ' (deleted)'):
            raise RuntimeError('stale runtime ownership record; refusing to signal another process')
        os.killpg(pid, signal.SIGTERM)
        for _ in range(30):
            if not Path(f'/proc/{pid}').exists():
                break
            time.sleep(.1)
        else:
            os.killpg(pid, signal.SIGKILL)
    except FileNotFoundError:
        pass
    owned.unlink()


class Runtime:
    def __init__(self, binary, directory, profile, base, secrets=()):
        self.directory = directory
        self.live = not bool(base)
        self.step = 'launch'
        self.snapshots = 0
        self.peak_rss = 0
        self.boot = time.monotonic()
        self.cpu_seconds = 0
        self.socket_path = profile.parent / 'controller.sock'
        env = dict(os.environ, DISPLAY=os.environ.get('DISPLAY', ':0'),
                   XAUTHORITY=os.environ.get('XAUTHORITY', '/home/michele/.Xauthority'),
                   XDG_CONFIG_HOME=str(profile), XDG_CACHE_HOME=str(profile.parent / 'cache'),
                   GMCA_TEST_SOCKET=str(self.socket_path), GMCA_TEST_READ_ONLY='1',
                   SDL_VIDEODRIVER='x11')
        if base:
            env['GMCA_TEST_FIXTURE_URL'] = base
        else:
            env.pop('GMCA_TEST_FIXTURE_URL', None)
        self.proc = subprocess.Popen(['stdbuf', '-oL', '-eL', str(binary)], cwd=binary.parent, env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
        (BUILD / 'runtime.json').write_text(json.dumps({'pid': self.proc.pid, 'start_ticks': start_ticks(self.proc.pid)}))
        # Never persist a raw log containing signed stream/addon URLs or account tokens.
        def collect():
            with (directory / 'gmca.log').open('w') as log:
                total = 0
                for raw in self.proc.stdout:
                    text = raw.decode(errors='replace')
                    for secret in secrets:
                        if secret:
                            text = text.replace(secret, '[redacted]')
                    text = re.sub(r'https?://[^\s"\']+', '[url]', text)
                    if total < 4 * 1024 * 1024:
                        log.write(text)
                        log.flush()
                        total += len(text)
        self.log_thread = threading.Thread(target=collect, daemon=True)
        self.log_thread.start()

    def call(self, **request):
        if self.proc.poll() is not None:
            raise RuntimeError(f'GMCA exited/crashed: {self.proc.returncode}')
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as conn:
            conn.settimeout(4)
            conn.connect(str(self.socket_path))
            conn.sendall(json.dumps(request).encode())
            reply = json.loads(conn.recv(262144))
        if reply.get('error') and not reply.get('ready'):
            raise RuntimeError(reply['error'])
        return reply

    def state(self):
        cpu = Path(f'/proc/{self.proc.pid}/stat').read_text().split(') ', 1)[1].split()
        self.cpu_seconds = (int(cpu[11]) + int(cpu[12])) / os.sysconf('SC_CLK_TCK')
        stat = Path(f'/proc/{self.proc.pid}/status').read_text()
        match = re.search(r'VmHWM:\s+(\d+)', stat)
        if match:
            self.peak_rss = max(self.peak_rss, int(match[1]))
        return self.call(command='state')

    def wait(self, predicate, step, timeout=25):
        self.step = step
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            try:
                last = self.state()
                if predicate(last):
                    return last
            except (FileNotFoundError, ConnectionRefusedError, socket.timeout):
                if self.proc.poll() is not None:
                    raise RuntimeError(f'GMCA exited/crashed: {self.proc.returncode}')
            time.sleep(.15)
        if last:
            (self.directory / 'failure-state.json').write_text(json.dumps(last, indent=2))
        raise AssertionError('timeout: ' + step)

    def press(self, button):
        saved_step = self.step
        self.wait(lambda s: not s.get('input_blocked', s['loading'] if s['dialog'] else False), 'input transition ready', timeout=10)
        self.step = saved_step
        # Let the real input loop observe the released previous button after
        # a modal transition; blocked frames do not update oldControllerState.
        self.state()
        self.state()
        self.call(command='button', button=button, pressed=True)
        time.sleep(.10)
        self.call(command='button', button=button, pressed=False)
        time.sleep(.25)

    def type_ime_text(self, value):
        """Enter synthetic fixture text through the active X11/GLFW IME."""
        self.step = 'type synthetic fixture text'
        # GLFW's X11 title is blank and Xvfb has no window manager, so select
        # the visible app by its WM_CLASS and focus it directly.
        windows = subprocess.check_output(['xdotool', 'search', '--onlyvisible', '--class', 'GMCA'],
                                          env=xenv(), text=True).splitlines()
        if not windows:
            raise RuntimeError('visible GMCA X11 window not found for fixture IME input')
        window = windows[-1]
        subprocess.run(['xdotool', 'windowfocus', '--sync', window], env=xenv(), check=True)
        subprocess.run(['xdotool', 'type', '--window', window, '--clearmodifiers', '--delay', '1', value],
                       env=xenv(), check=True)
        subprocess.run(['xdotool', 'key', '--window', window, 'Return'], env=xenv(), check=True)
        self.wait(lambda s: s.get('dialog') is None, 'submit fixture IME text', timeout=10)

    def focus(self):
        return self.state().get('focus')

    def ids(self, state=None):
        return [n.get('id', '') for n in (state or self.state()).get('focus') or []]

    def seek(self, target, buttons, limit=24):
        """Observe semantic focus after each controller input; bound all searches."""
        self.step = 'controller reach ' + target
        for _ in range(limit):
            if target in self.ids():
                return
            for button in buttons:
                self.press(button)
                assert self.focus(), 'controller navigation lost focus'
                if target in self.ids():
                    return
        raise AssertionError('unreachable from controller: ' + target)

    def checkpoint(self, name):
        self.snapshots += 1
        state = self.state()
        if self.live and (state.get('view') not in ('stremio_home', 'stremio_catalogs') or state.get('dialog')):
            raise RuntimeError('live screenshot suppressed outside safe Stremio browsing views')
        (self.directory / f'{self.snapshots:02}-{name}.json').write_text(json.dumps(state, indent=2))
        subprocess.run(['scrot', '-o', str(self.directory / f'{self.snapshots:02}-{name}.png')], env=xenv(), check=True)
        return state

    def close(self):
        if self.proc.poll() is None:
            try:
                self.call(command='quit')
                self.proc.wait(timeout=8)
            except Exception:
                os.killpg(self.proc.pid, signal.SIGTERM)
                try:
                    self.proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                    self.proc.wait()
        self.log_thread.join(timeout=2)
        (BUILD / 'runtime.json').unlink(missing_ok=True)


def profile(path, base, live=None, startup=None):
    dest = path / 'GMCA'
    dest.mkdir(parents=True)
    secrets = []
    if live:
        data = json.loads(live.read_text())
        servers = [s for s in data.get('servers', []) if s.get('type') == 'stremio' and s.get('access_token')]
        if not servers:
            raise RuntimeError('No authenticated Stremio profile available')
        server = servers[0]
        users = [u for u in data.get('users', []) if u.get('server_id') == server['id']]
        if not users:
            raise RuntimeError('Stremio profile has no matching user')
        data = {'servers': [server], 'users': [users[0]], 'user_id': users[0]['id']}
        secrets = [server['access_token'], users[0].get('access_token', '')]
        # Copy neither history nor unrelated remote-service credentials.
    elif startup == 'fresh':
        data = {'user_id': '', 'users': [], 'servers': []}
    elif startup == 'legacy-selected':
        # Keep the historical unsupported profile selected while a separate,
        # valid Stremio account remains available. Startup must fail closed to
        # sign-in rather than silently switching profiles or exposing old tiles.
        data = {
            'user_id': 'legacy-selected',
            'users': [
                {'id': 'legacy-selected', 'name': 'Legacy account', 'server_id': 'legacy-plex',
                 'access_token': 'fixture-legacy-token'},
                {'id': 'valid-stremio', 'name': 'Valid Stremio account', 'server_id': 'tvtest',
                 'access_token': 'fixture-user-token'},
            ],
            'servers': [
                {'id': 'legacy-plex', 'name': 'Legacy Plex', 'type': 'plex',
                 'access_token': 'fixture-legacy-token', 'urls': ['http://127.0.0.1/legacy']},
                {'id': 'tvtest', 'name': 'Stremio fixture', 'type': 'stremio',
                 'access_token': 'fixture-stremio-token', 'urls': [base],
                 'addons': [base + '/manifest.json']},
            ],
            'remotes': [{'id': 'legacy-remote', 'url': 'http://127.0.0.1:1/keep'}],
            'pins': [{'id': 'legacy-pin', 'opaque': True}],
            'legacy_extension': {'must_survive': ['legacy', 7]},
        }
    elif startup in ('multiple-one', 'multiple-two'):
        data = {
            'user_id': 'tvtest-one' if startup == 'multiple-one' else 'tvtest-two',
            'users': [
                {'id': 'tvtest-one', 'name': 'Fixture account one', 'server_id': 'stremio-one',
                 'access_token': 'fixture-user-one'},
                {'id': 'tvtest-two', 'name': 'Fixture account two', 'server_id': 'stremio-two',
                 'access_token': 'fixture-user-two'},
            ],
            'servers': [
                {'id': 'stremio-one', 'name': 'Stremio fixture one', 'type': 'stremio',
                 'access_token': 'fixture-token-one', 'urls': [base], 'addons': [base + '/manifest.json']},
                {'id': 'stremio-two', 'name': 'Stremio fixture two', 'type': 'stremio',
                 'access_token': 'fixture-token-two', 'urls': [base], 'addons': [base + '/manifest.json']},
            ],
        }
    else:
        data = {'user_id': 'tvtest', 'users': [{'id': 'tvtest', 'name': 'TV test', 'server_id': 'tvtest', 'access_token': 'fixture'}],
                'servers': [{'id': 'tvtest', 'name': 'Stremio fixture', 'type': 'stremio',
                             'access_token': 'fixture', 'urls': [base], 'addons': [base + '/manifest.json']}]}
    data['setting'] = {'app_update': '1.1.0', 'fullscreen': True, 'app_lang': 'en-US',
                       'request_threads': 2, 'player_subtitle_lang': 'off', 'app_swap_abxy': False}
    (dest / 'config.json').write_text(json.dumps(data))
    (dest / 'config.json').chmod(0o600)
    if not live:
        for kind, ident in [('movies', 'tt9000001'), ('series', 'tt9000010')]:
            (dest / f'imdb-top-{kind}.json').write_text(json.dumps({'schema': 1, 'fetchedAt': int(time.time()),
                'items': [{'id': ident, 'title': 'Fixture top', 'year': 2025, 'rating': 8.0}]}))
    return secrets


def boot(app):
    app.step = 'boot to Stremio home'
    state = app.wait(lambda s: s.get('focus') and s.get('controllers', 0) > 0 and
                     any(n.get('id') == 'tab/home' for n in nodes(s.get('tree', {}))) and not s['loading'], 'boot to Stremio home')
    assert state.get('mapping_verified'), 'SDL to Borealis button mapping was not verified'
    app.build_commit = state.get('build_commit')
    expected = getattr(app, 'expected_commit', None)
    assert not expected or (app.build_commit and expected.startswith(app.build_commit)), 'executable build commit differs from configured candidate'
    app.boot_seconds = time.monotonic() - app.boot
    app.checkpoint('home')
    return state


def navigation(app):
    original = app.focus()
    app.press('down')
    assert app.focus() and app.focus() != original, 'D-pad did not change focus'
    app.press('up')
    assert app.focus(), 'focus lost after Up'
    app.checkpoint('navigation')


def scenarios(app, name, fixture):
    if name in ('fresh', 'legacy-selected'):
        app.step = 'startup sign-in routing'
        state = app.wait(lambda s: s.get('view') == 'stremio_signin' and s.get('focus'),
                         'fresh Stremio sign-in screen')
        assert contains(app, 'StremioAdd', state), 'logged-out startup did not show Stremio sign-in'
        if name == 'legacy-selected':
            assert not any('ConnectionTile' in n.get('class', '') for n in flat(app, state)), \
                'unsupported legacy connection tile leaked into Stremio sign-in'
            assert state.get('test_account_id') == 'legacy-selected', \
                'startup silently changed the selected legacy account'
        app.checkpoint(name)
        return
    if name == 'fresh-login':
        state = app.wait(lambda s: s.get('view') == 'stremio_signin' and s.get('focus'),
                         'fresh profile Stremio sign-in')
        assert contains(app, 'StremioAdd', state), 'fresh profile did not reach sign-in form'
        app.press('a')
        app.wait(lambda s: contains(app, 'EditTextDialog', s), 'open email input')
        app.type_ime_text('tvtest@example.invalid')
        app.press('down')
        app.press('a')
        app.wait(lambda s: contains(app, 'EditTextDialog', s), 'open password input')
        app.type_ime_text('fixture-password')
        app.press('down')
        app.press('a')
        state = app.wait(lambda s: s.get('view') in ('stremio_home', 'stremio_catalogs') and
                         s.get('test_account_id') == 'tvtest-login', 'fixture Stremio sign-in completes')
        assert any(path == '/api/login' for path in fixture.requests), 'fixture auth endpoint was not called'
        app.checkpoint('fresh-sign-in-complete')
        return
    if name in ('multiple-one', 'multiple-two'):
        state = boot(app)
        expected = 'tvtest-one' if name == 'multiple-one' else 'tvtest-two'
        assert state.get('test_account_id') == expected, \
            f'{expected} was not the active Stremio account after restart'
        assert state.get('view') in ('stremio_home', 'stremio_catalogs'), \
            'selected Stremio account did not reach the app shell'
        app.checkpoint(name)
        if name == 'multiple-one':
            # RB follows the real sidebar tab order, including the pinned
            # account-picker tab. Then move from the selected first account to
            # the second tile and activate it through the controller path.
            app.step = 'navigate to account switcher'
            switcher = None
            for _ in range(16):
                state = app.state()
                if state.get('view') == 'account_switcher':
                    switcher = state
                    break
                focused_sidebar = (state.get('focus') or [None])[0]
                if (focused_sidebar and 'AutoSidebarItem' in focused_sidebar.get('class', '')
                        and not focused_sidebar.get('id')):
                    app.press('a')
                    switcher = app.wait(lambda s: s.get('view') == 'account_switcher',
                                        'activate account switcher sidebar tab')
                    break
                app.press('down')
            assert switcher, 'sidebar navigation did not open the account switcher'
            tiles = [n for n in flat(app, switcher) if 'ConnectionTile' in n.get('class', '')]
            assert len(tiles) == 2, f'expected two Stremio account tiles, got {len(tiles)}'
            app.checkpoint('account-switcher-open')
            app.press('right')
            assert any('ConnectionTile' in node.get('class', '') for node in (app.state().get('focus') or [])), \
                'D-pad Right did not focus a real account tile'
            # The first Right enters the grid from the avatar; the second
            # moves from the active first account to the other Stremio tile.
            app.press('right')
            app.checkpoint('account-switcher-right')
            app.press('a')
            state = app.wait(lambda s: s.get('test_account_id') == 'tvtest-two' and
                             s.get('view') in ('stremio_home', 'stremio_catalogs'),
                             'select second Stremio account from account switcher')
            app.checkpoint('selected-second-account')
        return
    if name == 'library':
        boot(app)
        sidebar(app, 'tab/watchlist')
        app.press('right')
        state = app.wait(lambda s: contains(app, 'WatchlistTab', s) and
                         any(i['id'] in ('movie:tt9000001', 'series:tt9000010')
                             for n in flat(app, s) for i in n.get('media_items', [])),
                         'Stremio account library items', timeout=25)
        items = [i['id'] for n in flat(app, state) for i in n.get('media_items', [])]
        assert 'movie:tt9000001' in items and 'series:tt9000010' in items, \
            'Stremio library did not render both movie and series entries'
        assert '/api/datastoreGet' in fixture.requests, 'library did not read the fixture account datastore'
        app.checkpoint('stremio-library')
        return
    if name == 'offline-download':
        boot(app)
        # Queue a concrete resolved source from the movie details screen. This
        # covers the Stremio explicit-source DownloadManager path, including
        # caching source metadata and writing the completed index/file.
        catalog(app, 'movie')
        app.seek('movie:tt9000001', ['down', 'left'], limit=10)
        app.press('a')
        app.wait(lambda s: contains(app, 'MediaMovie', s) and
                 'movie/source/0' in app.ids(s) and not s['loading'],
                 'movie details with fixture release row')
        app.seek('movie/source/0', ['down'], limit=12)
        app.press('x')
        config_dir = getattr(app, 'config_dir', None)
        assert config_dir, 'runtime profile path not available for download verification'
        index_path = Path(config_dir) / 'downloads' / 'index.json'
        deadline = time.monotonic() + 45
        completed = None
        while time.monotonic() < deadline:
            try:
                entries = json.loads(index_path.read_text())
                completed = next((item for item in entries if item.get('itemId') == 'movie:tt9000001'), None)
                if completed and completed.get('status') == 'Completed':
                    break
            except (OSError, ValueError):
                pass
            app.state()
            time.sleep(.2)
        assert completed and completed.get('status') == 'Completed', \
            'source download did not reach Completed in downloads/index.json'
        download_path = Path(config_dir) / 'downloads' / completed['itemId'] / completed['filePath']
        assert download_path.is_file() and download_path.stat().st_size == fixture.media_path.stat().st_size, \
            'completed download file is missing or has an unexpected size'
        assert '/video.mp4' in fixture.requests, 'explicit source download did not fetch fixture media'
        assert completed.get('partKey', '').startswith(fixture.base + '/video.mp4'), \
            'completed download did not cache the selected fixture source URL'
        media_requests_before_offline_playback = fixture.requests.count('/video.mp4')
        # Stop the loopback server only after the queue and file are complete;
        # playback must now come solely from the Downloads tab's local path.
        fixture.close()
        sidebar(app, 'tab/downloads')
        app.press('right')
        state = app.wait(lambda s: contains(app, 'DownloadView', s) and
                         any(n.get('text') == 'Fixture Movie' for n in flat(app, s)),
                         'completed download list')
        for _ in range(8):
            if any('DownloadCard' in item.get('class', '') for item in (app.state().get('focus') or [])):
                break
            app.press('down')
        assert any('DownloadCard' in item.get('class', '') for item in (app.state().get('focus') or [])), \
            'D-pad did not focus the completed download card'
        app.press('a')
        state = app.wait(lambda s: s.get('local_playback') and s.get('duration_seconds', 0) > 0 and
                         not s.get('player_stopped'), 'completed download opens LocalPlayer', timeout=25)
        assert fixture.requests.count('/video.mp4') == media_requests_before_offline_playback, \
            'offline playback requested the network video URL again'
        app.checkpoint('offline-local-player')
        close_player(app, 'local player Back')
        return
    if name == 'subtitles':
        boot(app)
        play_first_episode(app, fixture)
        state = app.wait(lambda s: len(s.get('subtitle_tracks', [])) >= 2,
                         'external Stremio subtitle tracks loaded in mpv', timeout=25)
        assert any('/subtitles/' in path for path in fixture.requests), \
            'Stremio subtitles resource was not requested'
        assert '/subtitle.vtt' in fixture.requests, 'mpv did not fetch the fixture subtitle sidecar'
        app.checkpoint('external-subtitles-loaded')
        close_player(app, 'subtitle player Back')
        return
    if name == 'next-episode':
        boot(app)
        play_first_episode(app, fixture)
        initial = app.wait(lambda s: s.get('player_item') == 'series:tt9000010:2:1',
                           'first season-two episode starts')
        for _ in range(18):
            if initial.get('dialog') == 'dialog' or initial.get('playback_seconds', 0) >= initial.get('duration_seconds', 120) - 8:
                break
            app.press('r1')
            initial = app.state()
        if initial.get('dialog') != 'dialog':
            initial = app.wait(lambda s: s.get('dialog') == 'dialog',
                               'up-next prompt near episode end', timeout=25)
        texts = [n.get('text', '') for n in flat(app, initial)]
        assert any('Play next now' in text for text in texts), 'up-next prompt does not offer the next episode'
        app.press('a')
        state = app.wait(lambda s: s.get('player_item') == 'series:tt9000010:2:2' and
                         s.get('duration_seconds', 0) > 0 and not s.get('player_stopped'),
                         'next episode starts from the up-next prompt', timeout=30)
        app.checkpoint('next-episode-player')
        close_player(app, 'next episode player Back')
        return
    boot(app)
    if name in ('smoke', 'navigation', 'live'):
        navigation(app)
    if name in ('boot', 'navigation'):
        return
    if name == 'live':
        # Read-only live integration uses the real account/addons; never starts playback.
        sidebar(app, 'lib/movie')
        app.press('right')
        app.wait(lambda s: contains(app, 'StremioCatalogs', s) and not s['loading'] and bool(s['focus']) and any(n.get('id', '').startswith('movie:') and 'VideoCardCell' in n.get('class', '') for n in flat(app, s)), 'live movies catalog', timeout=60)
        app.checkpoint('live-movies')
        return
    # Concrete scenario functions are below; every route observes real UI state.
    selected = (name,)
    app.timings = {}
    for scenario in selected:
        print('TV test scenario: ' + scenario, flush=True)
        began = time.monotonic()
        FUNCTIONS[scenario](app, fixture)
        app.timings[scenario] = round(time.monotonic() - began, 3)


def flat(app, state=None):
    return list(nodes((state or app.state()).get('tree', {})))


def contains(app, value, state=None):
    return any(value in n.get('class', '') for n in flat(app, state))


def home(app):
    for _ in range(6):
        if app.state().get('depth', 0) <= 1 and not any(contains(app, cls) for cls in ('MediaMovie', 'MediaSeries', 'MediaSeason', 'SearchResult', 'MediaCollection')):
            break
        app.press('b')
    sidebar(app, 'tab/home')
    app.wait(lambda s: not s['loading'] and bool(s['focus']), 'home ready')


def sidebar(app, target):
    # Reach the actual sidebar using D-pad; no test command sets focus.
    for _ in range(12):
        if any(i.startswith(('tab/', 'lib/')) for i in app.ids()):
            break
        app.press('left')
    for _ in range(12):
        old = app.focus()
        app.press('up')
        if app.focus() == old:
            break
    app.seek(target, ['down'], limit=18)


def catalog(app, kind):
    home(app)
    sidebar(app, 'lib/' + kind)
    app.press('right')
    app.wait(lambda s: not s['loading'] and contains(app, 'StremioCatalogs', s), kind + ' catalog ready')
    app.press('down')
    app.wait(lambda s: bool(s['focus']), kind + ' grid focus')


def movie(app, fixture):
    catalog(app, 'movie')
    app.seek('movie:tt9000001', ['down', 'left'], limit=10)
    app.checkpoint('movies-catalog')
    app.press('a')
    app.wait(lambda s: contains(app, 'MediaMovie', s) and not s['loading'], 'movie detail')
    app.checkpoint('movie')
    app.press('b')
    app.wait(lambda s: not contains(app, 'MediaMovie', s), 'movie Back')
    home(app)


def series(app, fixture):
    catalog(app, 'series')
    app.seek('series:tt9000010', ['down', 'left'], limit=10)
    app.press('a')
    app.wait(lambda s: contains(app, 'MediaSeries', s) and not s['loading'], 'series detail')
    app.checkpoint('series')
    app.seek('series/seasons', ['down'], limit=12)
    original = app.focus()
    app.press('right')
    assert app.focus() and app.focus() != original, 'cannot switch season'
    app.checkpoint('season-selection')
    app.press('a')
    app.wait(lambda s: contains(app, 'MediaSeason', s) and any(n.get('id') == 'series:tt9000010:2:1' for n in flat(app, s)), 'season episodes')
    app.seek('series:tt9000010:2:1', ['down'], limit=6)
    app.press('down')
    app.wait(lambda s: 'series:tt9000010:2:2' in app.ids(s), 'episode navigation')
    app.press('up')
    app.checkpoint('episodes')
    return


def play_first_episode(app, fixture):
    series(app, fixture)
    app.wait(lambda s: 'series:tt9000010:2:1' in app.ids(s), 'first episode focus')
    app.press('a')
    app.wait(lambda s: s['source_picker'] and not s['loading'], 'episode source picker')
    app.press('a')
    return app.wait(lambda s: s['player'] and s.get('duration_seconds', 0) > 0 and
                    not s.get('player_stopped'), 'fixture episode starts', timeout=30)


def close_player(app, step):
    app.step = step
    # TV mode first hides a visible OSD; a following Back closes playback.
    for _ in range(3):
        if not app.state()['player']:
            break
        app.press('b')
    app.wait(lambda s: not s['player'], step)


def source_picker(app, fixture):
    if not contains(app, 'MediaSeason'):
        series(app, fixture)
    # Episode IDs are carried by the actual recycling card, not a counter of presses.
    app.wait(lambda s: any(i.startswith('series:tt9000010:') for i in app.ids(s)), 'episode focus')
    before = app.focus()
    app.press('a')
    app.wait(lambda s: s['source_picker'] and s['focus'], 'episode source picker')
    state = app.checkpoint('source-picker')
    rows = [n for n in flat(app, state) if n.get('id', '').startswith('stremio/source/')]
    assert len(rows) == 2, 'source picker fixture did not expose both sources'
    labels = [[v.get('text') for v in nodes(row)] for row in rows]
    assert sum('ITA AUDIO' in values for values in labels) == 1, 'ITA AUDIO must classify audio only, never SUB ITA'
    first = app.focus()
    app.press('down')
    assert app.focus() and first != app.focus(), 'source list is not navigable'
    app.press('b')
    app.wait(lambda s: not s['source_picker'], 'source picker Back')
    assert app.focus() == before, 'picker did not restore episode focus'
    app.press('a')
    app.wait(lambda s: s['source_picker'] and not s['loading'], 'source picker reopen')
    state = app.checkpoint('source-picker-reopened')
    assert any(i.startswith('stremio/source/') for i in app.ids(state)), 'reopened picker focused Cancel instead of source'
    app.press('a')
    app.wait(lambda s: s['player'] and s.get('duration_seconds', 0) > 0 and not s.get('player_stopped'), 'source confirm opens playing mpv', timeout=45)
    app.checkpoint('player')
    close_player(app, 'player Back')
    home(app)


def continue_watching(app, fixture):
    home(app)
    app.press('right')
    app.wait(lambda s: any(i.startswith(('series:', 'movie:')) for i in app.ids(s)), 'Continue Watching focus')
    state = app.checkpoint('continue-watching')
    rows = [n for n in flat(app, state) if n.get('continue_watching')]
    assert rows, 'Continue Watching row missing'
    items = rows[0]['media_items']
    assert any(i['id'] == 'series:tt9000010' and i['key'] == 'series:tt9000010:1:1' and i['position_ms'] == 12000 for i in items), 'partial series checkpoint missing'
    assert any(i['id'] == 'movie:tt9000001' and i['position_ms'] == 12000 for i in items), 'partial movie missing'
    app.press('a')
    app.wait(lambda s: s['dialog'] == 'resume', 'partial progress offers Resume menu')
    app.checkpoint('resume-menu')
    app.press('b')
    app.wait(lambda s: s['dialog'] is None, 'Resume Back')
    home(app)
    with fixture._lock:
        record = next(r for r in fixture.library if r['_id'] == 'tt9000010')
        record['state']['timeOffset'] = 119000
    app.press('right')
    app.press('back')
    state = app.wait(lambda s: not s['loading'] and any(n.get('continue_watching') and any(i['id'] == 'series:tt9000010' and i['key'] == 'series:tt9000010:1:2' for i in n.get('media_items', [])) for n in flat(app, s)), 'completed episode advances to next')
    app.checkpoint('continue-next-episode')
    with fixture._lock:
        record['state']['timeOffset'] = 12000
    app.press('right')
    app.press('back')
    app.wait(lambda s: not s['loading'] and any(n.get('continue_watching') and any(i['id'] == 'series:tt9000010' and i['key'] == 'series:tt9000010:1:1' and i['position_ms'] == 12000 for i in n.get('media_items', [])) for n in flat(app, s)), 'partial checkpoint restored')


def resume(app, fixture):
    # Exercise both movie and series with existing progress.
    for ident in ('movie:tt9000001', 'series:tt9000010'):
        home(app)
        app.press('right')
        app.seek(ident, ['right'], limit=4)
        app.press('a')
        app.wait(lambda s: s['dialog'] == 'resume', 'resume menu ' + ident)
        # Overview is the fourth actual action; verify its resulting screen.
        for _ in range(3):
            app.press('down')
        app.press('a')
        cls = 'MediaMovie' if ident.startswith('movie:') else 'MediaSeries'
        app.wait(lambda s: contains(app, cls, s) and not s['loading'], 'Go to overview ' + ident)
        app.checkpoint('resume-overview')
        home(app)
        app.press('right')
        app.seek(ident, ['right'], limit=4)
        app.press('a')
        app.wait(lambda s: s['dialog'] == 'resume', 'Resume reopened ' + ident)
        # Resume, then Restart. Values are reached through real dropdown navigation.
        app.press('a')
        state = app.wait(lambda s: s['source_picker'] or s['player'], 'Resume source choice')
        if state['source_picker']:
            app.press('a')
        app.wait(lambda s: s['player'] and s.get('playback_seconds', 0) >= 10, 'Resume keeps checkpoint')
        app.checkpoint('resume-player')
        close_player(app, 'Resume player closed')
        home(app)
        app.press('right')
        app.seek(ident, ['right'], limit=4)
        app.press('a')
        app.wait(lambda s: s['dialog'] == 'resume', 'Restart menu')
        app.press('down')
        app.press('down')
        app.press('a')
        state = app.wait(lambda s: s['source_picker'] or s['player'], 'Restart source choice')
        if state['source_picker']:
            app.press('a')
        state = app.wait(lambda s: s['player'] and s.get('duration_seconds', 0) > 0, 'Restart player opened')
        assert state.get('playback_seconds', 999) < 5, 'Restart reused progress'
        app.checkpoint('restart-player')
        close_player(app, 'Restart player closed')
    home(app)


def toggle_watched(app):
    app.press('x')
    app.wait(lambda s: contains(app, 'ContextMenu', s), 'watched context menu')
    app.seek('menu/mark/play', ['down'], limit=8)
    app.checkpoint('watched-menu')
    app.press('a')
    app.wait(lambda s: not contains(app, 'ContextMenu', s), 'watched action closes menu')


def datastore_state(fixture, ident, predicate):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        with fixture._lock:
            if any(r['_id'] == ident and predicate(r.get('state', {})) for r in fixture.library):
                return
        time.sleep(.1)
    raise AssertionError('watched not persisted to fixture datastore: ' + ident)


def watched(app, fixture):
    # Movie and whole-series actions must be reversible and stay separate.
    for kind, ident in [('movie', 'tt9000001'), ('series', 'tt9000010')]:
        catalog(app, kind)
        key = kind + ':' + ident
        app.seek(key, ['down', 'left'], limit=10)
        for expected in (True, False):
            toggle_watched(app)
            app.wait(lambda s: any(any(i['id'] == key and i['watched'] == expected for i in n.get('media_items', [])) for n in flat(app, s)), 'watched card state ' + key)
            datastore_state(fixture, ident, lambda s: bool(s.get('flaggedWatched')) == expected)
            app.checkpoint('watched-' + kind + ('-set' if expected else '-clear'))
        home(app)
    # An episode checkmark must not mark its sibling or the complete show watched.
    series(app, fixture)
    episode = 'series:tt9000010:2:1'
    app.seek(episode, ['down'], limit=6)
    def badge(state, ident):
        card = next((n for n in flat(app, state) if n.get('id') == ident), None)
        return card is not None and any(n.get('id') == 'episode/card/watched' for n in nodes(card))
    for expected in (True, False):
        app.seek(episode, ['up', 'down'], limit=6)
        toggle_watched(app)
        app.wait(lambda s: any(n.get('id') == episode for n in flat(app, s)) and badge(s, episode) == expected, 'episode watched badge')
        assert not badge(app.state(), 'series:tt9000010:2:2'), 'episode action marked its sibling watched'
        datastore_state(fixture, 'tt9000010', lambda s: not s.get('flaggedWatched') and s.get('videoId') == 'tt9000010:2:1')
        app.checkpoint('watched-episode' + ('-set' if expected else '-clear'))
    home(app)


def search(app, fixture):
    home(app)
    sidebar(app, 'tab/search')
    app.press('right')
    app.wait(lambda s: contains(app, 'SearchTab', s), 'controller search')
    app.checkpoint('search')
    # The on-screen keys are intentionally anonymous layout cells. Exercise the
    # first key through the real action row: down to its first button, then the
    # explicit route from that button down to key A.
    app.press('down')
    app.press('down')
    for _ in range(5):  # A -> F, which matches the deterministic Fixture titles.
        app.press('right')
    app.press('a')
    state = app.checkpoint('search-input')
    input_text = next((n.get('text', '') for n in flat(app, state)
                       if n.get('id') == 'tv/search/input'), '')
    assert input_text == 'F', 'controller keyboard failed to enter the fixture search query'
    app.press('start')
    app.wait(lambda s: contains(app, 'SearchResult', s) and not s['loading'] and bool(s['focus']) and any(n.get('media_items') for n in flat(app, s)), 'search results preserve focus')
    state = app.checkpoint('search-results')
    ids = [i['id'] for n in flat(app, state) for i in n.get('media_items', [])]
    assert ids and len(ids) == len(set(ids)), 'search missing results or immediately duplicated'
    home(app)


def genre(app, title):
    def targets(state):
        return [n['id'] for n in flat(app, state) if n.get('id', '').startswith('genre/') and any(v.get('text') == title for v in nodes(n))]
    state = app.wait(lambda s: bool(targets(s)), 'genre card ' + title)
    if not any(i.startswith('genre/') for i in app.ids(state)):
        app.press('down')
    app.seek(targets(state)[0], ['right'], limit=6)


def error_loading(app, fixture):
    catalog(app, 'movie')
    app.press('r1')
    app.press('r1')
    app.wait(lambda s: contains(app, 'GenresTab', s) and not s['loading'], 'Genres controller tab')
    genre(app, 'Drama')
    app.checkpoint('genres')
    fixture.mode = 'slow'
    app.press('a')
    app.wait(lambda s: s['loading'], 'slow genre request loading visible', timeout=5)
    assert app.focus(), 'loading removed all focus'
    app.press('down')
    assert app.focus(), 'loading navigation lost focus'
    app.checkpoint('loading')
    app.wait(lambda s: not s['loading'], 'slow request completes without human input', timeout=15)
    ids = [i['id'] for n in flat(app) for i in n.get('media_items', [])]
    assert ids and len(ids) == len(set(ids)), 'genre catalog empty or duplicated'
    app.checkpoint('genre-catalog')
    app.press('b')
    app.wait(lambda s: contains(app, 'GenresTab', s), 'genre catalog Back')
    genre(app, 'Comedy')
    fixture.mode = 'error'
    app.press('a')
    app.wait(lambda s: s['error'], 'request error visible', timeout=15)
    app.checkpoint('error')
    fixture.mode = 'normal'
    app.press('b')
    app.wait(lambda s: s['dialog'] is None and s['focus'] and contains(app, 'GenresTab', s), 'error Back restores navigation')
    home(app)


FUNCTIONS = {'movies': movie, 'series': series, 'source-picker': source_picker,
             'continue-watching': continue_watching, 'resume': resume, 'watched': watched,
             'search': search, 'error-loading': error_loading}


def check_live_account(profile_path):
    # One authenticated read proves live account integration without persisting
    # its returned history or sending any mutation to the account.
    data = json.loads(profile_path.read_text())
    server = next(s for s in data['servers'] if s.get('type') == 'stremio' and s.get('access_token'))
    user = next(u for u in data['users'] if u.get('server_id') == server['id'])
    token = user.get('access_token') or server['access_token']
    request = urllib.request.Request('https://api.strem.io/api/datastoreGet',
        json.dumps({'authKey': token, 'collection': 'libraryItem', 'all': True}).encode(),
        {'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            reply = json.load(response)
        if reply.get('error') or not isinstance(reply.get('result'), list):
            raise ValueError('invalid account read response')
    except Exception:
        raise RuntimeError('live authenticated account read failed') from None


def configured_live():
    configured = os.environ.get('GMCA_TEST_LIVE_CONFIG')
    if not configured:
        return None
    candidate = Path(configured).expanduser()
    try:
        data = json.loads(candidate.read_text())
        if any(s.get('type') == 'stremio' and s.get('access_token') and any(u.get('server_id') == s.get('id') for u in data.get('users', [])) for s in data.get('servers', [])):
            return candidate
    except (OSError, ValueError):
        pass
    return None


def runtime_case(name, directory, media, expected_commit=None):
    result = {'scenario': name, 'status': 'FAIL', 'step': 'launch'}
    app = fixture = None
    began = time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix='gmca-tvtest-') as temporary:
            path = Path(temporary) / 'config'
            if name == 'live':
                live = configured_live()
                if not live:
                    raise RuntimeError('Live tests require an explicit authenticated GMCA_TEST_LIVE_CONFIG path')
                result['step'] = 'live authenticated account read'
                secrets = profile(path, None, live)
                check_live_account(path / 'GMCA/config.json')
                result['account_read_validated'] = True
                base = None
            else:
                fixture = FixtureServer(media).start()
                base = fixture.base
                startup = 'fresh' if name == 'fresh-login' else (name if name in (
                    'fresh', 'legacy-selected', 'multiple-one', 'multiple-two') else None)
                secrets = profile(path, base, startup=startup)
            stop_previous()
            app = Runtime(BUILD / 'GMCA', directory, path, base, secrets)
            app.config_dir = path / 'GMCA'
            app.expected_commit = expected_commit
            result['step'] = 'runtime'
            try:
                scenarios(app, name, fixture)
                result['status'] = 'PASS'
                result['step'] = 'complete'
            finally:
                if result['status'] != 'PASS':
                    try:
                        app.checkpoint('failure')
                    except Exception:
                        if name != 'live':
                            subprocess.run(['scrot', '-o', str(directory / 'failure.png')], env=xenv(), check=False)
                result['failed_step'] = app.step if result['status'] != 'PASS' else None
                result['peak_rss_kib'] = app.peak_rss
                result['cpu_seconds'] = round(app.cpu_seconds, 3)
                result['scenario_seconds'] = getattr(app, 'timings', {})
                result['boot_seconds'] = getattr(app, 'boot_seconds', None)
                result['build_commit'] = getattr(app, 'build_commit', None)
                app.close()
                result['exit_code'] = app.proc.returncode
                if app.proc.returncode != 0:
                    result['status'] = 'FAIL'
                    result['failed_step'] = result.get('failed_step') or 'GMCA exit'
                if name == 'legacy-selected':
                    saved = json.loads((path / 'GMCA' / 'config.json').read_text())
                    assert saved.get('user_id') == 'legacy-selected', 'legacy selected account ID was rewritten'
                    assert saved.get('users', []) == [
                        {'id': 'legacy-selected', 'name': 'Legacy account', 'server_id': 'legacy-plex',
                         'access_token': 'fixture-legacy-token'},
                        {'id': 'valid-stremio', 'name': 'Valid Stremio account', 'server_id': 'tvtest',
                         'access_token': 'fixture-user-token'},
                    ], 'legacy and valid Stremio user records were not preserved'
                    assert saved.get('remotes') == [{'id': 'legacy-remote', 'url': 'http://127.0.0.1:1/keep'}], \
                        'legacy remotes were not preserved'
                    assert saved.get('pins') == [{'id': 'legacy-pin', 'opaque': True}], \
                        'legacy pins were not preserved'
                    assert saved.get('legacy_extension') == {'must_survive': ['legacy', 7]}, \
                        'opaque legacy config field was not preserved'
                    result['legacy_config_preserved'] = True
    except Exception as error:
        result['status'] = 'FAIL'
        result['error'] = str(error)
    finally:
        if fixture:
            (directory / 'requests.json').write_text(json.dumps(fixture.requests, indent=2))
            fixture.close()
        result['duration_seconds'] = round(time.monotonic() - began, 3)
        (directory / 'result.json').write_text(json.dumps(result, indent=2))
    return result


def main():
    global BUILD
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('scenario', nargs='?', default='smoke', choices=[
        'smoke', 'fresh', 'fresh-login', 'legacy-selected', 'multiple-one', 'multiple-two',
        'boot', 'navigation', 'movies', 'series', 'library', 'source-picker',
        'continue-watching', 'subtitles', 'next-episode', 'offline-download',
        'resume', 'watched', 'search', 'error-loading', 'live',
    ])
    parser.add_argument('--runtime-only', action='store_true', help='reuse already validated build for scenario debugging')
    parser.add_argument('--live', action='store_true', help='opt in to a read-only live case using GMCA_TEST_LIVE_CONFIG')
    parser.add_argument('--build-dir', type=Path, default=BUILD, help='isolated build directory (default: build-test-bench)')
    parser.add_argument('--generator', help='optional CMake generator; otherwise reuse the existing build cache or default generator')
    args = parser.parse_args()
    BUILD = args.build_dir if args.build_dir.is_absolute() else ROOT / args.build_dir
    if args.live and not os.environ.get('GMCA_TEST_LIVE_CONFIG'):
        parser.error('--live requires GMCA_TEST_LIVE_CONFIG to name a private config file')
    if args.live and args.scenario not in ('smoke', 'live'):
        parser.error('--live is available only with smoke or live')
    os.umask(0o077)
    RESULTS.mkdir(exist_ok=True)
    (BUILD / 'tmp').mkdir(parents=True, exist_ok=True)
    os.environ['TMPDIR'] = str(BUILD / 'tmp')
    tempfile.tempdir = str(BUILD / 'tmp')
    with (RESULTS / '.lock').open('w') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print('FAIL: another TV test runner is active')
            return 1
        directory = RESULTS / ('run-' + time.strftime('%Y%m%d-%H%M%S') + '-' + args.scenario)
        directory.mkdir()
        result = {'scenario': args.scenario, 'status': 'FAIL', 'step': 'dependencies', 'ps4_validated': False}
        start = time.monotonic()
        try:
            for tool in ('git', 'c++', 'cmake', 'make', 'pkg-config', 'ffmpeg', 'scrot', 'xrandr', 'stdbuf', 'xdotool'):
                if not shutil.which(tool):
                    raise RuntimeError('missing dependency: ' + tool)
            for package in ('sdl2', 'mpv', 'libavformat', 'libcurl'):
                subprocess.run(['pkg-config', '--exists', package], check=True)
            subprocess.run(['xrandr', '--current'], env=dict(os.environ, DISPLAY=os.environ.get('DISPLAY', ':0'), XAUTHORITY=os.environ.get('XAUTHORITY', '/home/michele/.Xauthority')), stdout=subprocess.DEVNULL, check=True)
            result['step'] = 'stop previous test runtime'
            stop_previous()
            result['runtime_only'] = args.runtime_only
            result['commit'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
            result['dirty'] = bool(subprocess.check_output(['git', 'status', '--porcelain', '--ignore-submodules=dirty'], cwd=ROOT))
            if not args.runtime_only:
                result['step'] = 'submodules'
                command(['git', 'submodule', 'update', '--init', '--recursive'], directory / 'build.log')
                patch('borealis-fixes.patch')
                patch('borealis-test-observers.patch')
                result['step'] = 'unit tests'
                command([str(ROOT / 'tests/run.sh')], directory / 'unit-tests.log')
                command([sys.executable, '-m', 'unittest', 'discover', '-s', str(ROOT / 'tests/ui'), '-p', 'test_*.py'], directory / 'unit-tests.log')
                result['step'] = 'configure'
                configure = ['cmake', '-S', str(ROOT), '-B', str(BUILD), '-DGMCA_LINUX_TEST_BENCH=ON', '-DUSE_SDL2=ON', '-DUSE_SYSTEM_SDL2=ON', '-DGMCA_TEST_HARNESS=ON']
                if args.generator:
                    configure += ['-G', args.generator]
                if shutil.which('ccache'):
                    configure += ['-DCMAKE_CXX_COMPILER_LAUNCHER=ccache', '-DCMAKE_C_COMPILER_LAUNCHER=ccache']
                command(configure, directory / 'build.log')
                result['step'] = 'build'
                command(['cmake', '--build', str(BUILD), '--parallel', '2'], directory / 'build.log')
            command(['cmake', '-E', 'copy_directory', str(ROOT / 'resources'), str(BUILD / 'resources')], directory / 'build.log')
            result['step'] = 'fixture video'
            media = BUILD / 'tvtest-video.mp4'
            if not media.exists():
                command(['ffmpeg', '-nostdin', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=160x90:rate=5', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=22050', '-t', '120', '-c:v', 'mpeg4', '-q:v', '10', '-c:a', 'aac', '-threads', '1', '-movflags', '+faststart', str(media)], directory / 'build.log')
            result['step'] = 'runtime'
            selected = ('fresh', 'fresh-login', 'legacy-selected', 'multiple-one', 'multiple-two',
                        'navigation', 'library', 'continue-watching', 'source-picker', 'subtitles',
                        'next-episode', 'offline-download', 'resume', 'movies', 'watched', 'search',
                        'error-loading') if args.scenario == 'smoke' else (args.scenario,)
            result['cases'] = {}
            if args.scenario == 'smoke':
                if args.live:
                    if not configured_live():
                        raise RuntimeError('The explicit live config contains no authenticated Stremio profile')
                    selected += ('live',)
                else:
                    result['cases']['live'] = {'status': 'SKIP', 'reason': 'live checks are opt-in with --live and GMCA_TEST_LIVE_CONFIG'}
            for name in selected:
                case_directory = directory / name if args.scenario == 'smoke' else directory
                case_directory.mkdir(exist_ok=True)
                case = runtime_case(name, case_directory, media, None if args.runtime_only else result['commit'])
                result['cases'][name] = case
                result['peak_rss_kib'] = max(result.get('peak_rss_kib', 0), case.get('peak_rss_kib', 0))
                if case['status'] != 'PASS':
                    result['failed_step'] = name + ': ' + (case.get('failed_step') or case['step'])
                    raise AssertionError(case.get('error', 'GMCA exit/crash'))
            result['status'] = 'PASS'
            result['step'] = 'complete'
        except Exception as error:
            result['error'] = str(error)
        finally:
            result['duration_seconds'] = round(time.monotonic() - start, 2)
            (directory / 'result.json').write_text(json.dumps(result, indent=2))
            print(f"{result['status']}: {result.get('failed_step') or result['step']} — {directory}")
        return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
