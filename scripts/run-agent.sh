#!/usr/bin/env bash
# run-agent.sh <name> <promptfile> <logfile> — headless pi subagent runner
set -u
cd /root/peridot-multiboot
name="$1"; prompt="$2"; log="$3"
echo "=== agent $name started $(date) ===" >> "$log"
pi -p --no-session -- "$(cat "$prompt")" >> "$log" 2>&1
echo "=== agent $name exited $(date) code=$? ===" >> "$log"
