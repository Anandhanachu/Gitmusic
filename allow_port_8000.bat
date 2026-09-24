@echo off
echo ============================================
echo   GitMusic - Firewall Setup
echo ============================================
echo.
echo Adding firewall rule to allow port 8000...
netsh advfirewall firewall add rule name="GitMusic Port 8000" dir=in action=allow protocol=TCP localport=8000 profile=any
if %errorlevel% == 0 (
    echo.
    echo [OK] Firewall rule added successfully!
    echo      ESP32 can now connect to the backend.
) else (
    echo.
    echo [ERROR] Failed to add rule. Please check administrator privileges.
)
echo.
pause
