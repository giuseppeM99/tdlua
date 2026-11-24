#!/usr/bin/env python3
"""Offline integration and comparison on the same HTTP fixture."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import gzip
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
HZ = os.sysconf("SC_CLK_TCK")


def save(path, data):
    temp = path.with_suffix(".tmp")
    temp.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    temp.replace(path)


def eventually(check, seconds=6):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        try:
            found = check()
            if found:
                return found
        except (OSError, http.client.HTTPException, AssertionError) as err:
            last = err
        time.sleep(0.008)
    raise AssertionError(f"state did not converge: {last}")


class UnixConnection(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__("localhost", timeout=5)
        self.path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)


class Server:
    def __init__(self, run, process, port=None, path=None):
        self.run, self.process, self.port, self.path = run, process, port, path

    def connection(self):
        if self.path:
            return UnixConnection(self.path)
        return http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)

    def request(self, path, conn=None):
        own = conn is None
        if own:
            conn = self.connection()
        before = time.perf_counter()
        try:
            conn.request("GET", path)
            response = conn.getresponse()
            body = response.read().decode()
            elapsed = time.perf_counter() - before
            assert response.status == 200, (path, response.status, body)
            return json.loads(body), elapsed
        finally:
            if own:
                conn.close()

    def get(self, path):
        return self.request(path)[0]

    def status(self):
        return self.get("/status")

    def workers(self):
        master = self.process.pid
        return [int(p) for p in Path(f"/proc/{master}/task/{master}/children").read_text().split()]

    def cpu(self):
        cpu = {}
        for pid in [self.process.pid, *self.workers()]:
            stats = Path(f"/proc/{pid}/stat").read_text().split(") ", 1)[1].split()
            cpu[pid] = (int(stats[11]) + int(stats[12])) / HZ
        return cpu

    def abort(self):
        with socket.create_connection(("127.0.0.1", self.port), timeout=5) as conn:
            conn.sendall(b"GET /pending HTTP/1.1\r\nHost: localhost\r\n\r\n")
            eventually(lambda: self.status()["active"] == 1)


def verify_maps(server, backend, workers):
    expected = os.environ[f"P2_{backend.upper()}_MODDIR"]
    assert "tdlua.so" not in Path(f"/proc/{server.process.pid}/maps").read_text()
    pids = server.workers()
    assert len(pids) == workers
    evidence = {}
    for pid in pids:
        maps = {line.split()[-1] for line in Path(f"/proc/{pid}/maps").read_text().splitlines()}
        lua = {p for p in maps if "/liblua" in p}
        td = {p for p in maps if p.endswith("/tdlua.so")}
        assert len(lua) == 1 and "libluajit" in next(iter(lua)), lua
        assert td == {str((Path(expected) / "tdlua.so").resolve())}, td
        assert not any(p.endswith("/probe.so") for p in maps)
        evidence[pid] = sorted(lua | td)
    return evidence


@contextmanager
def launch(root, name, backend, profile="official", workers=1, db=False, reference=None):
    run = root / name
    run.mkdir(exist_ok=False, parents=True)
    (run / "logs").mkdir()
    module = Path(os.environ[f"P2_{backend.upper()}_MODDIR"]).resolve()
    with tempfile.TemporaryDirectory(prefix="tdlua-p2-") as scratch:
        path = str(Path(scratch) / "http.sock") if db else None
        port = None
        if not db:
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                port = reservation.getsockname()[1]
        values = {"WORKERS": workers, "LISTEN": f"unix:{path}" if db else f"127.0.0.1:{port}",
                  "REPO": REPO, "TESTS": HERE, "MODULE": module,
                  "REFERENCE": f"{reference}/?.lua;" if reference else ""}
        config = (HERE / "nginx.conf.in").read_text()
        for key, value in values.items():
            config = config.replace(f"@{key}@", str(value))
        (run / "nginx.conf").write_text(config)
        env = {"P2_PROFILE": profile}
        if db:
            database_root = Path(scratch) / "database"
            database_root.mkdir(mode=0o700)
            env["P2_DBROOT"] = str(database_root)
        command = [os.environ["P2_OPENRESTY"], "-p", f"{run}/", "-c", "nginx.conf"]
        save(run / "config.json", {"command": command, "environment": env, "backend": backend,
                                  "profile": profile, "workers": workers})
        ok = False
        with (run / "stdio.log").open("w") as out:
            proc = subprocess.Popen(command, env={**os.environ, **env},
                                    stdout=out, stderr=subprocess.STDOUT)
            try:
                server = Server(run, proc, port, path)
                eventually(server.status)
                save(run / "maps.json", eventually(lambda: verify_maps(server, backend, workers)))
                yield server
                ok = True
            finally:
                if proc.poll() is None:
                    proc.send_signal(signal.SIGQUIT)
                    try:
                        proc.wait(timeout=6)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
                save(run / "exit.json", {"passed": ok, "nginx_exit": proc.returncode})
        assert proc.returncode == 0
        logs = (run / "error.log").read_text()
        assert "exited on signal" not in logs and "[error]" not in logs and "[alert]" not in logs, logs


def pending_stop(app, reload=False):
    with ThreadPoolExecutor(1) as pool:
        pending = pool.submit(app.get, "/pending")
        old = eventually(lambda: state if (state := app.status())["active"] == 1 else None)
        if reload:
            app.process.send_signal(signal.SIGHUP)
        else:
            assert app.get("/stop")["stopped"]
        answer = pending.result()
    assert answer["pid"] == old["pid"]
    assert answer["reason"] == ("worker shutdown" if reload else "stopped"), answer
    if reload:
        new = eventually(lambda: state if (state := app.status())["pid"] != old["pid"] else None)
        eventually(lambda: not Path(f"/proc/{old['pid']}").exists())
        assert app.get("/async")["pending"]
        return {"new_pid": new["pid"], **answer}
    state = app.status()
    assert state["active"] == 0 and state["notices"] == 1
    return answer


def integration(root, backend):
    with launch(root, f"runtime-{backend}", backend) as app:
        paths = ("/sync", "/async", "/async?client=2", "/task", "/composition",
                 "/errors", "/phases", "/timeout", "/multi", "/cleanup", "/kill",
                 "/kill?raw=1", "/descendant", "/descendant?cleanup=1", "/redirect", "/raw")
        evidence = {}
        for path in paths:
            evidence[path] = app.get(path)
            save(app.run / "responses.json", evidence)
        assert evidence["/sync"]["value"] == "f10611ee1f03f91ddae8185483a1ccfd3d664668"
        assert evidence["/composition"]["entities"] == 1
        assert evidence["/kill?raw=1"]["active"] == 1
        assert app.status()["active"] == 0
        before = app.status()
        app.abort()
        after = eventually(lambda: s if (s := app.status()).get("alarm_ready") else None)
        assert after["aborts"] == 1 and after["active"] == 0
        assert after["canceled"] == before["canceled"] + 1
        assert after["registered"] == after["delivered"] + after["canceled"]
        evidence["abort"] = after
        evidence["reload"] = pending_stop(app, reload=True)
        evidence["stop"] = pending_stop(app)
        save(app.run / "responses.json", evidence)

    for path, reason in (("/reentrant", "stopped"), ("/reentrant?fail=1", "error")):
        with launch(root, f"callback-{backend}-{reason}", backend) as app:
            result = app.get(path)
            status = app.status()
            assert result["reason"] == reason and status["notices"] == 1, result
            assert status["stop_reason"] == reason and status["active"] == 0
            if reason == "error":
                assert "raw update failure" in result["detail"]
            save(app.run / "responses.json", {"response": result, "status": status})


def database(root, backend):
    assert os.readlink("/proc/self/ns/net") != os.environ["P2_HOST_NETNS"]
    links = json.loads(subprocess.check_output(["ip", "-json", "link"]))
    routes = json.loads(subprocess.check_output(["ip", "-json", "route", "show", "table", "all"]))
    assert links and all("UP" not in entry["flags"] for entry in links) and not routes
    with launch(root, f"database-{backend}", backend, workers=2, db=True) as app:
        found = {}
        for _ in range(12):
            result = app.get("/database")
            found[result["pid"]] = result
            if len(found) == 2:
                break
        assert len(found) == 2 and len({v["path"] for v in found.values()}) == 2
        for result in found.values():
            assert result["reopened"]
            paths = sorted(p.name for p in Path(result["path"]).iterdir() if p.is_file())
            assert any(p.endswith(".sqlite") for p in paths), paths
            result["files"] = paths
        save(app.run / "responses.json", {"workers": found, "links": links, "routes": routes})


def sample_load(app, seconds, count):
    end = time.perf_counter() + seconds

    def client_load(index):
        conn = app.connection()
        samples = []
        try:
            while time.perf_counter() < end:
                result, elapsed = app.request(f"/async?client={index % 2 + 1}", conn)
                assert result["pending"]
                samples.append({"http_s": elapsed, "pid": result["pid"]})
        finally:
            conn.close()
        return samples

    before = time.perf_counter()
    with ThreadPoolExecutor(count) as pool:
        rows = [row for series in pool.map(client_load, range(count)) for row in series]
    return rows, time.perf_counter() - before


def benchmark(root, backend, workers, profile, trial, reference):
    with launch(root, f"bench-{backend}-{workers}-{profile}-{trial}", backend,
                profile=profile, workers=workers, reference=reference if profile != "official" else None) as app:
        warm, _ = sample_load(app, 0.07, 8)
        initial = app.status()
        cpu_before, idle_begin = app.cpu(), time.perf_counter()
        time.sleep(0.75 + 0.025 * trial)
        idle_end, cpu_after = time.perf_counter(), app.cpu()
        first, first_s = app.request("/async")
        idle_status = app.status()
        cpu_active_before = app.cpu()
        rows, wall = sample_load(app, 0.7, 16)
        cpu_active_after = app.cpu()
        assert first["pending"] and len({r["pid"] for r in rows}) == workers
        final = app.status()
        assert final["active"] == 0 and final["registered"] == final["delivered"]
        if workers == 1:
            assert final["registered"] == len(warm) + 1 + len(rows)
        result = {"backend": backend, "workers": workers, "profile": profile, "trial": trial,
                  "first_s": first_s, "initial_status": initial, "idle_status": idle_status,
                  "requests": rows, "wall_s": wall, "idle_s": idle_end - idle_begin,
                  "idle_cpu_s": sum(cpu_after.values()) - sum(cpu_before.values()),
                  "active_cpu_s": sum(cpu_active_after.values()) - sum(cpu_active_before.values())}
        with gzip.open(app.run / "samples.json.gz", "wt") as out:
            json.dump(result, out, separators=(",", ":"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("suite", choices=["integration", "database", "benchmark"])
    parser.add_argument("out", type=Path)
    parser.add_argument("--p1", type=Path)
    parser.add_argument("--trials", type=int, default=5)
    args = parser.parse_args()
    root = args.out.resolve()
    root.mkdir(parents=True, exist_ok=True)
    backends = ("json", "native")
    if args.suite == "benchmark":
        assert args.p1 and (args.p1 / "p1_timer.lua").is_file()
        base = ["p1", "official", "fixed"]
        jobs = []
        for trial in range(args.trials):
            offset = trial % len(base)
            profiles = base[offset:] + base[:offset]
            if trial % 2:
                profiles.reverse()
            for workers in (1, 2):
                for backend in backends if trial % 2 == 0 else backends[::-1]:
                    jobs.extend((backend, workers, profile, trial, args.p1.resolve()) for profile in profiles)
        test = benchmark
    else:
        jobs = [(backend,) for backend in backends]
        test = database if args.suite == "database" else integration
    save(root / "manifest.json", {"suite": args.suite, "cases": [list(map(str, job)) for job in jobs]})
    start = time.monotonic()
    for i, args in enumerate(jobs, 1):
        print(f"[{i}/{len(jobs)}] {test.__name__}", *args, flush=True)
        test(root, *args)
        elapsed = time.monotonic() - start
        save(root / "progress.json", {"done": i, "total": len(jobs), "seconds": elapsed,
                                      "eta_s": elapsed / i * (len(jobs) - i)})
    print(f"{test.__name__}: PASS", flush=True)


if __name__ == "__main__":
    main()
