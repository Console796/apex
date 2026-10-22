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
                          "last_relative": None, "max_relative_gap": 0.0,
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
            timestamp = float(event.get("t_ms", 0))
            if active["last_relative"] is not None:
                active["max_relative_gap"] = max(active["max_relative_gap"],
                                                   timestamp - active["last_relative"])
            active["last_relative"] = timestamp
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
              f"relative_sum=({gesture['relative_x']:.2f},{gesture['relative_y']:.2f}) magnitude={rel_mag:.2f} "
              f"max_relative_gap={gesture['max_relative_gap']:.1f}ms")

    lock_events = [event for event in events if event.get("type") == "pointer_lock"]
    lock_counts = defaultdict(int)
    for event in lock_events:
        lock_counts[event.get("event", "?")] += 1
    requests = [event for event in lock_events if event.get("event") == "request"]
    targets = defaultdict(int)
    for event in requests:
        targets[event.get("surface_target", "?")] += 1
    if lock_events:
        print("pointer lock: " + ", ".join(f"{name}={count}" for name, count in sorted(lock_counts.items()))
              + (" targets=" + ",".join(f"{name}:{count}" for name, count in sorted(targets.items()))
                 if targets else ""))

    splitters = []
    current = None
    for event in events:
        if event.get("type") == "splitter":
            if current is None:
                current = {"steps": 0, "max_error": 0, "clamped": 0, "start": event.get("t_ms", 0),
                           "clock_offset": float(event.get("t_ms", 0)) - float(event.get("event_ms", 0)),
                           "input_lag": [], "apply_ms": [], "coalesced": 0}
            current["steps"] += 1
            current["coalesced"] += int(event.get("coalesced", 1))
            if "event_ms" in event:
                lag = (float(event.get("t_ms", 0)) - float(event["event_ms"])) - current["clock_offset"]
                current["input_lag"].append(lag)
            if "apply_ms" in event:
                current["apply_ms"].append(float(event["apply_ms"]))
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
        duration_ms = gesture["end"] - gesture["start"]
        duration_s = max(duration_ms / 1000.0, 0.001)
        lag = sorted(gesture["input_lag"])
        apply = sorted(gesture["apply_ms"])
        lag_stats = (f"median={lag[len(lag) // 2]:.1f}ms max={max(lag):.1f}ms" if lag else "unavailable")
        apply_stats = (f"median={apply[len(apply) // 2]:.2f}ms max={max(apply):.2f}ms"
                       if apply else "unavailable")
        print(f"  {number}: duration={duration_ms:.1f}ms steps={gesture['steps']} "
              f"applies_per_s={gesture['steps'] / duration_s:.1f} coalesced_inputs={gesture['coalesced']} "
              f"input_lag={lag_stats} apply_duration={apply_stats} "
              f"max_target_error={gesture['max_error']}px clamped_moves={gesture['clamped']} reason={gesture['reason']}")

        frames = defaultdict(int)
        recreate_ms = []
        recreate_count = 0
        for event in events:
            timestamp = float(event.get("t_ms", -1))
            if timestamp < gesture["start"] or timestamp > gesture["end"]:
                continue
            if event.get("type") == "frame":
                frames[event.get("pane")] += 1
            elif event.get("type") == "swapchain" and event.get("event") == "recreate":
                recreate_count += int(event.get("recreate_delta", 1))
                if "recreate_ms" in event:
                    recreate_ms.append(float(event["recreate_ms"]))
        fps = ", ".join(f"pane {pane}={count / duration_s:.2f}" for pane, count in sorted(frames.items())) or "none"
        timing = (f" median={sorted(recreate_ms)[len(recreate_ms) // 2]:.2f}ms max={max(recreate_ms):.2f}ms"
                  if recreate_ms else " unavailable")
        print(f"     frame_fps: {fps}; recreates={recreate_count}; recreate_duration:{timing}")

    frame_times = defaultdict(list)
    durations = []
    recreates = defaultdict(int)
    recreate_durations = []
    fd_counts = []
    buffer_mismatches = 0
    max_retired = 0
    forced_retirements = defaultdict(int)
    for event in events:
        if event.get("type") == "frame":
            frame_times[event.get("pane")].append(float(event.get("t_ms", 0)))
            durations.append(float(event.get("duration_ms", 0)))
        elif event.get("type") == "swapchain":
            pane = event.get("pane")
            recreates[pane] += int(event.get("recreate_delta", 1))
            max_retired = max(max_retired, int(event.get("retired", 0)))
            forced_retirements[pane] = max(forced_retirements[pane],
                                            int(event.get("forced_retirements", 0)))
            if "recreate_ms" in event:
                recreate_durations.append(float(event["recreate_ms"]))
        if "fd_count" in event and int(event["fd_count"]) >= 0:
            fd_counts.append(int(event["fd_count"]))
        if event.get("type") in ("frame", "swapchain") and all(
                key in event for key in ("buffer_width", "buffer_height", "window_width", "window_height", "scale")):
            expected_width = round(float(event["window_width"]) * float(event["scale"]))
            expected_height = round(float(event["window_height"]) * float(event["scale"]))
            buffer_mismatches += int(int(event["buffer_width"]) != expected_width or
                                     int(event["buffer_height"]) != expected_height)
    gaps = [(times[i] - times[i - 1], pane) for pane, times in frame_times.items() for i in range(1, len(times))]
    longest_gap, gap_pane = max(gaps, default=(0.0, "?"))
    print(f"frames: count={sum(map(len, frame_times.values()))} longest_gap={longest_gap:.1f}ms "
          f"gap_pane={gap_pane} max_render={max(durations, default=0.0):.1f}ms")
    print("swapchain recreates: " + (", ".join(f"pane {pane}={count}" for pane, count in sorted(recreates.items())) or "none"))
    if forced_retirements or max_retired:
        forced = ", ".join(f"pane {pane}={count}" for pane, count in sorted(forced_retirements.items()))
        print(f"swapchain retirement: max_retired={max_retired} forced_drains={forced or 'none'}")
    if recreate_durations:
        ordered = sorted(recreate_durations)
        print(f"recreate duration: median={ordered[len(ordered) // 2]:.2f}ms max={max(ordered):.2f}ms")
    if fd_counts:
        print(f"open fds: min={min(fd_counts)} max={max(fd_counts)} growth={max(fd_counts) - min(fd_counts)}")
    if any("buffer_width" in event for event in events):
        print(f"presented buffer/window-size mismatches: {buffer_mismatches}")

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
