#!/usr/bin/env python3
"""Print the download URL for a package on a given platform from autobuild.xml.

Used by cmake on platforms without autobuild (e.g. FreeBSD) to fetch
platform-independent "common" archives (headers, fonts, data) directly.

Usage: fetch_autobuild.py <package> <autobuild.xml> [platform=common]
"""
import re
import sys

name = sys.argv[1]
xml_path = sys.argv[2]
platform = sys.argv[3] if len(sys.argv) > 3 else "common"

data = open(xml_path, encoding="utf-8", errors="replace").read()
idx = data.find(">" + name + "<")
url = ""
if idx >= 0:
    pat = r"<string>(https://[^<]+" + re.escape(platform) + r"[^<]+tar\.(?:zst|bz2|xz|gz))</string>"
    m = re.search(pat, data[idx:idx + 4000])
    if m:
        url = m.group(1)
print(url)
