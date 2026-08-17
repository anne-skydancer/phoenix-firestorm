from pathlib import Path

import renderdoc as rd

names = []
for name in (
    "EnumerateRemoteTargets",
    "CreateTargetControl",
    "TargetControl",
):
    value = getattr(rd, name)
    names.append(name + "=" + str(getattr(value, "__doc__", None)))
Path(r"C:\vulkanstorm\renderer-captures\renderdoc-api-probe.txt").write_text(
    "\n".join(names), encoding="utf-8"
)

raise SystemExit(0)
