import os
import sys
import json
import time
import zipfile
import urllib.request
import subprocess

CONFIG_FILE = "d:\\thien\\01_DuAn_Code\\squeezelite-esp32\\builder_config.json"

class NoAuthRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new_req = urllib.request.HTTPRedirectHandler.redirect_request(self, req, fp, code, msg, headers, newurl)
        if new_req:
            new_req.remove_header("Authorization")
            new_req.remove_header("authorization")
        return new_req

def find_file(directory, filename):
    for root, dirs, files in os.walk(directory):
        if filename in files:
            return os.path.join(root, filename)
    return None

def main():
    with open(CONFIG_FILE, "r") as f:
        config = json.load(f)

    repo_path = config["github_repo"].split("github.com/")[-1].replace(".git", "")
    headers = {
        "Authorization": f"Bearer {config['github_token']}",
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "ESP32-Builder-Agent"
    }

    print(">>> Locating latest GitHub Actions run for commit d18e02cd...")
    run_id = None
    for attempt in range(15):
        try:
            req = urllib.request.Request(f"https://api.github.com/repos/{repo_path}/actions/runs?per_page=5", headers=headers)
            with urllib.request.urlopen(req) as resp:
                data = json.loads(resp.read().decode())
                runs = data.get("workflow_runs", [])
                for r in runs:
                    if r.get("head_sha", "").startswith("d18e02c"):
                        run_id = r["id"]
                        print(f"Found Run ID: {run_id} ({r.get('name')}) - Status: {r.get('status')}")
                        break
        except Exception as e:
            print("Query error:", e)
        if run_id:
            break
        time.sleep(3)

    if not run_id:
        print("[ERROR] Could not find workflow run for latest commit!")
        sys.exit(1)

    # Wait for completion
    completed = False
    for i in range(45): # up to 45 * 10s = 450s
        try:
            req = urllib.request.Request(f"https://api.github.com/repos/{repo_path}/actions/runs/{run_id}", headers=headers)
            with urllib.request.urlopen(req) as response:
                run = json.loads(response.read().decode())
                status = run.get("status")
                conclusion = run.get("conclusion")
                print(f"[{i+1}/45] Status: {status}, Conclusion: {conclusion}")
                if status == "completed":
                    if conclusion == "success":
                        completed = True
                        break
                    else:
                        print(f"[ERROR] Build failed with conclusion: {conclusion}")
                        sys.exit(1)
        except Exception as e:
            print(f"Poll error: {e}")
        time.sleep(10)

    if not completed:
        print("[ERROR] Timed out waiting for build!")
        sys.exit(1)

    print("\n>>> Fetching build artifacts...")
    req = urllib.request.Request(f"https://api.github.com/repos/{repo_path}/actions/runs/{run_id}/artifacts", headers=headers)
    with urllib.request.urlopen(req) as response:
        artifacts = json.loads(response.read().decode()).get("artifacts", [])

    if not artifacts:
        print("[ERROR] No artifacts found for this run!")
        sys.exit(1)

    target_artifact = artifacts[0]
    for a in artifacts:
        if "s3" in a["name"].lower() or "4m" in a["name"].lower():
            target_artifact = a
            break

    print(f"Downloading artifact: {target_artifact['name']} (ID: {target_artifact['id']})...")
    dl_url = target_artifact["archive_download_url"]
    opener = urllib.request.build_opener(NoAuthRedirectHandler)
    req = urllib.request.Request(dl_url, headers=headers)

    build_dir = "d:\\thien\\01_DuAn_Code\\squeezelite-esp32\\build_artifacts"
    os.makedirs(build_dir, exist_ok=True)
    zip_path = os.path.join(build_dir, f"artifact_{run_id}.zip")

    with opener.open(req) as resp, open(zip_path, "wb") as out_file:
        out_file.write(resp.read())

    print(f"Artifact downloaded ({os.path.getsize(zip_path)} bytes). Extracting...")
    extract_dir = os.path.join(build_dir, f"extracted_{run_id}")
    with zipfile.ZipFile(zip_path, 'r') as zip_ref:
        zip_ref.extractall(extract_dir)

    # Nested zip check
    for item in os.listdir(extract_dir):
        if item.endswith(".zip"):
            with zipfile.ZipFile(os.path.join(extract_dir, item), 'r') as nested_ref:
                nested_ref.extractall(extract_dir)

    bootloader = find_file(extract_dir, "bootloader.bin")
    partition = find_file(extract_dir, "partition-table.bin")
    recovery = find_file(extract_dir, "recovery.bin")
    squeezelite = find_file(extract_dir, "squeezelite.bin")

    print("Found binaries:")
    print(" Bootloader:", bootloader)
    print(" Partition :", partition)
    print(" Recovery  :", recovery)
    print(" Squeeze   :", squeezelite)

    if not squeezelite:
        print("[ERROR] squeezelite.bin not found in artifact!")
        sys.exit(1)

    # Flash without wiping otadata
    print(f"\n>>> Flashing to ESP32-S3 via {config['com_port']} at {config['baud_rate']} baud...")
    flash_cmd = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32s3",
        "--port", config["com_port"],
        "--baud", config["baud_rate"],
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
        "0x0000", bootloader,
        "0x8000", partition,
        "0x10000", recovery,
        "0x150000", squeezelite
    ]

    res = subprocess.run(flash_cmd)
    if res.returncode != 0:
        print("[ERROR] Flashing failed!")
        sys.exit(1)

    print("\n>>> Flashing SUCCESS! Waiting 5s for boot...")
    time.sleep(5)

    import telnetlib
    try:
        tn = telnetlib.Telnet("192.168.1.19", 23, timeout=5)
        time.sleep(1)
        tn.write(b"\n")
        out = tn.read_very_eager().decode('utf-8', errors='replace')
        if "recovery" in out.lower():
            print("Device is in Recovery mode, switching to Squeezelite...")
            tn.write(b"restart_ota\n")
            time.sleep(3)
        else:
            print("Device booted straight into Squeezelite!")
        tn.close()
    except Exception as e:
        print("Telnet post-check note:", e)

if __name__ == "__main__":
    main()
