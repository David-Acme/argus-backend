#!/usr/bin/env python3

import argparse
import json
import os
import random
import re
import secrets
import socket
import sys

LISTENER_KEYS = {
    "server": {"port", "grpc_port", "health_port", "device_port", "home_port"},
}
ADDRESS_SECTIONS = {"rpc", "internal_tls"}
SECRET_KEYS = {
    ("jwt", "secret"),
    ("jwt", "refresh_secret"),
    ("device", "fingerprint_secret"),
    ("identity", "rpc_secret"),
    ("auth", "rpc_secret"),
    ("sync", "control_secret"),
    ("tunnel", "secret"),
    ("rtc", "api_key"),
    ("rtc", "api_secret"),
}
DATABASE_KEYS = {"db", "db_path"}
PEER = re.compile(
    r"^(?P<scheme>[a-z]+://)?"
    r"(?P<host>127\.0\.0\.1|localhost|0\.0\.0\.0|\[::1\]|argus-[a-z0-9-]+)"
    r":(?P<port>\d+)(?P<rest>/.*)?$")
LINE = re.compile(r"^(?P<indent>\s*)(?P<key>[A-Za-z0-9_]+)(?P<eq>\s*=\s*)(?P<value>.*?)\s*$")
SECTION = re.compile(r"^\s*\[(?P<name>[^\]]+)\]\s*$")
PORT_LOW = 20000
PORT_HIGH = 60999


class Ports:
    def __init__(self, reserved):
        self.taken = set(reserved)
        self.taken.add(4222)

    def fresh(self):
        for _ in range(10000):
            port = random.randint(PORT_LOW, PORT_HIGH)
            if port in self.taken:
                continue
            probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            try:
                probe.bind(("127.0.0.1", port))
            except OSError:
                continue
            finally:
                probe.close()
            self.taken.add(port)
            return port
        raise SystemExit("boot-proof: no free loopback port was found")


def unquote(value):
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        return value[1:-1]
    return value


def quote(value):
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def resolve_path(value, root, service):
    if not value or os.path.isabs(value):
        return value
    root = os.path.normpath(root)
    for base in (os.path.join(root, "services", service), root):
        candidate = os.path.normpath(os.path.join(base, value))
        if os.path.commonpath([root, candidate]) == root and os.path.exists(candidate):
            return candidate
    return value


def render(template, options, ports):
    service = options.service
    closed = ports.fresh()
    summary = {
        "service": service,
        "closed": closed,
        "listeners": [],
        "health": None,
        "rpc": None,
        "tls": None,
        "secrets": [],
    }
    service_plain = False
    service_port = None
    server_port = None
    health_port = None
    lines = []
    section = ""
    for raw in template.splitlines():
        header = SECTION.match(raw)
        if header:
            section = header.group("name").strip()
            lines.append(raw)
            continue
        line = LINE.match(raw)
        if not line:
            lines.append(raw)
            continue
        key = line.group("key")
        value = unquote(line.group("value"))
        rendered = None
        if section in LISTENER_KEYS and key in LISTENER_KEYS[section] or \
                (section == service and key == "port"):
            port = ports.fresh()
            summary["listeners"].append(port)
            rendered = str(port)
            if section == service:
                service_port = port
            elif key == "health_port":
                health_port = port
            elif key == "port":
                server_port = port
            elif key == "grpc_port":
                summary["rpc"] = port
        elif section in ADDRESS_SECTIONS and key == "address":
            port = ports.fresh()
            summary["listeners"].append(port)
            rendered = quote(f"127.0.0.1:{port}")
            summary["tls" if section == "internal_tls" else "rpc"] = port
        elif section == "remote" and key == "tunnel_port":
            rendered = "0"
        elif section == "remote" and key == "enabled":
            rendered = "false"
        elif key == "host" and section in ("server", service):
            rendered = quote("127.0.0.1")
        elif section == "internal_tls" and key == "dir":
            rendered = quote(options.pki)
        elif section == "cert" and key == "server_cert":
            rendered = quote(options.cert)
        elif section == "cert" and key == "server_key":
            rendered = quote(options.key)
        elif section == "nats" and key == "url":
            rendered = quote("")
        elif section == "mdns" and key == "enabled":
            rendered = "false"
        elif section == "settings" and key == "first_run":
            rendered = "false"
        elif section.startswith("owners.") and key == "target":
            rendered = quote("")
        elif (section, key) in SECRET_KEYS or section == "rpc.callers" or \
                (section == "grpc" and key.startswith("caller_")):
            rendered = quote(secrets.token_hex(48))
            summary["secrets"].append(f"{section}.{key}")
        elif key in DATABASE_KEYS or (section == "database" and key in ("file", "path", "db")):
            name = os.path.basename(value) if value else f"{service}.db"
            rendered = quote(os.path.join(options.scratch, "data", name))
        elif key == "schema":
            rendered = quote(resolve_path(value, options.root, service))
        elif key.endswith("_dir") or key.endswith("_path") or value.startswith("models/"):
            rendered = quote(resolve_path(value, options.root, service))
        else:
            peer = PEER.match(value) if line.group("value").startswith('"') else None
            if peer:
                rendered = quote((peer.group("scheme") or "") + f"127.0.0.1:{closed}" +
                                 (peer.group("rest") or ""))
        if section == service and key == "plain":
            service_plain = value == "true"
        if rendered is None:
            lines.append(raw)
        else:
            lines.append(line.group("indent") + key + line.group("eq") + rendered)
    if service_port is not None:
        summary["health"] = {"scheme": "http" if service_plain else "https",
                             "port": service_port}
    elif health_port is not None:
        summary["health"] = {"scheme": "http", "port": health_port}
    elif server_port is not None:
        summary["health"] = {"scheme": "http", "port": server_port}
    return "\n".join(lines) + "\n", summary


def masked(config, summary):
    hidden = set(summary["secrets"])
    out = []
    section = ""
    for raw in config.splitlines():
        header = SECTION.match(raw)
        if header:
            section = header.group("name").strip()
            out.append(raw)
            continue
        line = LINE.match(raw)
        if line and f"{section}.{line.group('key')}" in hidden:
            out.append(line.group("indent") + line.group("key") + line.group("eq") + '"<masked>"')
        else:
            out.append(raw)
    return "\n".join(out) + "\n"


def write_config(options):
    reserved = set()
    with open(options.reserved, encoding="utf-8") as listed:
        for entry in listed:
            entry = entry.strip()
            if entry.isdigit():
                reserved.add(int(entry))
    with open(options.template, encoding="utf-8") as source:
        template = source.read()
    config, summary = render(template, options, Ports(reserved))
    fd = os.open(options.out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as out:
        out.write(config)
    with open(options.summary, "w", encoding="utf-8") as out:
        json.dump(summary, out, indent=1, sort_keys=True)
    if options.masked:
        with open(options.masked, "w", encoding="utf-8") as out:
            out.write(masked(config, summary))
    return 0


def print_field(options):
    with open(options.summary, encoding="utf-8") as source:
        value = json.load(source)
    for part in options.name.split("."):
        value = value.get(part) if isinstance(value, dict) else None
    if isinstance(value, list):
        print(" ".join(str(item) for item in value))
    elif value is not None:
        print(value)
    return 0


def internal_tls_loaded(text):
    try:
        body = json.loads(text)
    except ValueError:
        return "unparsed"
    if not isinstance(body, dict):
        return "unparsed"
    section = body["info"] if isinstance(body.get("info"), dict) else body
    tls = section.get("internalTls")
    if not isinstance(tls, dict):
        return "absent"
    return "true" if tls.get("loaded") is True else "false"


def print_health(_options):
    print(internal_tls_loaded(sys.stdin.read()))
    return 0


GRPCURL_CODE = re.compile(r"^\s*Code: (\w+)\s*$", re.M)
METADATA_HEADERS = ("Response headers received:", "Response trailers received:")
METADATA_LINE = re.compile(r"^[a-z0-9-]+: ")
HANDSHAKE = re.compile(r"tls|handshake|certificate|x509", re.I)


def upper_snake(name):
    return re.sub(r"(?<!^)(?=[A-Z])", "_", name).upper()


def received_metadata(text):
    found = []
    collecting = False
    for line in text.splitlines():
        if line.strip() in METADATA_HEADERS:
            collecting = True
            continue
        if not collecting:
            continue
        if line.strip() == "(empty)":
            continue
        if METADATA_LINE.match(line):
            found.append(line)
            continue
        collecting = False
    return found


def error_lines(text):
    return "\n".join(line for line in text.splitlines()
                     if "Failed to dial target host" in line or
                     line.strip().startswith("Message:"))


def classify(text):
    dial_failed = "Failed to dial target host" in text
    metadata = received_metadata(text)
    code = GRPCURL_CODE.search(text)
    if code:
        status = upper_snake(code.group(1))
    elif dial_failed:
        status = "UNAVAILABLE"
    elif "Response contents:" in text:
        status = "OK"
    else:
        status = "UNKNOWN"
    origin = "server" if metadata and not dial_failed else "transport"
    return {
        "status": status,
        "origin": origin,
        "serving": '"status": "SERVING"' in text,
        "handshake": origin == "transport" and bool(HANDSHAKE.search(error_lines(text))),
        "dialFailed": dial_failed,
    }


def print_classification(_options):
    result = classify(sys.stdin.read())
    print(f"{result['status']} {result['origin']} "
          f"{'serving' if result['serving'] else 'not-serving'} "
          f"{'handshake' if result['handshake'] else 'no-handshake-error'}")
    return 0


def main():
    parser = argparse.ArgumentParser(description="The boot proof's helpers.")
    commands = parser.add_subparsers(dest="command", required=True)

    config = commands.add_parser("config",
                                 help="write config.toml from a config template")
    for name in ("service", "template", "root", "scratch", "pki", "cert", "key",
                 "reserved", "out", "summary"):
        config.add_argument(f"--{name}", required=True)
    config.add_argument("--masked", default=None)
    config.set_defaults(run=write_config)

    field = commands.add_parser("field", help="print one field of the summary")
    field.add_argument("--summary", required=True)
    field.add_argument("name")
    field.set_defaults(run=print_field)

    health = commands.add_parser(
        "health", help="read a /health body on stdin, print internalTls.loaded")
    health.set_defaults(run=print_health)

    classified = commands.add_parser(
        "classify",
        help="read grpcurl -v output on stdin, print status, origin, SERVING and "
             "whether a TLS handshake error was reported")
    classified.set_defaults(run=print_classification)

    options = parser.parse_args()
    return options.run(options)


if __name__ == "__main__":
    sys.exit(main())
