import os
import sys
import json
import time
import zipfile
import urllib.request
import subprocess

TARGET_RUN_ID = 40442683496
CONFIG_FILE = "builder_config.json"

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
    print("=========================================")
    print("  Monitoring Squeezelite Fix Run         ")
    print(f"  Target Run ID: {TARGET_RUN_ID}         ")
    print("=========================================")

    with open(CONFIG_FILE, "r") as f:
        config = json.load(f)

    repo_path = config["github_repo"].split("github.com/")[-1].replace(".git", "")
    headers = {
        "Authorization": f"Bearer {config['github_token']}",
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "ESP32-Builder-Agent"
    }

    # 1. Wait for completion
    completed = False
    for i in range(120): # up to 20 mins
        time.sleep(10)
        try:
            req = urllib.request.Request(f"https://api.github.com/repos/{repo_path}/actions/runs/{TARGET_RUN_ID}", headers=headers)
            with urllib.request.urlopen(req) as response:
                run = json.loads(response.read().decode())
                status = run.get("status")
                conclusion = run.get("conclusion")
                print(f"[{i*10}s] Build status: {status}, Conclusion: {conclusion or 'Running...'}")
                if status == "completed":
                    completed = True
                    if conclusion != "success":
                        print(f"[ERROR] Build failed with conclusion: {conclusion}")
                        sys.exit(1)
                    break
        except Exception as e:
            print("Warning fetching run status:", e)

    if not completed:
        print("[ERROR] Build timed out.")
        sys.exit(1)

    # 2. Get artifact download URL
    print("Build successful! Fetching artifact link...")
    artifacts_url = f"https://api.github.com/repos/{repo_path}/actions/runs/{TARGET_RUN_ID}/artifacts"
    req = urllib.request.Request(artifacts_url, headers=headers)
    with urllib.request.urlopen(req) as response:
        data = json.loads(response.read().decode())
        if not data.get("artifacts"):
            print("[ERROR] No artifacts found!")
            sys.exit(1)
        artifact = data["artifacts"][0]
        download_url = artifact["archive_download_url"]

    # 3. Download and extract
    print(f"Downloading firmware ({artifact['name']})...")
    opener = urllib.request.build_opener(NoAuthRedirectHandler)
    req = urllib.request.Request(download_url, headers=headers)
    os.makedirs("firmware_download", exist_ok=True)
    zip_dest = os.path.join("firmware_download", "firmware_fix.zip")
    with opener.open(req) as response, open(zip_dest, "wb") as out_file:
        out_file.write(response.read())

    print("Extracting firmware binaries...")
    extract_dir = os.path.join("firmware_download", "extracted_fix")
    os.makedirs(extract_dir, exist_ok=True)
    with zipfile.ZipFile(zip_dest, 'r') as zip_ref:
        zip_ref.extractall(extract_dir)

    bootloader = find_file(extract_dir, "bootloader.bin")
    partition = find_file(extract_dir, "partition-table.bin")
    otadata = find_file(extract_dir, "ota_data_initial.bin")
    recovery = find_file(extract_dir, "recovery.bin")
    squeezelite = find_file(extract_dir, "squeezelite.bin")

    # 4. Erase NVS partitions to wipe old WiFi password & old display config
    print("\n>>> Erasing old NVS settings on ESP32-S3...")
    erase_cmd1 = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32s3",
        "--port", config["com_port"],
        "--baud", config["baud_rate"],
        "erase_region", "0x9000", "0x4000"
    ]
    subprocess.run(erase_cmd1)

    erase_cmd2 = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32s3",
        "--port", config["com_port"],
        "--baud", config["baud_rate"],
        "erase_region", "0x3f0000", "0x10000"
    ]
    subprocess.run(erase_cmd2)

    # 5. Flash new firmware
    print("\n>>> Flashing new Squeezelite firmware...")
    flash_cmd = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32s3",
        "--port", config["com_port"],
        "--baud", config["baud_rate"],
        "write_flash",
        "0x0000", bootloader,
        "0x8000", partition,
        "0xd000", otadata,
        "0x10000", recovery,
        "0x150000", squeezelite
    ]
    res = subprocess.run(flash_cmd)
    if res.returncode == 0:
        print("\n>>> SUCCESS! New firmware flashed with fix applied!")
    else:
        print("\n[ERROR] Flashing failed.")
        sys.exit(1)

if __name__ == "__main__":
    main()
