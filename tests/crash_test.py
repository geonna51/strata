#!/usr/bin/env python3
import os
import random
import shutil
import signal
import subprocess
import sys
import time

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_dir = os.path.dirname(script_dir)

    bin_path = None
    if len(sys.argv) > 1 and os.path.exists(sys.argv[1]):
        bin_path = sys.argv[1]
    elif "CRASH_WRITER_BIN" in os.environ and os.path.exists(os.environ["CRASH_WRITER_BIN"]):
        bin_path = os.environ["CRASH_WRITER_BIN"]
    else:
        candidates = [
            os.path.join(project_dir, "build", "crash_writer"),
            os.path.join(project_dir, "build-asan", "crash_writer"),
            os.path.join(project_dir, "build-tsan", "crash_writer"),
            os.path.join(os.getcwd(), "crash_writer"),
            os.path.join(os.getcwd(), "build", "crash_writer"),
            os.path.join(os.getcwd(), "build-asan", "crash_writer"),
        ]
        for cand in candidates:
            if os.path.exists(cand):
                bin_path = cand
                break

    if not bin_path or not os.path.exists(bin_path):
        print("Error: crash_writer binary not found. Run cmake and build first.")
        sys.exit(1)

    db_path = "/tmp/strata_crash_torture_db"
    ack_file = "/tmp/strata_crash_ack.txt"

    num_rounds = 3
    print("=================================================================")
    print("STRATA CRASH RECOVERY TORTURE TEST")
    print(f"Running {num_rounds} rounds of SIGKILL (kill -9) during active writes")
    print("=================================================================")

    for r in range(1, num_rounds + 1):
        print(f"\n--- Round {r}/{num_rounds} ---")
        if os.path.exists(db_path):
            shutil.rmtree(db_path)
        if os.path.exists(ack_file):
            os.remove(ack_file)

        # Launch writer with sync=1 (fsync durability)
        proc = subprocess.Popen(
            [bin_path, "write", db_path, "100000", ack_file, "1"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )

        # Wait for writer to initialize
        ready = False
        while True:
            line = proc.stdout.readline()
            if "READY" in line:
                ready = True
                break
            if proc.poll() is not None:
                break

        if not ready:
            print("Writer failed to start")
            sys.exit(1)

        # Let it write a batch of keys (random sleep between 0.1 and 0.4 seconds)
        sleep_time = random.uniform(0.1, 0.4)
        time.sleep(sleep_time)

        # Deliver abrupt SIGKILL (kill -9)
        print(f"Sending SIGKILL (kill -9) to writer process (PID: {proc.pid})...")
        try:
            os.kill(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

        proc.communicate()
        print("Process abruptly terminated mid-write.")

        # Run verification step
        print("Restarting Strata and verifying all acknowledged writes...")
        verify_proc = subprocess.run(
            [bin_path, "verify", db_path, "100000", ack_file],
            capture_output=True,
            text=True
        )

        print(verify_proc.stdout)
        if verify_proc.stderr:
            print(verify_proc.stderr)

        if verify_proc.returncode != 0:
            print(f"FAILED: Round {r} detected data loss after crash!")
            sys.exit(2)

        print(f"Round {r} PASSED: 100% acknowledged data recovered. Zero data loss.")

    # Clean up
    if os.path.exists(db_path):
        shutil.rmtree(db_path)
    if os.path.exists(ack_file):
        os.remove(ack_file)

    print("\n=================================================================")
    print("ALL CRASH RECOVERY TESTS PASSED (Data loss: 0)")
    print("=================================================================")

if __name__ == "__main__":
    main()
