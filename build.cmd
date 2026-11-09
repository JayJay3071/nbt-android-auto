@echo off
setlocal
rem Allow locally authored scripts only in this child process; do not change saved policy.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy RemoteSigned -File "%~dp0scripts\Invoke-QnxBuild.ps1" -Action Build
exit /b %errorlevel%