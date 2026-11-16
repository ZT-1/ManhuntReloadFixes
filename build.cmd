@echo off
rem Rebuilds RevolverFlickFix.asi. Needs nothing but Visual Studio.
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
   /Fo:bin\ /Fe:bin\RevolverFlickFix.asi src\dllmain.cpp /link /SUBSYSTEM:WINDOWS

if errorlevel 1 exit /b 1
echo.
echo Built bin\RevolverFlickFix.asi
