import csv
import os
import subprocess
import sys

Import("env")


def parse_int(value):
    token = value.strip()
    if token.lower().startswith("0x"):
        return int(token, 16)
    if token[-1:].lower() == "k":
        return int(token[:-1]) * 1024
    if token[-1:].lower() == "m":
        return int(token[:-1]) * 1024 * 1024
    return int(token)


def parse_partitions(csv_path):
    partitions = []
    next_offset = 0x9000
    with open(csv_path, newline="", encoding="utf-8") as fp:
        reader = csv.reader(fp)
        for row in reader:
            if not row:
                continue
            if row[0].strip().startswith("#"):
                continue
            row = [c.strip() for c in row]
            if len(row) < 5:
                continue
            p_type = row[1]
            boundary = 0x10000 if p_type in ("0", "app") else 4
            offset = parse_int(row[3]) if row[3] else (next_offset + boundary - 1) & ~(boundary - 1)
            size = parse_int(row[4])
            partitions.append(
                {
                    "name": row[0],
                    "type": p_type,
                    "subtype": row[2],
                    "offset": offset,
                    "size": size,
                }
            )
            next_offset = offset + size
    return partitions


def find_partition(partitions, name):
    for part in partitions:
        if part["name"] == name:
            return part
    return None


project_dir = env.subst("$PROJECT_DIR")
build_dir = env.subst("$BUILD_DIR")
partitions_csv = os.path.join(project_dir, "partitions.csv")

custom_partitions_enabled = os.path.isfile(partitions_csv)
all_parts = []
littlefs_part = None

if custom_partitions_enabled:
    env.Replace(PARTITIONS_TABLE_CSV=partitions_csv)

    all_parts = parse_partitions(partitions_csv)
    app_part = find_partition(all_parts, "factory")
    if app_part:
        env.BoardConfig().update("upload.maximum_size", app_part["size"])

    littlefs_part = find_partition(all_parts, "littlefs")
    if littlefs_part:
        littlefs_offset = "0x%x" % littlefs_part["offset"]
        littlefs_image = os.path.join(build_dir, "littlefs.bin")
        # Keep upload flags in sync because this script runs after main.py computed flags.
        env.AppendUnique(UPLOADERFLAGS=[littlefs_offset, littlefs_image])
else:
    print("Warning: partitions.csv not found, skipping custom partition hooks")


def generate_flash_artifacts(source, target, env):
    if not custom_partitions_enabled:
        return

    framework_dir = env.PioPlatform().get_package_dir("framework-espidf")
    gen_esp32part = os.path.join(
        framework_dir, "components", "partition_table", "gen_esp32part.py"
    )
    python_exe = env.subst("$PYTHONEXE")
    partition_offset = env.BoardConfig().get("upload.partition_table_offset", "0x8000")
    flash_size = env.BoardConfig().get("upload.flash_size", "4MB")
    partitions_bin = os.path.join(build_dir, "partitions.bin")

    subprocess.check_call(
        [
            python_exe,
            gen_esp32part,
            "-q",
            "--offset",
            str(partition_offset),
            "--flash-size",
            str(flash_size),
            partitions_csv,
            partitions_bin,
        ]
    )

    if not littlefs_part:
        return

    tool_dir = env.PioPlatform().get_package_dir("tool-mklittlefs")
    tool_name = "mklittlefs.exe" if sys.platform.startswith("win") else "mklittlefs"
    mklittlefs = os.path.join(tool_dir, tool_name)
    littlefs_dir = os.path.join(project_dir, "littlefs")
    littlefs_image = os.path.join(build_dir, "littlefs.bin")

    subprocess.check_call(
        [
            mklittlefs,
            "-c",
            littlefs_dir,
            "-b",
            "4096",
            "-p",
            "256",
            "-s",
            str(littlefs_part["size"]),
            littlefs_image,
        ]
    )


for action_target in ("checkprogsize", "upload", "uploadfs"):
    env.AddPreAction(action_target, generate_flash_artifacts)
