#!/usr/bin/env python3
"""Compare SimpleNote against SimpleWords after every editing operation."""
import os
import random
import subprocess
import sys
import tempfile


def hex_text(text):
    return text.encode().hex()


steps = []
documents = [
    "", "Bottom line", "Above\nBottom line", "Above\n", "\n\n\n",
    "a" * 80 + "bc", "word " * 32,
    "large          X " * 10 + "\n\tindented\nEnd",
    "    Café 世界 e\u0301 👩\u200d💻\n\n\tEnd.\n",
    "\n".join(f"Scroll row {i:03}: alpha beta" for i in range(60)),
]
randomizer = random.Random(32011)
for rows, columns in ((32, 140), (12, 40), (12, 79), (12, 80), (12, 81), (8, 12)):
    for document in documents:
        steps.append(f"F {rows} {columns} {hex_text(document)}")
        # End must stay on its visual row; Left/Right may change wrap affinity
        # without consuming a character. Shift-Up and Shift-Down reach edges.
        steps += ["K 6 0", "K 2 0", "K 1 0", "K 5 0", "K 3 1", "K 4 1",
                  "K 8 1", "C", "X", "U", "R", "U", "K 7 0"]
        # Empty lines, wide characters, reverse drags, word/paragraph clicks,
        # dragging outside the body, and wheel navigation use source bytes.
        left = max(0, (columns - 80) // 2)
        steps += [f"M p 3 {left}", f"M d {rows - 2} {columns - 1}",
                  f"M r {rows - 2} {columns - 1}", "C", "X", "U", "Y", "U",
                  f"M p 3 {left + 1}", f"M r 3 {left + 1}",
                  f"M p 3 {left + 1}", f"M r 3 {left + 1}",
                  f"M p 3 {left + 1}", f"M r 3 {left + 1}",
                  f"M p {rows - 2} {columns - 1}", f"M d 0 0", "M r 0 0",
                  "M u 4 1", "M w 4 1", "H 0", "H 1", "M p 5 5", "M r 5 5",
                  "K 1 0"]
        for _ in range(120):
            action = randomizer.randrange(10)
            if action < 5:
                movement = randomizer.randrange(1, 9)
                extend = randomizer.randrange(2) if movement not in (5, 6) else 0
                steps.append(f"K {movement} {extend}")
            elif action == 5:
                steps.append("T " + hex_text(randomizer.choice(("x", " ", "test", "é", "世界", "\n", "\t"))))
            elif action == 6:
                steps.append("P " + hex_text("pasted\r\nUnicode: café 世界"))
            else:
                steps.append(randomizer.choice(("B", "D", "U", "R", "C", "X", "Y")))
        steps += [f"Z {rows + 2} {columns + 5}", "K 8 1", "K 7 1", "C"]

with tempfile.TemporaryDirectory(prefix="text-surface-parity-") as home:
    environment = dict(os.environ, HOME=home, LC_ALL="C.UTF-8")
    payload = "\n".join(steps) + "\n"
    results = []
    for binary in sys.argv[1:]:
        result = subprocess.run([binary], input=payload, text=True, capture_output=True, env=environment)
        if result.returncode:
            print(f"{binary} failed ({result.returncode}):\n{result.stderr}", file=sys.stderr)
            raise SystemExit(1)
        results.append(result.stdout.splitlines())
    assert len(results) >= 2
    for result in results:
        assert len(result) == len(steps), (len(result), len(steps))
    for index, command in enumerate(steps):
        states = [result[index] for result in results]
        if len(set(states)) != 1:
            print(f"Surface differs at step {index}: {command}", file=sys.stderr)
            print("Recent actions: " + repr(steps[max(0, index - 12):index + 1]), file=sys.stderr)
            for binary, state in zip(sys.argv[1:], states):
                print(f"{binary}: {state[:1500]}", file=sys.stderr)
            raise SystemExit(1)
print(f"OK identical text surfaces: {len(steps)} steps across six terminal sizes; "
      "cursor, selection, highlighted cells, scroll, mouse, Unicode, clipboard, undo and redo")
