#!/usr/bin/env bash
# Regression guard (2026-10-30 WD crash): async WebEngine callbacks (runJavaScript,
# fetchHtml, requestWordCount, requestHeadingFont) must not capture by reference [&];
# they can fire after the waiting stack frame has returned. Use shared state by value.
cd "$(dirname "$0")/.."
bad=$(grep -nE '(runJavaScript|fetchHtml|requestWordCount|requestHeadingFont)\(' -A1 *.cpp | grep -E '\[&\]|\[&,' || true)
if [ -n "$bad" ]; then echo "FAIL: by-reference capture in async callback:"; echo "$bad"; exit 1; fi
echo "ok: no by-reference captures in async WebEngine callbacks"
