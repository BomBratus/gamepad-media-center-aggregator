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
from fixtures import FixtureServer

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / 'build-tvtest'
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
        if start_ticks(pid) != data['start_ticks'] or Path(f'/proc/{pid}/exe').resolve() != BUILD / 'GMCA':
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
        self.step = 'launch'
        self.snapshots = 0
        self.peak_rss = 0
        self.boot = time.monotonic()
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
        self.proc = subprocess.Popen([str(binary)], cwd=binary.parent, env=env,
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
        if reply.get('error') == 'invalid test command':
            raise RuntimeError(reply['error'])
        return reply

    def state(self):
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
            except (FileNotFoundError, ConnectionRefusedError):
                if self.proc.poll() is not None:
                    raise RuntimeError(f'GMCA exited/crashed: {self.proc.returncode}')
            time.sleep(.15)
        if last:
            (self.directory / 'failure-state.json').write_text(json.dumps(last, indent=2))
        raise AssertionError('timeout: ' + step)

    def press(self, button):
        self.call(command='button', button=button, pressed=True)
        time.sleep(.10)
        self.call(command='button', button=button, pressed=False)
        time.sleep(.25)

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
        (self.directory / f'{self.snapshots:02}-{name}.json').write_text(json.dumps(state, indent=2))
        subprocess.run(['scrot', '-o', str(self.directory / f'{self.snapshots:02}-{name}.png')], check=True)
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


def profile(path, base, live=None):
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
    state = app.wait(lambda s: s.get('focus') and s.get('controllers', 0) > 0 and
                     any(n.get('id') == 'tab/home' for n in nodes(s.get('tree', {}))) and not s['loading'], 'boot to Stremio home')
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
    boot(app)
    if name in ('smoke', 'navigation', 'live'):
        navigation(app)
    if name == 'navigation':
        return
    if name == 'live':
        # Read-only live integration uses the real account/addons; never starts playback.
        sidebar(app, 'lib/movie')
        app.press('right')
        app.wait(lambda s: not s['loading'] and bool(s['focus']), 'live movies catalog', timeout=60)
        app.checkpoint('live-movies')
        return
    # Concrete scenario functions are below; every route observes real UI state.
    selected = ('continue-watching', 'resume', 'movies', 'watched', 'series', 'source-picker', 'search', 'error-loading') if name == 'smoke' else (name,)
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
        if app.state().get('depth', 0) <= 1 and not any(contains(app, cls) for cls in ('MediaMovie', 'MediaSeries', 'MediaSeason')):
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
    app.wait(lambda s: contains(app, 'MediaSeason', s) and not s['loading'], 'season episodes')
    app.checkpoint('episodes')
    return


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
    app.wait(lambda s: s['source_picker'], 'source picker reopen')
    app.press('a')
    app.wait(lambda s: s['player'] and s.get('duration_seconds', 0) > 0 and not s.get('player_stopped'), 'source confirm opens playing mpv', timeout=30)
    app.checkpoint('player')
    app.press('b')
    app.wait(lambda s: not s['player'], 'player Back')
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
    app.press('back')
    state = app.wait(lambda s: not s['loading'] and any(n.get('continue_watching') and any(i['id'] == 'series:tt9000010' and i['key'] == 'series:tt9000010:1:2' for i in n.get('media_items', [])) for n in flat(app, s)), 'completed episode advances to next')
    app.checkpoint('continue-next-episode')
    with fixture._lock:
        record['state']['timeOffset'] = 12000
    app.press('back')
    app.wait(lambda s: not s['loading'], 'partial checkpoint restored')


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
        app.press('b')
        app.wait(lambda s: not s['player'], 'Resume player closed')
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
        app.press('b')
        app.wait(lambda s: not s['player'], 'Restart player closed')
    home(app)


def watched(app, fixture):
    catalog(app, 'movie')
    app.seek('movie:tt9000001', ['down', 'left'], limit=10)
    app.press('x')
    app.wait(lambda s: contains(app, 'ContextMenu', s), 'watched context menu')
    app.checkpoint('watched-menu')
    # Locate the focused menu item by its visible label, never assume menu order.
    for _ in range(14):
        state = app.state()
        focus_ids = app.ids(state)
        focus_tree = next((n for n in flat(app, state) if n.get('id') and n['id'] in focus_ids), {})
        texts = [n.get('text', '') for n in nodes(focus_tree)]
        if any('watched' in t.lower() or 'played' in t.lower() for t in texts):
            app.press('a')
            break
        app.press('down')
    else:
        raise AssertionError('watched action unreachable')
    app.wait(lambda s: not contains(app, 'ContextMenu', s), 'watched action closes menu')
    app.wait(lambda s: any(any(i['id'] == 'movie:tt9000001' and i['watched'] for i in n.get('media_items', [])) for n in flat(app, s)), 'watched card semantic state')
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        with fixture._lock:
            if any(r['_id'] == 'tt9000001' and r.get('state', {}).get('flaggedWatched') for r in fixture.library):
                break
        time.sleep(.1)
    else:
        raise AssertionError('watched not persisted to fixture datastore')
    app.checkpoint('watched')
    home(app)


def search(app, fixture):
    home(app)
    sidebar(app, 'tab/search')
    app.press('right')
    app.wait(lambda s: contains(app, 'SearchTab', s), 'controller search')
    app.checkpoint('search')
    # TV search has an on-screen keyboard, exercised with controller confirmation.
    app.seek('tv/search/key/A', ['down'], limit=8)
    app.press('a')
    app.press('start')
    app.wait(lambda s: not s['loading'] and bool(s['focus']), 'search results preserve focus')
    state = app.checkpoint('search-input')
    assert any(n.get('id') == 'tv/search/input' and n.get('text') == 'A' for n in flat(app, state)), 'controller keyboard failed to type A'
    ids = [i['id'] for n in flat(app, state) for i in n.get('media_items', [])]
    assert ids and len(ids) == len(set(ids)), 'search missing results or immediately duplicated' 
    home(app)


def error_loading(app, fixture):
    catalog(app, 'movie')
    app.press('r1')
    app.press('r1')
    app.wait(lambda s: contains(app, 'GenresTab', s) and not s['loading'], 'Genres controller tab')
    app.seek('genre/Drama', ['down', 'left'], limit=10)
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
    app.seek('genre/Comedy', ['right'], limit=4)
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('scenario', nargs='?', default='smoke', choices=['smoke', 'navigation', 'movies', 'series', 'source-picker', 'continue-watching', 'resume', 'watched', 'search', 'error-loading', 'live'])
    parser.add_argument('--runtime-only', action='store_true', help='reuse already validated build for scenario debugging')
    parser.add_argument('--sync', action='store_true', help='fetch origin refs; never reset or merge local changes')
    args = parser.parse_args()
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
        for old in sorted(RESULTS.glob('run-*'))[:-4]:
            if old.is_dir() and not old.is_symlink():
                shutil.rmtree(old)
        directory = RESULTS / ('run-' + time.strftime('%Y%m%d-%H%M%S') + '-' + args.scenario)
        directory.mkdir()
        result = {'scenario': args.scenario, 'status': 'FAIL', 'step': 'dependencies', 'ps4_validated': False}
        app = fixture = None
        start = time.monotonic()
        try:
            for tool in ('git', 'c++', 'cmake', 'ninja', 'pkg-config', 'ffmpeg', 'scrot', 'xrandr'):
                if not shutil.which(tool):
                    raise RuntimeError('missing dependency: ' + tool)
            for package in ('sdl2', 'mpv', 'libavformat', 'libcurl'):
                subprocess.run(['pkg-config', '--exists', package], check=True)
            subprocess.run(['xrandr', '--current'], env=dict(os.environ, DISPLAY=os.environ.get('DISPLAY', ':0'), XAUTHORITY=os.environ.get('XAUTHORITY', '/home/michele/.Xauthority')), stdout=subprocess.DEVNULL, check=True)
            if args.sync:
                result['step'] = 'fetch'
                command(['git', 'fetch', 'origin', '--prune'], directory / 'build.log')
            result['commit'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
            result['dirty'] = bool(subprocess.check_output(['git', 'status', '--porcelain', '--ignore-submodules=dirty'], cwd=ROOT))
            if not args.runtime_only:
                result['step'] = 'submodules'
                command(['git', 'submodule', 'update', '--init', '--recursive'], directory / 'build.log')
                patch('borealis-fixes.patch')
                patch('borealis-test-observers.patch')
                result['step'] = 'unit tests'
                command([str(ROOT / 'tests/run.sh')], directory / 'unit-tests.log')
                result['step'] = 'configure'
                configure = ['cmake', '-S', str(ROOT), '-B', str(BUILD), '-G', 'Ninja', '-DPLATFORM_DESKTOP=ON', '-DUSE_SDL2=ON', '-DUSE_SYSTEM_SDL2=ON', '-DGMCA_STREMIO_ONLY=ON', '-DGMCA_TEST_HARNESS=ON']
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
            with tempfile.TemporaryDirectory(prefix='gmca-tvtest-') as temporary:
                path = Path(temporary) / 'config'
                if args.scenario == 'live':
                    live = Path(os.environ.get('GMCA_TEST_LIVE_CONFIG', str(Path.home() / '.config/GMCA/config.json')))
                    secrets = profile(path, None, live)
                    base = None
                else:
                    fixture = FixtureServer(media)
                    fixture.start()
                    base = fixture.base
                    secrets = profile(path, base)
                stop_previous()
                app = Runtime(BUILD / 'GMCA', directory, path, base, secrets)
                result['step'] = 'runtime'
                try:
                    scenarios(app, args.scenario, fixture)
                    result['status'] = 'PASS'
                    result['step'] = 'complete'
                finally:
                    if result['status'] != 'PASS':
                        try:
                            app.checkpoint('failure')
                        except Exception:
                            subprocess.run(['scrot', '-o', str(directory / 'failure.png')], check=False)
                    result['failed_step'] = app.step if result['status'] != 'PASS' else None
                    result['peak_rss_kib'] = app.peak_rss
                    result['scenario_seconds'] = getattr(app, 'timings', {})
                    result['boot_seconds'] = getattr(app, 'boot_seconds', None)
                    app.close()
                    result['exit_code'] = app.proc.returncode
                    if app.proc.returncode != 0:
                        result['status'] = 'FAIL'
                        result['step'] = 'GMCA exit'
                if fixture:
                    (directory / 'requests.json').write_text(json.dumps(fixture.requests, indent=2))
        except Exception as error:
            result['error'] = str(error)
        finally:
            if fixture:
                fixture.close()
            result['duration_seconds'] = round(time.monotonic() - start, 2)
            (directory / 'result.json').write_text(json.dumps(result, indent=2))
            print(f"{result['status']}: {result.get('failed_step') or result['step']} — {directory}")
        return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())
