import re
import glob
import os

def parse_logs():
    log_files = [
        "debug_log_part1.txt",
        "debug_log_part2a.txt",
        "debug_log_part2b.txt",
        "debug_log_part3a.txt",
        "debug_log_part3b.txt"
    ]
    
    events = []
    
    # regex patterns
    # Time [Proc] Message
    # We stripped Proc in some? No, kept [Proc].
    # Line: "37.257855\t[TileManager] OnTileReady: Key=... (LOD=1 X=8 Y=6)..."
    
    re_decode = re.compile(r"Worker (\d+): Decode Tile \(LOD (\d+), C(\d+) R(\d+)\)")
    re_ready = re.compile(r"OnTileReady:.*\(LOD=(\d+) X=(\d+) Y=(\d+)\)")
    re_done = re.compile(r"Worker (\d+): DONE Tile")
    re_main_ready = re.compile(r"\[Main\] TileReady: LOD=(\d+) \((\d+),(\d+)\)")
    
    for fname in log_files:
        if not os.path.exists(fname):
            print(f"Checking {fname}... Missing (skipping)")
            continue
            
        with open(fname, 'r', encoding='utf-8') as f:
            for line in f:
                parts = line.strip().split('\t')
                if len(parts) < 2: continue
                
                time_str = parts[0]
                try:
                    ts = float(time_str)
                except:
                    continue
                    
                msg = parts[-1]
                
                # Decode Dispatch
                m = re_decode.search(msg)
                if m:
                    evt = {
                        'type': 'DISPATCH',
                        'ts': ts,
                        'worker': int(m.group(1)),
                        'lod': int(m.group(2)),
                        'x': int(m.group(3)),
                        'y': int(m.group(4))
                    }
                    events.append(evt)
                    continue

                # Tile Ready (Worker callback)
                m = re_ready.search(msg)
                if m:
                    evt = {
                        'type': 'READY_TM',
                        'ts': ts,
                        'lod': int(m.group(1)),
                        'x': int(m.group(2)),
                        'y': int(m.group(3))
                    }
                    events.append(evt)
                    continue

                # Main Thread Ready
                m = re_main_ready.search(msg)
                if m:
                    evt = {
                        'type': 'READY_MAIN',
                        'ts': ts,
                        'lod': int(m.group(1)),
                        'x': int(m.group(2)),
                        'y': int(m.group(3))
                    }
                    events.append(evt)
                    continue
                    
                # Worker Done
                m = re_done.search(msg)
                if m:
                    evt = {
                        'type': 'DONE_WORKER',
                        'ts': ts,
                        'worker': int(m.group(1))
                    }
                    events.append(evt)
                    continue

    events.sort(key=lambda x: x['ts'])
    return events

def analyze(events):
    # Track state
    dispatched = {} # (LOD, X, Y) -> timestamp
    ready_tm = {}   # (LOD, X, Y) -> timestamp
    ready_main = {} # (LOD, X, Y) -> timestamp
    
    worker_state = {} # worker_id -> current_tile (LOD, X, Y) or None
    
    print(f"Total Events Parsed: {len(events)}")
    
    for e in events:
        if e['type'] == 'DISPATCH':
            key = (e['lod'], e['x'], e['y'])
            dispatched[key] = e['ts']
            worker_state[e['worker']] = key
            
        elif e['type'] == 'READY_TM':
            key = (e['lod'], e['x'], e['y'])
            if key not in ready_tm:
                ready_tm[key] = e['ts']
                
        elif e['type'] == 'READY_MAIN':
            key = (e['lod'], e['x'], e['y'])
            ready_main[key] = e['ts']
            
        elif e['type'] == 'DONE_WORKER':
            wid = e['worker']
            # We don't explicitly know which tile finished from "DONE Tile", 
            # but we assume it's the last one assigned to this worker.
            # However, async logic might be complex. 
            # In the log: "Worker X: DONE Tile" follows "Decode Tile" usually?
            # Actually, "DONE" usually precedes the NEXT "Decode".
            pass

    # 1. Check for Missing Tiles in the Block
    # Determine bounds for each LOD
    lods = set(k[0] for k in dispatched.keys())
    
    for lod in sorted(lods):
        keys = [k for k in dispatched.keys() if k[0] == lod]
        xs = [k[1] for k in keys]
        ys = [k[2] for k in keys]
        
        min_x, max_x = min(xs), max(xs)
        min_y, max_y = min(ys), max(ys)
        
        print(f"\n=== LOD {lod} Analysis ===")
        print(f"Bounds: X[{min_x}..{max_x}], Y[{min_y}..{max_y}]")
        print(f"Total Dispatched: {len(keys)}")
        
        # Check specific rectangular grid completeness?
        # The viewport might not be a perfect rectangle if rotated, but usually is.
        # Let's list any gaps inside the bounding box.
        
        missing_dispatch = []
        pending = []
        
        grid_count = 0
        for y in range(min_y, max_y + 1):
            for x in range(min_x, max_x + 1):
                k = (lod, x, y)
                if k not in dispatched:
                    # Is this a gap or just outside the viewport (e.g. jagged edge)?
                    # If neighbors are present, it's likely a gap.
                    has_left = (lod, x-1, y) in dispatched
                    has_right = (lod, x+1, y) in dispatched
                    has_top = (lod, x, y-1) in dispatched
                    has_bottom = (lod, x, y+1) in dispatched
                    neighbor_count = sum([has_left, has_right, has_top, has_bottom])
                    if neighbor_count >= 2:
                        missing_dispatch.append(k)
                else:
                    grid_count += 1
                    # Check if ready
                    if k not in ready_tm:
                        pending.append(k)
        
        if missing_dispatch:
            print(f"POTENTIAL GAPS (Dispatched neighbors but missing self):")
            for m in missing_dispatch:
                print(f"  - (LOD {m[0]}, X {m[1]}, Y {m[2]}) NOT DISPATCHED")
        else:
            print("No geometric gaps detected in dispatched grid.")

        if pending:
            print(f"STUCK TILES (Dispatched but not Ready): {len(pending)}")
            for p in pending:
                print(f"  - (LOD {p[0]}, X {p[1]}, Y {p[2]}) :: Dispatched at {dispatched[p]:.3f}s")
        else:
            print("All dispatched tiles became READY.")

    # 2. Check LOD 0 specifics (User complaint)
    # User says "From 50% start". 50% is likely LOD 1 or 0 transitions.
    # Log shows LOD 0 appearing at 62.6s.
    
    # Let's just list the counts.
    
if __name__ == "__main__":
    events = parse_logs()
    analyze(events)
