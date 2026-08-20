@echo off
setlocal
if "%ARKAPI_ROOT%"=="" (
  echo ERROR: set ARKAPI_ROOT to the ArkServerApi/AseApi 3.56 root first.
  exit /b 1
)
if "%PERMISSIONS_ROOT%"=="" (
  echo ERROR: set PERMISSIONS_ROOT to the ASE-Plugins\Permissions root first.
  exit /b 1
)
msbuild TurretControl.sln /m /p:Configuration=ReleasePermissions /p:Platform=x64
exit /b %ERRORLEVEL%
