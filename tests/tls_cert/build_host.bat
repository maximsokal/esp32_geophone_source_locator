@echo off
rem Builds the host driver for web_tls_cert.c against ESP-IDF's own mbedtls. usage: build_host.bat <idf mbedtls dir> <out dir>
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set MB=%~1
set OUT=%~2
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%OUT%"
if not exist mbedtls.lib (
  cl /nologo /c /O1 /W0 /I"%MB%\include" /I"%MB%\library" "%MB%\library\*.c" || exit /b 1
  lib /nologo /out:mbedtls.lib *.obj || exit /b 1
  del /q *.obj
)
cl /nologo /W3 /I"%MB%\include" /I"%~dp0..\..\components\web_server" "%~dp0web_tls_cert_issue_main.c" "%~dp0..\..\components\web_server\web_tls_cert.c" mbedtls.lib advapi32.lib bcrypt.lib /Fe:web_tls_cert_issue.exe || exit /b 1
