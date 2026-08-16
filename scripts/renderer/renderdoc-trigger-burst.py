from pathlib import Path
import time

import renderdoc as rd


active_root_file = Path(r"C:\vulkanstorm\renderer-captures\active-renderdoc-path.txt")
capture_root = Path(active_root_file.read_text(encoding="utf-8").strip())
viewer_pid = int((capture_root / "viewer-pid.txt").read_text(encoding="utf-8").strip())
log_path = capture_root / "renderdoc-target-control.txt"


def log(message):
    with log_path.open("a", encoding="utf-8") as stream:
        stream.write(message + "\n")


ident = rd.EnumerateRemoteTargets("", 0)
target = None

while ident:
    candidate = rd.CreateTargetControl("", ident, "Vulkanstorm burst capture", False)
    if candidate is not None:
        pid = candidate.GetPID()
        log(f"ident={ident} pid={pid} api={candidate.GetAPI()} target={candidate.GetTarget()}")
        if pid == viewer_pid:
            target = candidate
            break
        candidate.Shutdown()
    ident = rd.EnumerateRemoteTargets("", ident)

if target is None:
    log(f"ERROR no target found for viewer pid={viewer_pid}")
    raise SystemExit(2)

log(f"triggering 6 consecutive frames for pid={viewer_pid}")
target.TriggerCapture(6)

captures = 0
deadline = time.monotonic() + 120.0
while captures < 6 and time.monotonic() < deadline and target.Connected():
    message = target.ReceiveMessage(None)
    if message is None:
        time.sleep(0.05)
        continue
    if message.type == rd.TargetControlMessageType.NewCapture:
        captures += 1
        destination = capture_root / f"zink-vulkan-burst-{captures:02d}.rdc"
        copied = target.CopyCapture(message.newCapture.captureId, str(destination))
        log(
            f"capture={captures} id={message.newCapture.captureId} "
            f"source={message.newCapture.path} destination={destination} copied={copied}"
        )
    elif message.type == rd.TargetControlMessageType.Disconnected:
        log("ERROR target disconnected")
        break

target.Shutdown()
log(f"complete captures={captures}")
raise SystemExit(0 if captures == 6 else 3)
