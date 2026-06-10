# Hybrid post-quantum signatures

Implementation notes for the modified code paths.

## Overview

Adds hybrid signing to every place xrpld signs something, so that
breaking ECC alone, or breaking ML-DSA-44 alone, does not break the
network. Every signed payload (transaction, manifest, validation,
consensus proposal) can carry both an ECC signature and a Dilithium
signature. The protocol verifies both when present and binds the
per-payload PQ pubkey to a published reference (account state for
transactions, validator manifest for validations) to close downgrade
attacks.

All new behaviour is gated by the `Quantum` amendment
(`include/xrpl/protocol/detail/features.macro:18`). Pre-amendment
behaviour of every existing transactor, manifest verifier, and
validation handler is unchanged.

## Architecture

Two layers, applied uniformly across the four signing surfaces.

### Cryptographic layer (libxrpl, ledger-free)

Each signed payload type gets a sibling pair of fields:

- ECC: `sfSigningPubKey` + `sfSignature` (existing) plus, on
  manifests, `sfPublicKey` + `sfMasterSignature` for the master pair.
- PQ: `sfQuantumPubKey` + `sfQuantumSignature` (new) plus, on
  manifests, `sfQuantumMasterPublicKey` + `sfQuantumMasterSignature`
  for the master pair.

The ECC signature covers the PQ pubkey fields (they are signing
fields, not NotSigning), and vice versa. Stripping any PQ field from
a payload invalidates the ECC signature too. Downgrade resistance is
baked into the field-set partitioning.

The PQ primitive is `xrpl::pqSign(Slice, Slice)` /
`xrpl::pqVerify(Slice, Slice, Slice)` in
`include/xrpl/protocol/PQSign.h`, wrapping the
`pq-code-package/mldsa-native` library vendored under
`external/mldsa-native/`. An `STObject`-aware overload
`pqSign(STObject&, HashPrefix, Slice, SF_VL)` mirrors the existing ECC
`xrpl::sign(STObject&, ...)` so manifest and validation code paths
look symmetric.

### Authentication / binding layer (xrpld, ledger-aware)

The cryptographic layer accepts any PQ signature over any PQ pubkey
present in the payload. The authentication layer rejects the payload
if that PQ pubkey is not the expected one for the signer.

| Payload                   | Reference                                                                                                                   | Code path                          |
| ------------------------- | --------------------------------------------------------------------------------------------------------------------------- | ---------------------------------- |
| Transaction (single-sign) | `AccountRoot.sfQuantumPubKey` of the source                                                                                 | `Transactor::checkSingleSign`      |
| Transaction (multi-sign)  | `AccountRoot.sfQuantumPubKey` of each signer, or `SignerEntry.sfQuantumPubKey` on the source's SignerList (phantom signers) | `Transactor::checkMultiSign`       |
| Manifest (ephemeral)      | `sfQuantumMasterPublicKey` published on the same manifest                                                                   | `Manifest::verify`                 |
| Validation                | `manifest.quantumSigningKey` for the validator's master                                                                     | `PeerImp::onMessage(TMValidation)` |
| Consensus proposal        | `manifest.quantumSigningKey` for the proposer's master                                                                      | `PeerImp::onMessage(TMProposeSet)` |

Mismatch is a hard reject regardless of any operator configuration.
A missing PQ on a hybrid-declared validator is governed by the
`[pq_validations] = fail_open | fail_closed` runtime flag.

## Scope of changes

- **Foundations.** Vendor `pq-code-package/mldsa-native` v1.0.0-beta2
  (formally verified ML-DSA-44, ISC license) as a git submodule
  under `external/mldsa-native/`. `KeyType::Dilithium` extends the
  enum that already covers Secp256k1 / Ed25519. New SFields:
  `sfQuantumPubKey` (32, VL), `sfQuantumSignature` (33, VL
  NotSigning), `sfQuantumMasterPublicKey` (34, VL),
  `sfQuantumMasterSignature` (35, VL NotSigning). `sfQuantumPubKey`
  is declared as a universal optional signing field on every
  transaction template and as an optional field on `AccountRoot`.
  `featureQuantum` amendment gates everything.

- **Transaction-side hybrid signing.** `STTx::sign` overload accepts
  an optional PQ pair; `STTx::checkSign` verifies the PQ signature
  when present. The multi-sign path binds the per-signer PQ pubkey
  into the signing payload via
  `buildMultiSigningData(STObject, AccountID, Slice pqPub)` so a PQ
  signature cannot be reused across signers. `sfSigner` inner-object
  template gains optional PQ fields.

- **Per-account authentication.** `AccountSet asfQuantum` flag
  (value 18) opts an account in to hybrid; `sfQuantumPubKey` on
  `AccountRoot` is the per-account reference.
  `SignerListSet` persists a per-`SignerEntry` PQ pubkey so phantom
  signers can also be authenticated. `Transactor::checkSingleSign`
  and `Transactor::checkMultiSign` enforce the per-tx
  PQ-pubkey-equals-on-ledger check. Anti-lockout invariants on both
  `AccountSet::preclaim` and `SignerListSet::preclaim` block any
  change that would leave the account unable to authorise a future
  hybrid tx.

- **Validator-side hybrid.** Validator manifests sign with all four
  keys (ECC master + ECC ephemeral + PQ master + PQ ephemeral).
  `STValidation` carries an optional PQ pair; `TMProposeSet`
  protobuf gains optional `pqPubKey` and `pqSignature` fields;
  `RCLCxPeerPos` carries the PQ pair through consensus.
  `PeerImp::onMessage(TMValidation)` applies the always-on binding
  check and a configurable fail-closed gate.
  `[pq_validations]` config section (`fail_open` default,
  `fail_closed` opt-in) controls the latter.

- **RPC surface.** `wallet_propose key_type=dilithium` derives an
  ML-DSA-44 keypair deterministically from the same `xrpl::Seed`
  input the ECC branch uses. `sign` and `sign_for` accept
  `pq_seed_hex` and emit hybrid `tx_blob` / hybrid `Signer`. The
  `manifest` RPC handler surfaces `pq_master_key` and
  `pq_ephemeral_key` for any validator in the manifest cache.

## Key design decisions

**Field reuse mirrors ECC, exactly.** `sfQuantumPubKey` plays the
same role across three contexts (per-tx signing field, manifest
ephemeral pubkey, validation signing pubkey) that ECC's
`sfSigningPubKey` plays. Only the master-side pair
(`sfQuantumMasterPublicKey`, `sfQuantumMasterSignature`) is new.
This keeps the validation template symmetric with the tx-side
per-signer template, and makes the binding rules trivially parallel.

**`asfQuantum` flag, not empty-value clear.** `sfQuantumPubKey`
doubles as a signing field on every tx and as the registration
payload on `AccountSet`. The empty-VL clearing convention used by
`sfMessageKey` cannot work here because an opted-in account's tx
must carry a non-empty `sfQuantumPubKey` for signing, so the empty
clear-signal cannot reach `doApply`. The flag-operation pattern
(`asfQuantum` `SetFlag` / `ClearFlag`), modelled after
`asfDisableMaster`, sidesteps that.

**Validator manifests sign with all four keys.** The manifest carries
ECC master, ECC ephemeral, PQ master, PQ ephemeral. Both PQ pubkeys
are signing fields (not NotSigning), so the ECC signatures commit to
them; an attacker cannot strip the PQ half from an existing manifest
without also breaking the ECC half. The complementary attack, an
attacker with the recovered ECC master secret minting a _fresh_
higher-sequence ECC-only manifest, is blocked separately by the
PQ-master continuity rule in `ManifestCache::applyManifest`: once a
master key has published a hybrid manifest, every later
non-revocation manifest must keep the same PQ master key.

**Fail-open is the default, and it covers substitution but not
stripping.** The binding gates reject a validation or proposal whose
PQ pubkey is _present but mismatched_ against the manifest's published
value, regardless of configuration. They do not, under `fail_open`,
reject a message from a hybrid-declared validator that omits the PQ
fields entirely: an attacker holding the validator's ECC ephemeral
secret can still mint accepted ECC-only validations/proposals. Closing
that stripping window is exactly what `fail_closed` does, so
fully-hybrid networks should run with it. `fail_open` is the
deliberate mixed-network rollout default, not a protocol-level
guarantee that the PQ half cannot be dropped.

**Determinism for tooling.** `wallet_propose key_type=dilithium`
derives the PQ keypair deterministically from the same passphrase /
seed input ECC uses. The trade-off is that the same `xrpl::Seed`
reveals both ECC and PQ keys via different KDFs, which is acceptable
because the operator already holds the seed.

## Build and verify

```sh
git submodule update --init --recursive
cmake --preset=default
cmake --build .build --target xrpld -j$(nproc)
```

Unit-test sweep:

```sh
./.build/xrpld --unittest
```

End-to-end local demo (xrpld standalone, Quantum pre-enabled at
genesis, walks `wallet_propose` -> fund -> opt-in -> hybrid Payment
-> ECC-only rejection in a few seconds):

```sh
python3 scripts/demo/demo.py
```

See `scripts/demo/README.md` for details on what the demo prints.

## Out of scope for this PoC

- Batch hybrid (`sfBatchSigner` inner template).
- Counterparty hybrid (`sfCounterpartySignature` inner template,
  `LoanSet`).
- Atomic PQ key rotation. Two-step clear-then-register is the
  workaround. Atomic rotation would need a separate
  `sfNewQuantumPubKey` payload field, since `sfQuantumPubKey` is
  consumed as the per-tx signing field.
- Distinct `ter` codes for PQ binding failures. Today every
  binding-related failure surfaces as `tefBAD_AUTH`.
