#!/usr/bin/env python3
"""Exercise transitive calls, explicit unlock/relock, and deferred lambda handling."""

import json
import subprocess
import sys
import tempfile
from pathlib import Path


SOURCE = r"""
namespace std {
struct mutex {};
template <class T> struct lock_guard { explicit lock_guard(T&); ~lock_guard(); };
template <class T> struct unique_lock {
    explicit unique_lock(T&); ~unique_lock(); void unlock(); void lock();
};
}
void Publish();
void relay() { Publish(); }
void protected_call(std::mutex& mutex) {
    std::lock_guard guard(mutex);
    relay();
}
void unlocked_call(std::mutex& mutex) {
    std::unique_lock lock(mutex);
    lock.unlock();
    relay();
}
void reprotected_call(std::mutex& mutex) {
    std::unique_lock lock(mutex);
    lock.unlock();
    lock.lock();
    relay();
}
void deferred_call(std::mutex& mutex) {
    std::lock_guard guard(mutex);
    auto later = [] { Publish(); };
    (void)later;
}
using Callback = void (*)();
void callback_under_lock(std::mutex& mutex, Callback callback) {
    std::lock_guard guard(mutex);
    callback();
}
"""


def main() -> None:
    audit = Path(__file__).with_name("audit-owner-locks.py")
    with tempfile.TemporaryDirectory(prefix="apc-owner-lock-audit-") as directory:
        root = Path(directory)
        source = root / "fixture.cpp"
        source.write_text(SOURCE, encoding="utf-8")
        database = root / "compile_commands.json"
        database.write_text(json.dumps([{"directory": str(root), "file": str(source),
                                         "command": f'clang-cl.exe /std:c++latest /TP "{source}"'}]), encoding="utf-8")
        report = root / "report.json"
        result = subprocess.run([sys.executable, "-B", str(audit), str(database), "--output", str(report)],
                                check=False)
        data = json.loads(report.read_text(encoding="utf-8"))
        assert result.returncode == 0, data["diagnosticErrors"]
        findings = data["findings"]
        owners = {finding["function"] for finding in findings}
        assert "protected_call" in owners, owners
        assert "reprotected_call" in owners, owners
        assert "callback_under_lock" in owners, owners
        assert "unlocked_call" not in owners, owners
        assert "deferred_call" not in owners, owners
        print("owner-lock audit fixture passed")


if __name__ == "__main__":
    main()
