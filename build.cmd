@echo off
rem One-command build. See tools\build.py for the options (debug, release, tests, -j N, --clean).
python "%~dp0tools\build.py" %*
exit /b %ERRORLEVEL%
