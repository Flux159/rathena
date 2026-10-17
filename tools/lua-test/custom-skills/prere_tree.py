#!/usr/bin/env python3
"""Pre-renewal has no Guillotine Cross: put the example's tree on the
Assassin Cross, with the stock GX skills it builds on."""
import sys
src = open(sys.argv[1]).read()
body = src[src.index("Body:"):]
body = body.replace("  - Job: Guillotine_Cross\n    Tree:\n", """  - Job: Assassin_Cross
    Tree:
      - Name: GC_CROSSIMPACT
        MaxLevel: 5
        Requires:
          - Name: AS_SONICBLOW
            Level: 10
      - Name: GC_RESEARCHNEWPOISON
        MaxLevel: 10
      - Name: GC_CLOAKINGEXCEED
        MaxLevel: 5
        Requires:
          - Name: AS_CLOAKING
            Level: 3
""")
assert "Assassin_Cross" in body
open(sys.argv[2], "w").write("Header:\n  Type: SKILL_TREE_DB\n  Version: 1\n\n" + body)
