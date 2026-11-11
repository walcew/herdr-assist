#!/usr/bin/env python3
"""`herdr-app` falso para os testes da ponte: registra o argv em FAKE_HC_DIR/app.jsonl."""
import json
import os
import sys

with open(os.path.join(os.environ["FAKE_HC_DIR"], "app.jsonl"), "a") as fh:
    fh.write(json.dumps(sys.argv[1:]) + "\n")
