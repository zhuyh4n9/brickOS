"""brickie — brickOS composition-time tooling (host side).

Layering (see ``Design/docs/2-toolchain/brickie/brickie-v0.1.md`` §9.1 and
``tools/brickie/docs/contract.md``):

    L5 frontend  Python3  this package: argparse / orchestration / schema shape validation / writes / presentation / exit codes
    L2 generator C++     ``brickie-gen``: template rendering only (skeletons / descriptors / headers)
    L0/L1        Rust    ``brickie-core``: all decisions and error codes (model / solve / version / interface)

**Rules for this package** (contract §8 / V-18c): no business rules. Closures, ranges, topology, cycles, phases,
taxonomy, privileges, budgets, four-segment version advancement, IFACE-IR and hashes all live on the native side; this package only does
"the six allowed things" — argparse, locating binaries, calling subprocesses, reading/writing files, JSON serialization, and
**single-record shape** validation against ``schema/*.schema.json``.

Module map:

    cli.py       29 leaf commands (23 composition + 6 build family) + 2 global switches: argparse and orchestration pipeline
    native.py    locate and call brickie-core / brickie-gen (JSON over stdio)
    runner.py    **executor** of the build family (concurrency/timeout/logging/skipping short-circuited downstream; no decisions)
    schema.py    bundled minimal JSON-Schema subset (BRV-MF-0001 / exit code 2)
    writer.py    idempotent writes / --check byte-for-byte compare / declaration-surface scan
    present.py   render data as human-readable text (--json and text share one source)
    hostinfo.py  host triple (coordinates for build/host/<arch>/<os>, same rules as tools/host-detect.sh)
"""

__version__ = "0.1.0"

# JSON over stdio contract version for L5 ↔ L0/L1/L2 (contract §2). Incompatible changes must bump it.
PROTOCOL = 1
