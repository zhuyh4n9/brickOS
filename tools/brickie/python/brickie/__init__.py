"""brickie -- brickOS composition-time tooling (host side).

Layering: this Python package is the L5 frontend (argparse / orchestration / shape validation /
writes / presentation / exit codes); ``brickie-gen`` (C++) renders templates only; ``brickie-core``
(Rust) holds all decisions and error codes (model / solve / version / interface).

No business rules here: closures, ranges, topology, cycles, phases, taxonomy, privileges, budgets,
four-segment version advancement, IFACE-IR and hashes all live natively. Allowed here: argparse,
locating binaries, calling subprocesses, reading/writing files, JSON serialization, and shape
validation against ``schema/*.schema.json``.
"""

__version__ = "0.1.0"

# JSON-over-stdio contract version for L5 <-> L0/L1/L2; bump on incompatible change.
PROTOCOL = 1
