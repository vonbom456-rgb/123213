@echo off
setlocal
if "%ARKAPI_ROOT%"=="" (
  echo ERROR: set ARKAPI_ROOT to the ArkServerApi/AseApi 3.56 root first.
  exit /b 1
)
msbuild TurretControl.sln /m /p:Configuration=Release /p:Platform=x64
exit /b %ERRORLEVEL%
