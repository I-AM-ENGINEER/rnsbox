"""Tiny KEY=VALUE config reader shared by the client and the server.

The file is deliberately boring: one ``key=value`` per line, ``#``/``;``
comments, surrounding quotes stripped.  Both the rnsbox init script and the
portal write this format.
"""
import os


def load(path):
    values = {}
    if not path:
        return values
    try:
        with open(path, "r", encoding="utf-8") as handle:
            for raw in handle:
                line = raw.strip()
                if not line or line[0] in "#;":
                    continue
                if "=" not in line:
                    continue
                key, value = line.split("=", 1)
                key = key.strip().lower()
                value = value.strip()
                if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                    value = value[1:-1]
                if key:
                    values[key] = value
    except OSError:
        pass
    return values


def csv(value):
    """Split a comma/semicolon/whitespace separated list."""
    if not value:
        return []
    for sep in (";", "\n", "\t", " "):
        value = value.replace(sep, ",")
    return [item.strip() for item in value.split(",") if item.strip()]


def env(name, default=None):
    value = os.environ.get(name)
    return default if value is None or value == "" else value
