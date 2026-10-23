Apex-authored test fixtures (MIT). Text fixtures (`.vmf`, `.vmt`, `.kv`, `.txt`) must carry an
`Apex-authored` marker in their first lines. Never add Valve or Strata content here.

`topology/` holds solids that today's same-topology write path rejects (0.3.1 P0, plan 0.3 D2). They
are kept out of `vmf/`, whose fixtures every identity sweep expects to round-trip.
