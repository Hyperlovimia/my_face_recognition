@echo off
setlocal
cd /d "%~dp0web"
if not exist "package.json" (
  echo ERROR: 未找到 web\package.json
  exit /b 1
)
call npm install
if errorlevel 1 exit /b 1
call npm run build
if errorlevel 1 exit /b 1
echo.
echo 前端已构建到 server_pc\static\，可执行: docker compose build
endlocal
