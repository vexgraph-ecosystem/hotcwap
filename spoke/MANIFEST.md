# MANIFEST.mf — load-policy reference file (not Java)

Despite the `.mf` extension, this file has nothing to do with Java: no JVM,
no JAR tooling, no `jar` command will ever read it. The extension is a naming
nod only — to the idea of a manifest as *the paper that says what may board*.
What reads this file is `HotManifest_parse` (`hot/manifest.h/.c`), plain JSON.

## What it is

The provider-owned allow-list for the vexspoke runtime substrate. Each
`consumers[]` row names one project that may bind vexspoke, the runtime
supervision level it executes at, and the API sections it is granted:

```json
{"name": "hotcwap", "runtime": "R1", "sections": ["arena", "rings"]}
```

- `name` — the consumer module. The listing itself is the authority:
  unlisted consumers are refused (`HotManifest_allows`, default-deny).
- `runtime` — R1–R5 runtime supervision level (R1 boots first, R5 last).
  Documentary: it records *where* the consumer executes, not seniority,
  and grants nothing by itself. This field is deliberately not called
  `rank` — nothing outranks anything here; levels supervise downward.
- `sections` — granted API surface at section granularity (`arena`,
  `rings`, …). Never per-function: per-function rows rot into
  unmaintained spreadsheets.

## Admitting a project

Append one row. No code, no swap, no quiesce — policy moves at policy
speed. If the newcomer needs API vexspoke does not yet expose, that is a
separate code change (table growth in `spoke/vexspoke.h`, versioned,
ABI-gated); the two travel together but commit separately.

## What it is not

- Not a package manager: no owners, no signatures, no dependency edges.
  Integrity of staged payloads is `hot/stage.h`'s job (`files.sha`).
- Not ABI: `HotManifest_compatible` and `HotManifest_digest` both ignore
  `consumers[]`. Policy churn never invalidates a validated contract.
- Not upward-grantable: only the provider side is honored. An R3 manifest
  claiming vexspoke access is meaningless and refused.

## Map

- Schema + gate: `hot/manifest.h/.c` (`HotConsumer`, `HotManifest_allows`)
- Contract table: `spoke/vexspoke.h` (`VexspokeApi`, section defines)
- Live seed: `MANIFEST.mf` (this directory — the loader reads this file)
- Harness: `_tests/hotcwap/manifest_test.c` (§8–§11; §11 mirrors this file)
