@echo off
rem Rebuilds ReloadFixes.asi and copies ReloadFixes.ini next to it. Needs nothing but Visual Studio.
setlocal
cd /d "%~dp0"

set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat
if not exist "%VCVARS%" (
	echo Could not find vcvars32.bat - edit the VCVARS line in this file.
	exit /b 1
)

call "%VCVARS%" >nul
if not exist bin mkdir bin

cl /nologo /LD /O2 /MT /GS- /W3 /EHsc /D_CRT_SECURE_NO_WARNINGS ^
   /Fo:bin\ /Fe:bin\ReloadFixes.asi src\dllmain.cpp /link /SUBSYSTEM:WINDOWS

if errorlevel 1 exit /b 1
copy /y ReloadFixes.ini bin\ReloadFixes.ini >nul
echo.
echo Built bin\ReloadFixes.asi
