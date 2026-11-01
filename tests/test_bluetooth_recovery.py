"""Exercise the actual embedded recovery script without touching host Bluetooth."""
import json
import pathlib
import re
import subprocess
import tempfile
import time

source = (pathlib.Path(__file__).parents[1] / "source/Showcase.m").read_text()
block = source.split("NSString *script =", 1)[1].split("NSDictionary *job", 1)[0]
script = "".join(json.loads(s) for s in re.findall(r'"(?:\\.|[^"\\])*"', block))
with tempfile.TemporaryDirectory(prefix="bluetooth recovery ") as directory:
    root = pathlib.Path(directory)
    log, ready, socket = (root / name for name in ("calls", "ready", "socket"))
    command = root / "mock command"
    command.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "' + str(log) + '"\n')
    command.chmod(0o755)
    socket.touch()
    owner = subprocess.Popen(["sleep", "30"])
    guard = subprocess.Popen(["/bin/sh", "-c", script, "guard", str(owner.pid),
                              str(command), "BTstack plist", "BlueTool plist",
                              "bluetoothd plist", str(ready), str(command), str(socket)])
    try:
        for _ in range(100):
            if ready.exists():
                break
            time.sleep(.01)
        assert ready.exists(), "Guard did not arm"
        assert not log.exists(), "Guard touched Bluetooth while app was alive"
        owner.kill()
        owner.wait()
        assert guard.wait(timeout=5) == 0
        assert log.read_text().splitlines() == [
            "-TERM CarDisplaySim", "unload BTstack plist",
            "load BlueTool plist", "load bluetoothd plist"]
        assert not ready.exists() and not socket.exists()
    finally:
        for process in (owner, guard):
            if process.poll() is None:
                process.kill()
                process.wait()
print("Bluetooth crash recovery passed")
