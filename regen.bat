@echo off
rem Re-run the xboxrecomp pipeline: bin\game\default.xbe -> src\recomp\gen\*.c
rem Needs Python 3.10+ with capstone (pip install capstone).
setlocal
set "PROJ=%~dp0"
set "XBE=%PROJ%bin\game\default.xbe"
set "A=%PROJ%analysis"
if not defined XBOXRECOMP_DIR set "XBOXRECOMP_DIR=%PROJ%..\..\xboxrecomp-0.17.0"
if not exist "%XBE%" (echo Run the game once first so it unpacks your disc image into bin\game. & exit /b 1)
cd /d "%XBOXRECOMP_DIR%" || exit /b 1
py -3 -m tools.xbe_parser "%XBE%" --json "%PROJ%bin\game\redninja_analysis.json" > "%A%\xbe_report.txt" || exit /b 1
py -3 -m tools.disasm "%XBE%" -o "%A%\disasm" || exit /b 1
py -3 -m tools.func_id "%XBE%" --functions "%A%\disasm\functions.json" --strings "%A%\disasm\strings.json" --xrefs "%A%\disasm\xrefs.json" -o "%A%\func_id" || exit /b 1
py -3 -m tools.abi_analysis "%XBE%" --disasm-dir "%A%\disasm" --func-id-dir "%A%\func_id" --output-dir "%A%\abi" || exit /b 1
py -3 -m tools.recomp "%XBE%" --game-name "Red Ninja: End of Honor" --all --split 250 --gen-dir "%PROJ%src\recomp\gen" --disasm-dir "%A%\disasm" --func-id-dir "%A%\func_id" --abi-dir "%A%\abi" --exclude-manual "%PROJ%src\recomp_manual.c" || exit /b 1
py -3 "%PROJ%tools\postprocess_gen.py" "%PROJ%src\recomp\gen" || exit /b 1
echo Done. Now run build.bat
