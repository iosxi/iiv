@echo off
rem Build both exes: server\iiv-server.exe and client\iiv-client.exe.
rem   build.bat          release builds
rem   build.bat debug    debug builds (in server\build and client\build)
rem Each side can also be built alone with its own build.bat.
rem
rem ASCII only and CRLF on purpose (see server\build.bat).
setlocal
call "%~dp0server\build.bat" %1 || exit /b 1
call "%~dp0client\build.bat" %1 || exit /b 1
exit /b 0
