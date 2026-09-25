@echo off
REM ============================================================================
REM  Wireshark extcap wrapper (Windows only)
REM
REM  On Windows, Wireshark only runs executables (.exe / .bat) from its extcap
REM  folder -- it will not run a .py script directly. Put this .bat next to
REM  blehound_extcap.py in the Wireshark extcap folder; Wireshark then calls
REM  this .bat, which launches the Python plugin.
REM
REM  Requires: Python 3 on PATH with pyserial installed (pip install pyserial).
REM  If `python` is not on PATH, replace it below with `py -3`.
REM ============================================================================
python "%~dp0blehound_extcap.py" %*
