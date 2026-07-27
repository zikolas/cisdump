@echo off
rem Build CISDUMP.EXE with Open Watcom 1.9 (16-bit, real mode, small model).
rem Expects the Watcom environment to be set, e.g.:
rem   set WATCOM=C:\WATCOM
rem   set INCLUDE=C:\WATCOM\H
rem   set PATH=C:\WATCOM\BINW;%PATH%
wcc -ms CISDUMP.C -fo=CISDUMP.obj
if errorlevel 1 goto end
wlink system dos name CISDUMP.exe file CISDUMP.obj
:end
