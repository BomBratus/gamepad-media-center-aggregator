"""Deterministic local Stremio addon and api.strem.io fixture server.

This server is intended for Linux UI integration tests. It uses only the Python
standard library and binds exclusively to loopback on an ephemeral port.
"""

from __future__ import annotations

import copy
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, unquote, urlsplit


class FixtureServer:
    """Serve a small Stremio catalog, account API, and local video file."""

    def __init__(self, media_path: Path):
        self.media_path = Path(media_path)
        self.requests: list[str] = []  # URL paths only; query strings are omitted.
        self.mode = "normal"  # catalog-only: "normal", "slow", or "error"
        self.library: list[dict[str, Any]] = [
            self._library_record(
                "tt9000001", "movie", "Fixture Movie", 12000, 120000
            ),
            self._library_record(
                "tt9000010",
                "series",
                "Fixture Series",
                12000,
                120000,
                video_id="tt9000010:1:1",
            ),
        ]
        self._lock = threading.RLock()
        self._httpd: ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None
        self.base = ""

    @staticmethod
    def _library_record(
        item_id: str,
        item_type: str,
        name: str,
        offset: int,
        duration: int,
        video_id: str | None = None,
    ) -> dict[str, Any]:
        state: dict[str, Any] = {
            "timeOffset": offset,
            "duration": duration,
            "flaggedWatched": 0,
            "watched": [],
            "videoId": video_id,
        }
        return {
            "_id": item_id,
            "type": item_type,
            "name": name,
            "removed": False,
            "temp": False,
            "state": state,
        }

    def start(self) -> "FixtureServer":
        if self._httpd is not None:
            return self

        fixture = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, _format: str, *_args: Any) -> None:
                return

            def _record_path(self) -> str:
                path = urlsplit(self.path).path
                with fixture._lock:
                    fixture.requests.append(path)
                return path

            def _send_json(self, value: Any, status: int = 200) -> None:
                payload = json.dumps(value, separators=(",", ":")).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(payload)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(payload)

            def _read_json(self) -> dict[str, Any]:
                try:
                    length = int(self.headers.get("Content-Length", "0"))
                    body = self.rfile.read(length)
                    value = json.loads(body or b"{}")
                    return value if isinstance(value, dict) else {}
                except (ValueError, json.JSONDecodeError):
                    return {}

            def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
                path = self._record_path()
                route = unquote(path)

                if route == "/manifest.json":
                    self._send_json(fixture._manifest())
                    return

                if route.startswith("/catalog/") and route.endswith(".json"):
                    if fixture.mode == "slow":
                        time.sleep(2.0)
                    elif fixture.mode == "error":
                        self._send_json({"error": {"message": "fixture catalog failure"}}, 500)
                        return
                    # Stremio catalog extras are appended as one path segment,
                    # e.g. /catalog/movie/tvtest/search=Fixture%20Movie&skip=1.json.
                    # Keep the raw path until after splitting so encoded values
                    # (including encoded slashes) remain part of their value.
                    raw_pieces = path.removesuffix(".json").split("/")
                    if len(raw_pieces) >= 4:
                        _, _, raw_type, raw_catalog_id, *raw_extra = raw_pieces
                        item_type = unquote(raw_type)
                        catalog_id = unquote(raw_catalog_id)
                        extras = parse_qs("&".join(raw_extra))
                        # Accept query-form extras too for simple external probes.
                        for key, values in parse_qs(urlsplit(self.path).query).items():
                            extras.setdefault(key, values)
                        try:
                            skip = max(0, int(extras.get("skip", ["0"])[0]))
                        except ValueError:
                            skip = 0
                        search = extras.get("search", [""])[0].casefold()
                        genre = extras.get("genre", [""])[0].casefold()
                        metas = fixture._catalog_items(item_type, catalog_id)
                        if search:
                            metas = [meta for meta in metas if search in meta["name"].casefold()]
                        if genre:
                            metas = [
                                meta for meta in metas
                                if genre in (value.casefold() for value in meta.get("genres", []))
                            ]
                        self._send_json({"metas": metas[skip:], "hasMore": False})
                    else:
                        self._send_json({"metas": [], "hasMore": False})
                    return

                if route.startswith("/meta/") and route.endswith(".json"):
                    pieces = route.removesuffix(".json").split("/")
                    if len(pieces) == 4:
                        _, _, item_type, item_id = pieces
                        meta = fixture._meta(item_type, item_id)
                        if meta is not None:
                            self._send_json({"meta": meta})
                            return
                    self._send_json({"meta": None}, 404)
                    return

                if route.startswith("/stream/") and route.endswith(".json"):
                    self._send_json({"streams": fixture._streams()})
                    return

                if route.startswith("/subtitles/") and route.endswith(".json"):
                    self._send_json({
                        "subtitles": [
                            {"id": "fixture-en", "lang": "eng", "url": fixture.base + "/subtitle.vtt"},
                            {"id": "fixture-it", "lang": "ita", "url": fixture.base + "/subtitle.vtt"},
                        ]
                    })
                    return

                if route == "/video.mp4":
                    self._send_media()
                    return

                if route == "/subtitle.vtt":
                    payload = b"WEBVTT\n\n00:00:00.000 --> 00:00:01.000\nFixture subtitle\n"
                    self.send_response(200)
                    self.send_header("Content-Type", "text/vtt; charset=utf-8")
                    self.send_header("Content-Length", str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                    return

                self._send_json({"error": "not found"}, 404)

            def _send_media(self) -> None:
                try:
                    size = fixture.media_path.stat().st_size
                    start, end = 0, size - 1
                    requested_range = self.headers.get("Range", "")
                    if requested_range.startswith("bytes="):
                        first, _, last = requested_range[6:].partition("-")
                        if first:
                            start = int(first)
                        if last:
                            end = min(int(last), size - 1)
                        if start >= size or end < start:
                            self.send_response(416)
                            self.send_header("Content-Range", f"bytes */{size}")
                            self.send_header("Content-Length", "0")
                            self.end_headers()
                            return
                    with fixture.media_path.open("rb") as media_file:
                        media_file.seek(start)
                        payload = media_file.read(end - start + 1)
                except (OSError, ValueError):
                    self._send_json({"error": "fixture video unavailable"}, 404)
                    return

                partial = bool(requested_range)
                self.send_response(206 if partial else 200)
                self.send_header("Content-Type", "video/mp4")
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Content-Length", str(len(payload)))
                if partial:
                    self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
                self.end_headers()
                self.wfile.write(payload)

            def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
                path = self._record_path()
                body = self._read_json()
                if path == "/api/addonCollectionGet":
                    self._send_json({"result": {"addons": [{"transportUrl": fixture.base + "/manifest.json"}]}})
                    return
                if path == "/api/datastoreGet":
                    with fixture._lock:
                        library = copy.deepcopy(fixture.library)
                    self._send_json({"result": library})
                    return
                if path == "/api/datastorePut":
                    changes = body.get("changes", [])
                    if isinstance(changes, dict):
                        changes = [changes]
                    if isinstance(changes, list):
                        with fixture._lock:
                            for change in changes:
                                if not isinstance(change, dict) or not change.get("_id"):
                                    continue
                                item_id = str(change["_id"])
                                current = next(
                                    (item for item in fixture.library if item.get("_id") == item_id),
                                    None,
                                )
                                if current is None:
                                    fixture.library.append(copy.deepcopy(change))
                                else:
                                    merged = copy.deepcopy(current)
                                    merged.update(copy.deepcopy(change))
                                    if isinstance(current.get("state"), dict) and isinstance(change.get("state"), dict):
                                        state = copy.deepcopy(current["state"])
                                        state.update(copy.deepcopy(change["state"]))
                                        merged["state"] = state
                                    current.clear()
                                    current.update(merged)
                    self._send_json({"result": {"updated": True}})
                    return
                self._send_json({"error": {"message": "not found"}}, 404)

        self._httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._httpd.daemon_threads = True
        self.base = f"http://127.0.0.1:{self._httpd.server_address[1]}"
        self._thread = threading.Thread(target=self._httpd.serve_forever, daemon=True)
        self._thread.start()
        return self

    def close(self) -> None:
        if self._httpd is None:
            return
        self._httpd.shutdown()
        self._httpd.server_close()
        if self._thread is not None:
            self._thread.join(timeout=2)
        self._httpd = None
        self._thread = None
        self.base = ""

    def __enter__(self) -> "FixtureServer":
        return self.start()

    def __exit__(self, _exc_type: Any, _exc: Any, _traceback: Any) -> None:
        self.close()

    @staticmethod
    def _catalog_items(item_type: str, catalog_id: str) -> list[dict[str, Any]]:
        if catalog_id != "tvtest":
            return []
        if item_type == "movie":
            return [
                {"id": "tt9000001", "type": "movie", "name": "Fixture Movie", "genres": ["Drama"]},
                {"id": "tt9000002", "type": "movie", "name": "Fixture Movie Two", "genres": ["Comedy"]},
                {"id": "tt9000003", "type": "movie", "name": "Fixture Movie Three", "genres": ["Drama"]},
            ]
        if item_type == "series":
            return [
                {"id": "tt9000010", "type": "series", "name": "Fixture Series", "genres": ["Drama"]},
                {"id": "tt9000011", "type": "series", "name": "Fixture Series Two", "genres": ["Comedy"]},
                {"id": "tt9000012", "type": "series", "name": "Fixture Series Three", "genres": ["Drama"]},
            ]
        return []

    @staticmethod
    def _manifest() -> dict[str, Any]:
        return {
            "id": "org.gmca.tvtest",
            "version": "1.0.0",
            "name": "GMCA TV Test Fixtures",
            "description": "Deterministic local fixture addon",
            "resources": [
                {"name": resource, "types": ["movie", "series"]}
                for resource in ("catalog", "meta", "stream", "subtitles")
            ],
            "types": ["movie", "series"],
            "idPrefixes": ["tt90000"],
            "catalogs": [
                {
                    "type": "movie", "id": "tvtest", "name": "TV Test Movies",
                    "extra": [
                        {"name": "search", "isRequired": False},
                        {"name": "genre", "options": ["Drama", "Comedy"], "isRequired": False},
                        {"name": "skip", "isRequired": False},
                    ],
                },
                {
                    "type": "series", "id": "tvtest", "name": "TV Test Series",
                    "extra": [
                        {"name": "search", "isRequired": False},
                        {"name": "genre", "options": ["Drama", "Comedy"], "isRequired": False},
                        {"name": "skip", "isRequired": False},
                    ],
                },
            ],
        }

    @staticmethod
    def _meta(item_type: str, item_id: str) -> dict[str, Any] | None:
        if item_type == "movie" and item_id in ("tt9000001", "tt9000002", "tt9000003"):
            names = {
                "tt9000001": "Fixture Movie",
                "tt9000002": "Fixture Movie Two",
                "tt9000003": "Fixture Movie Three",
            }
            return {
                "id": item_id,
                "type": "movie",
                "name": names[item_id],
                "releaseInfo": "2024",
                "runtime": "2h",
                "description": "A deterministic local playback fixture.",
            }
        if item_type == "series" and item_id in ("tt9000010", "tt9000011", "tt9000012"):
            series_index = int(item_id.removeprefix("tt")) - 9000010
            name = "Fixture Series" if series_index == 0 else f"Fixture Series {series_index + 1}"
            videos = [
                {
                    "id": f"{item_id}:{season}:{episode}",
                    "season": season,
                    "episode": episode,
                    "name": f"Fixture Series {series_index + 1} S{season:02d}E{episode:02d}",
                    "released": "2024-01-01",
                }
                for season in (1, 2)
                for episode in (1, 2)
            ]
            return {
                "id": item_id,
                "type": "series",
                "name": name,
                "releaseInfo": "2024",
                "description": "A deterministic local series fixture.",
                "videos": videos,
            }
        return None

    def _streams(self) -> list[dict[str, Any]]:
        video_url = self.base + "/video.mp4"
        return [
            {
                "name": "Fixture 720p ITA AUDIO",
                "title": "Fixture 720p ITA AUDIO",
                "url": video_url + "?source=ita",
            },
            {
                "name": "Fixture 720p SUB ITA",
                "title": "Fixture 720p SUB ITA",
                "url": video_url + "?source=sub",
            },
        ]
