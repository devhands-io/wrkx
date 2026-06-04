# Architecture Decision Records

This directory holds the wrkx **Architecture Decision Records (ADRs)** — the
durable log of significant architectural decisions, the context that drove them,
and their consequences.

## Conventions

- **One decision per ADR.**
- **Filename:** `NNNN-kebab-case-title.md`, zero-padded to 4 digits, numbered
  globally and sequentially (not per-phase). The phase, if relevant, is a field
  in the document header.
- **Immutable once Accepted.** Do not rewrite an accepted decision. Instead, write
  a new ADR that supersedes it, and set `Superseded by: NNNN` in the old record.
- **Status lifecycle:** `Proposed → Accepted → (Superseded by NNNN | Deprecated)`.
- **Tasks implement ADRs.** A task in `tasks/` cites the ADR it executes via an
  `adr:` header field (and `adr-step:` where the ADR defines an implementation
  sequence). ADRs are never "completed" — tasks are.

## Index

| ADR | Title | Status | Phase |
|-----|-------|--------|-------|
| [0001](0001-three-layer-engine-architecture.md) | Three-Layer Engine Architecture | Accepted  | Phase 1 |
| [0002](0002-layer-configuration-protocol.md)   | Layer Configuration Protocol    | Accepted  | Phase 1 |
