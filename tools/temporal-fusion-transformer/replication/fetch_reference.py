"""Fetch immutable original TFT source, retaining upstream notices and hashes."""
import hashlib
import json
from pathlib import Path
import urllib.request

REVISION = "5b09c22d73a9d35eb6c5d2a99b95677a45053466"
FILES = [
    "README.md", "requirements.txt", "run.sh",
    "libs/tft_model.py", "libs/utils.py", "libs/hyperparam_opt.py",
    "data_formatters/base.py", "data_formatters/electricity.py",
    "data_formatters/traffic.py", "data_formatters/volatility.py",
    "data_formatters/favorita.py", "expt_settings/configs.py",
    "script_download_data.py", "script_train_fixed_params.py",
]
EXPECTED = {
    "libs/tft_model.py": "53f0046e12b9b79ffea096516b2278774e65d760df684f97600341b9abef9a2c",
    "libs/utils.py": "0dfe603977033233018cd4d91666f5d8c0dc3c484705816d2fbea898fc0aa7c2",
    "script_download_data.py": "19308e2dea98f45518eb1b37d211492115945230a49ca9cd73ad49de3ca73f4e",
    "script_train_fixed_params.py": "aee8112fa684b8086102b15580570e38e38470fbfcd5fef0156e20c0e0c9858c",
}


def main():
    root = Path(__file__).resolve().parents[3]
    destination = root / ".build/reference/tft"
    manifest = {"revision": REVISION, "files": {}}
    for name in FILES:
        url = "https://raw.githubusercontent.com/google-research/google-research/{}/tft/{}".format(REVISION, name)
        with urllib.request.urlopen(url, timeout=90) as response:
            content = response.read()
        digest = hashlib.sha256(content).hexdigest()
        if name in EXPECTED and digest != EXPECTED[name]:
            raise RuntimeError("Reference checksum mismatch: " + name)
        path = destination / name
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.exists():
            if path.read_bytes() != content:
                raise RuntimeError("Existing reference differs; preserved: " + str(path))
        else:
            with path.open("xb") as stream:
                stream.write(content)
        manifest["files"][name] = {"url": url, "sha256": digest, "bytes": len(content)}
        print("Verified", name, flush=True)
    manifest_path = destination.parent / "source-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print("Reference source ready at", destination)


if __name__ == "__main__":
    main()
