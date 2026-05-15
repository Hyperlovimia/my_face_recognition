#!/usr/bin/env python3

import argparse
import getpass
import hashlib
import secrets
import sys


ITERATIONS = 120000
SALT_BYTES = 16
PIN_LEN = 6


def is_valid_pin(pin: str) -> bool:
    return len(pin) == PIN_LEN and pin.isdigit()


def prompt_pin() -> str:
    pin = getpass.getpass("请输入 6 位管理 PIN: ")
    if not is_valid_pin(pin):
        raise ValueError("PIN 必须是 6 位数字")
    confirm = getpass.getpass("请再次输入以确认: ")
    if pin != confirm:
        raise ValueError("两次输入不一致")
    return pin


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate ui_admin_pin_hash for face_netd.ini")
    parser.add_argument("--pin", help="6-digit numeric PIN for non-interactive use")
    args = parser.parse_args()

    try:
        pin = args.pin if args.pin is not None else prompt_pin()
        if not is_valid_pin(pin):
            raise ValueError("PIN 必须是 6 位数字")
    except ValueError as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        return 1

    salt = secrets.token_bytes(SALT_BYTES)
    digest = hashlib.pbkdf2_hmac("sha256", pin.encode("utf-8"), salt, ITERATIONS, dklen=32)
    value = f"pbkdf2_sha256${ITERATIONS}${salt.hex()}${digest.hex()}"
    print("ui_admin_pin_hash=" + value)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
