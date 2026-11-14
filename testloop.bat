@echo off
rem Unattended test loop for Claude. Leave this window open.
rem   bin\rn_go.txt    -> (re)start the game (after swapping in RedNinja_next.exe if present)
rem   bin\rn_quit.txt  -> the running game exits by itself
rem   bin\rn_stop.txt  -> this loop ends
rem Close this window at any time to stop.
setlocal
cd /d "%~dp0bin"
del rn_go.txt rn_stop.txt rn_quit.txt 2>nul
echo Test loop running. Waiting for Claude...
:loop
if exist rn_stop.txt (del rn_stop.txt & echo Stopped. & exit /b 0)
if exist rn_go.txt (
  del rn_go.txt
  if exist RedNinja_next.exe (
    move /y RedNinja_next.exe RedNinja.exe >nul
    if exist RedNinja_next.pdb move /y RedNinja_next.pdb RedNinja.pdb >nul
  )
  del fb*.bmp 2>nul
  type nul > pad_live.txt
  echo [%time%] starting RedNinja.exe
  start "" /wait RedNinja.exe
  echo [%time%] game exited
  type nul > rn_done.txt
)
ping -n 2 127.0.0.1 >nul
goto loop
