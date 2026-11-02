@echo off
rem Test CYD bez płytek C5: udaje oba czujniki (drony 100 m, 500 m i 3 km od rynku w Nowym Wiśniczu).
rem Przed uruchomieniem zamknij agenta RID Decoder - port CYD może mieć otwarty tylko jeden program.
chcp 65001 >nul
cd /d "%~dp0"
python -c "import serial" 2>nul || python -m pip install pyserial
echo Test czujnikow C5 na CYD - zamknij agenta RID Decoder. Ctrl+C konczy test.
python c5_sim.py %*
pause
