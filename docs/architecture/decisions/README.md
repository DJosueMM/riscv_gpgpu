# Architecture Decision Records

ADRs capture decisions that affect more than one component. They record why a
choice was made, its current status, and the evidence required to change it.

| ADR | Decision | Status |
|---|---|---|
| [0001](0001-platform-roles.md) | Platform roles and portable accelerator core | Accepted |
| [0002](0002-baseline-isa-abi.md) | Baseline ISA and kernel ABI | Accepted |
| [0003](0003-csr-memory-contract.md) | Canonical CSR and memory contract | Proposed |
| [0004](0004-kria-memory-transport.md) | Kria memory transport | Proposed |
| [0005](0005-alveo-shell-selection.md) | Alveo U55C shell selection | Proposed |
| [0006](0006-control-data-plane-separation.md) | Control, data, and trace plane separation | Accepted |

An accepted ADR can be superseded only by another ADR. A proposed ADR must not
be cited as implemented behavior.