# AGENTS.md - DevReciproc (DLG4000 + DriveA5)

Purpose
This file is only for Codex. Keep it short, actionable, ASCII-only, and optimized for editing and safety.

Project goal
Build a reciprocating tribometer with two C executables:
1) DLG4000 data acquisition (UDP).
2) DriveA5 motor control + position logging (Modbus RTU).
Python UI is only an orchestrator/visualizer.
Current status: DLG4000 and DriveA5 projects are working and should be treated as reference implementations.

Architecture summary
- Core in C for timing control and acquisition. UI in Python only for control and plotting.
- IPC: JSON lines over STDIN/STDOUT (simple, versioned).
- Data: CSV with fixed columns and clear units (auto-described header when possible).

Key decisions (do not regress)
- Position per revolution uses P0B-09 (0..65535). Drop old heuristics.
- DLG timing uses QPC and starts only after first OP_ACQDATA to avoid startup lag.
- Explicit loss handling: inject "NULL" rows for timeouts/gaps and report counters.
- Separate timeouts: command ops are lenient, sampling is short.
- Logs are ASCII-only.

Module notes
DLG4000 (UDP/WinSock2):
- Pipeline: ACQSTOP (preventive) -> setup -> ACQSTART -> receive -> ACQSTOP.
- Accept older ACQSETUP_R (short response).
- CSV: include elapsed time, frame, raw channels; inject NULL on losses.
- Channel config uses index values from the manual: iGain index 0..7 => [1,3,10,30,100,300,1000,3000].
- Excitation uses iSensPwr index 0..4 => [1V, 2.5V, 3.3V, 5V, user].
- ACQDATA samples are interleaved by burst; total samples = nSignals * nBurstsPack.
- CalibraDLG capture uses short recv timeout and can reissue ACQSETUP/START if no data arrives.
- CalibraDLG re-syncs the stream at each calibration point (drains socket and restarts ACQ).
- CalibraDLG stops/starts acquisition per point to avoid stream stalls between user inputs.
- Bug history: calibration stalled after point 1/2 because wait_first_packet always re-sent ACQSETUP with channel 1, and the stream could stall during long user input; fixed by using the selected channel and restarting ACQ per point with socket drain.

DriveA5 (Modbus RTU / libmodbus):
- RUN/STOP, set RPM, read P0B-09. Try FC03, fallback FC04.
- Standard test: 10 rpm, 120 s, ~200 Hz. CSV: t_s,pos,rev.
- Revolution count: detect robust wrap (prev > 60000 and pos < 5000).

Operational constraints
- Windows only (VS2022 + CMake). Use PowerShell at repo root.
- All executables must run on other Windows machines (self-contained distribution even if larger).
- DLG IP: 192.168.1.100, UDP port 41401. Open firewall IN/OUT.
- Do not change protocol structs or packing without validating offsets/endianness.
- If operating hardware: confirm IP/port and do not move actuators without explicit approval.

CSV conventions
- ASCII logs and CSV only.
- Fixed headers and units. Use "NULL" rows only for real losses.
- Write outputs into out/ subfolders (gitignored) when creating artifacts.

Roadmap (short)
- Load calibration (a,b) from file and apply on-the-fly in DLG logger.
- Formalize JSON protocol v1 (required/optional fields, errors).
- Unify both executables into one core with threads for motor/DLG/IPC.
- Parametrization via config file (duration, rate, channels, serial port, output dir).
- Add bench tests with simulated DLG and synthetic drive position.

Agent behavior
- Read the repo and produce a plan before editing, unless trivial.
- Propose focused diffs; avoid broad reformatting.
- If unexpected changes appear that you did not make, stop and ask.
- Keep AGENTS.md updated when protocol, calibration, or behavior changes are introduced.

Multi-exe orchestration
- Expect multiple executables (one per function). A supervisor program will orchestrate them.
- Executables should exchange data via files: e.g., calibration tool writes a file that the main DLG logger reads.
