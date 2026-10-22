#!/usr/bin/env python3
"""Summarize an Apex APEX_INPUT_LOG JSONL human-interface trace."""

import argparse
import json
import math
import sys
from collections import defaultdict
from pathlib import Path


def xy(value):
    return (float(value.get("x", 0)), float(value.get("y", 0))) if isinstance(value, dict) else (0.0, 0.0)


def distance(a, b):
    return math.hypot(b[0] - a[0], b[1] - a[1])


def load(path):
    events = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError as error:
            print(f"{path}:{number}: invalid JSON: {error}", file=sys.stderr)
            continue
        if isinstance(event, dict):
            events.append(event)
    return events


def summarize(events):
    start = next((event for event in events if event.get("type") == "start"), {})
    print(f"platform={start.get('platform', '?')} qt={start.get('qt', '?')} scale={start.get('scale', '?')}")

    gestures = []
    active = None
    for event in events:
        kind = event.get("type")
        if kind == "mouse":
            position = xy(event.get("global"))
            if event.get("event") == "press" and active is None:
                active = {"start": event.get("t_ms", 0), "last": position, "mouse": 0.0,
                          "relative": 0, "relative_x": 0.0, "relative_y": 0.0,
                          "button": event.get("button", 0)}
            elif active is not None:
                active["mouse"] += distance(active["last"], position)
                active["last"] = position
                if event.get("event") == "release" and not event.get("buttons", 0):
                    active["end"] = event.get("t_ms", active["start"])
                    gestures.append(active)
                    active = None
        elif kind == "relative" and active is not None:
            dx, dy = xy(event.get("delta"))
            active["relative"] += 1
            active["relative_x"] += dx
            active["relative_y"] += dy
    if active is not None:
        active["end"] = events[-1].get("t_ms", active["start"]) if events else active["start"]
        gestures.append(active)

    print("pointer gestures:")
    if not gestures:
        print("  (none)")
    for number, gesture in enumerate(gestures, 1):
        rel_mag = math.hypot(gesture["relative_x"], gesture["relative_y"])
        print(f"  {number}: button={gesture['button']} duration={gesture['end'] - gesture['start']:.1f}ms "
              f"mouse_path={gesture['mouse']:.1f}px relative_events={gesture['relative']} "
              f"relative_sum=({gesture['relative_x']:.2f},{gesture['relative_y']:.2f}) magnitude={rel_mag:.2f}")

    splitters = []
    current = None
    for event in events:
        if event.get("type") == "splitter":
            if current is None:
                current = {"steps": 0, "max_error": 0, "clamped": 0, "start": event.get("t_ms", 0)}
            current["steps"] += 1
            for move in event.get("moves", []):
                requested, result = move.get("requested", 0), move.get("result", 0)
                bounds = move.get("range", [requested, requested])
                legal = min(max(requested, bounds[0]), bounds[1]) if len(bounds) == 2 else requested
                current["max_error"] = max(current["max_error"], abs(result - legal))
                current["clamped"] += int(requested != legal)
        elif event.get("type") == "splitter_end" and current is not None:
            current["end"] = event.get("t_ms", current["start"])
            current["reason"] = event.get("reason", "?")
            splitters.append(current)
            current = None
    print("splitter gestures:")
    if not splitters:
        print("  (none)")
    for number, gesture in enumerate(splitters, 1):
        print(f"  {number}: duration={gesture['end'] - gesture['start']:.1f}ms steps={gesture['steps']} "
              f"max_target_error={gesture['max_error']}px clamped_moves={gesture['clamped']} reason={gesture['reason']}")

    frame_times = defaultdict(list)
    durations = []
    recreates = defaultdict(int)
    for event in events:
        if event.get("type") == "frame":
            frame_times[event.get("pane")].append(float(event.get("t_ms", 0)))
            durations.append(float(event.get("duration_ms", 0)))
        elif event.get("type") == "swapchain":
            pane = event.get("pane")
            recreates[pane] += int(event.get("recreate_delta", 1))
    gaps = [(times[i] - times[i - 1], pane) for pane, times in frame_times.items() for i in range(1, len(times))]
    longest_gap, gap_pane = max(gaps, default=(0.0, "?"))
    print(f"frames: count={sum(map(len, frame_times.values()))} longest_gap={longest_gap:.1f}ms "
          f"gap_pane={gap_pane} max_render={max(durations, default=0.0):.1f}ms")
    print("swapchain recreates: " + (", ".join(f"pane {pane}={count}" for pane, count in sorted(recreates.items())) or "none"))

    markers = [event for event in events if event.get("type") == "marker"]
    if markers:
        print("markers: " + ", ".join(f"#{event.get('n')}@{event.get('t_ms', 0):.1f}ms" for event in markers))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    summarize(load(args.trace))


if __name__ == "__main__":
    main()
