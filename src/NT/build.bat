@echo off
REM mmcsdnt.sys for NT 3.1/3.50/3.51/4.0. MSVCDIR = Visual C++ 4.0, DDKDIR = NT4 DDK

set MSVCDIR=C:\MSDEV
set DDKDIR=C:\NT4DDK

set PATH=%MSVCDIR%\BIN;%DDKDIR%\BIN;%PATH%
set INCLUDE=%DDKDIR%\inc;%DDKDIR%\src\storage\inc;%DDKDIR%\inc\crt;%MSVCDIR%\INCLUDE;C:\HYBRIDST\inc
set LIB=%DDKDIR%\lib\i386;%MSVCDIR%\LIB;%DDKDIR%\lib\i386\free;%LIB%

if exist mmcmain.obj del mmcmain.obj
if exist mmccmd.obj del mmccmd.obj
if exist mmcsatl.obj del mmcsatl.obj
if exist mmcacpi.obj del mmcacpi.obj
if exist mmcsdnt.sys del mmcsdnt.sys

cl -nologo -c -Gz -Ox -W3 -Zp8 -Zi -DMMC_NT4 -D_X86_=1 -Di386=1 -DCONDITION_HANDLING=1 -DNT_UP=1 -DNT_INST=0 -DWIN32=100 -D_NT1X_=100 -DWINNT=1 -D_WIN32_WINNT=0x0350 /I.. /I..\inc ..\mmcmain.c ..\mmccmd.c ..\mmcsatl.c ..\mmcacpi.c
if errorlevel 1 goto error

rc -r -fommcsdnt.res ..\mmcsdnt.rc
if errorlevel 1 goto error

link -nologo -debug -debugtype:both -subsystem:native,3.50 -entry:DriverEntry@8 -driver -base:0x10000 -align:0x200 -out:mmcsdnt.sys mmcmain.obj mmccmd.obj mmcsatl.obj mmcacpi.obj scsiport.lib ntoskrnl.lib mmcsdnt.res
if errorlevel 1 goto error

echo mmcsdnt.sys built.
goto end

:error
echo Build FAILED.

:end
