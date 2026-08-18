# Historical record

These documents are the working record of the 802.11s mesh port: the scope, the
plans, the audits, and the open questions at the time each was written. They are
kept because the reasoning is worth having, and because several of them contain
measurements that are still the only record of how the chip behaves.

**They are not current documentation, and several are now wrong.** They were
written while mesh receive was still unsolved, and they say so. Mesh works: it
peers, carries data, answers HWMP path requests, and interoperates with
OpenMANET.

For anything you intend to act on, read instead:

- [`../mesh-openmanet.md`](../mesh-openmanet.md) — mesh setup and OpenMANET/OpenWrt interop
- the [wiki](../../wiki/) — task-oriented guides
- [`../../README.md`](../../README.md) — current status

| Document | What it was |
|---|---|
| `mesh-port-scope.md` | Branch scope for the mesh work, with per-phase plans |
| `mesh-driver-port-plan.md` | Plan for porting the Linux `morse_driver` |
| `mesh-port-audit.md` | Audit of the chip command paths the AP code uses |
| `mesh-rx-keystone-status.md` | Status of the mesh RX problem, when it was still open |
| `morse-support-inquiry.md` | A support inquiry about mesh-mode RX on `mm6108.mbin` 1.17.6 |
