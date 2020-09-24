@ECHO OFF
TASKLIST /FI "IMAGENAME eq verusd.exe" 2>NUL | find /I /N "verusd.exe">NUL
if "%ERRORLEVEL%"=="0" exit 1
SET PROCESS_NAME=Verus Bootstrap
TASKLIST /V /NH /FI "imagename eq cmd.exe"| FIND /I /C "%PROCESS_NAME%" >NUL
IF %ERRORLEVEL%==0 exit 1
TITLE %PROCESS_NAME%

SETLOCAL EnableDelayedExpansion
SET BOOTSTRAP_URL=https://bootstrap.veruscoin.io
SET TAR_FOUND=0
FOR %%x in (tar.exe) DO IF NOT [%%~$PATH:x]==[] SET TAR_FOUND=1
IF %TAR_FOUND% EQU 1 (
    SET BOOTSTRAP_PACKAGE=VRSC-bootstrap.tar.gz
) ELSE (
    SET BOOTSTRAP_PACKAGE=VRSC-bootstrap.zip
)
SET BOOTSTRAP_PACKAGE_SIG=!BOOTSTRAP_PACKAGE!.verusid

CALL :MAIN
PAUSE
EXIT 0

:MAIN
    CD !Temp!
    SET "DOWNLOAD_CMD="
    FOR %%x IN (CURL.EXE BITSADMIN.EXE) DO IF NOT [%%~$PATH:x]==[] IF NOT DEFINED DOWNLOAD_CMD SET "DOWNLOAD_CMD=FETCH_%%x"
    CALL :SET_INSTALL_DIR
    SET "USE_BOOTSTRAP=1"
    SET i=0
    IF NOT EXIST "!VRSC_DATA_DIR!" (
        ECHO No VRSC data directory found, creating directory.
        MD "!VRSC_DATA_DIR!"
    )
    FOR %%F IN (fee_estimates.dat, komodostate, komodostate.ind, peers.dat, db.log, debug.log, signedmasks) DO (
        IF  EXIST "!VRSC_DATA_DIR!\%%F" (
            ECHO Found "!VRSC_DATA_DIR!\%%F"
            SET USE_BOOTSTRAP=0
        )
    )
    FOR /D %%D IN (blocks, chainstate, database, notarisations) DO (
        IF EXIST "!VRSC_DATA_DIR!\%%D" (
            ECHO Found "!VRSC_DATA_DIR!\%%D"
            SET USE_BOOTSTRAP=0
        )
    )
    IF /I "!USE_BOOTSTRAP!" EQU "0" (
        CHOICE  /C:nyq /N /M "Existing blockchain data found. Overwrite? ([y]es/[n]o/[q]uit)"%1
        IF !ERRORLEVEL! EQU 3 EXIT 0
        SET OVERWRITE_BLOCK_DATA=!ERRORLEVEL!
        IF !ERRORLEVEL! EQU 2 (
            FOR %%F IN (fee_estimates.dat, komodostate, komodostate.ind, peers.dat, db.log, debug.log, signedmasks) DO (
                IF  EXIST "!VRSC_DATA_DIR!\%%F" (
                    ECHO Removing "!VRSC_DATA_DIR!\%%F"
                    DEL /Q/S "!VRSC_DATA_DIR!\%%F" >NUL
                )
            )
            FOR /D %%D IN (blocks, chainstate, database, notarisations) DO (
                IF EXIST "!VRSC_DATA_DIR!\%%D" (
                    ECHO Removing "!VRSC_DATA_DIR!\%%D"
                    DEL /Q/S  "!VRSC_DATA_DIR!\%%D" >NUL
                )
            )
           CALL :FETCH_BOOTSTRAP
        ) ELSE (
            ECHO Bootstrap not installed
            PAUSE
            EXIT 0
        )
     ) ELSE (
         CALL :FETCH_BOOTSTRAP
     )
GOTO :EOF

:SET_INSTALL_DIR
    SET VRSC_DATA_DIR=""
    SET /P VRSC_DATA_DIR=Enter blockchain data directory or leave blank for default:
    IF !VRSC_DATA_DIR! == "" (
        SET "VRSC_DATA_DIR=%APPDATA%\Komodo\VRSC"
    )
    CHOICE  /C:nyq /N /M "Install bootstrap in !VRSC_DATA_DIR!? ([y]es/[n]o/[q]uit)"%1
    IF !ERRORLEVEL! EQU 3 EXIT 0
    IF !ERRORLEVEL! NEQ 2 GOTO SET_INSTALL_DIR
GOTO :EOF

:FETCH_BITSADMIN.EXE
    SET "filename=%~1"
    SET "URL=%~2"
    CALL bitsadmin /transfer "Downloading %filename%" /priority FOREGROUND /download "%URL%/%filename%" "%Temp%\%filename%"
GOTO :EOF

:FETCH_CURL.EXE
    SET "filename=%~1"
    SET "URL=%~2"
    curl -# -L -C - "%URL%/%filename%" -o "%Temp%/%filename%"
GOTO :EOF

:GET_SHA256SUM
    SET "file=!%~1!"
    SET "sha256sum="
    FOR /f "skip=1 tokens=* delims=" %%# IN ('certutil -hashfile !file! SHA256') DO (
        IF NOT DEFINED sha256sum (
            FOR %%Z IN (%%#) DO SET "sha256sum=!sha256sum!%%Z"
        )
    )
    SET "%~2=!sha256sum!"
GOTO :EOF

:WRITE_BOOTSTRAP_README
   (
   ECHO !BOOTSTRAP_PACKAGE! needs be extracted directly into this directory. After extration, blocks and chainstate folders should be in this directory.
   ECHO !BOOTSTRAP_PACKAGE! can be deleted after extraction.
   )>"!VRSC_DATA_DIR!\BOOTSTRAP-README.txt"
GOTO :EOF

:FETCH_BOOTSTRAP
     ECHO Fetching VRSC bootstrap
        CALL :!DOWNLOAD_CMD! !BOOTSTRAP_PACKAGE!  !BOOTSTRAP_URL!
        CALL :!DOWNLOAD_CMD! !BOOTSTRAP_PACKAGE_SIG! !BOOTSTRAP_URL!
        ECHO Verifying download
        SET "filehash="
        CALL :GET_SHA256SUM "!Temp!\!BOOTSTRAP_PACKAGE!" filehash
        FINDSTR /m "!filehash!" "!Temp!\!BOOTSTRAP_PACKAGE_SIG!" >Nul
        IF !ERRORLEVEL! EQU 0 (
            ECHO Checksum verified!
            IF %TAR_FOUND% EQU 1  (
                ECHO Extracting Verus blockchain bootstrap
                tar -xf "!Temp!\!BOOTSTRAP_PACKAGE!" --directory "!VRSC_DATA_DIR!"
                ECHO Bootstrap successfully installed at "!VRSC_DATA_DIR!"
            ) ELSE (
                MOVE "!Temp!\!BOOTSTRAP_PACKAGE!" "!VRSC_DATA_DIR!"
                CALL :WRITE_BOOTSTRAP_README
                ECHO tar not found. Opening installation dir for manual bootstrap extraction.
                START !VRSC_DATA_DIR!
            )
        ) ELSE (
	        ECHO "!filehash!"
            ECHO Failed to verify bootstrap checksum
        )
        FOR %%F IN (!BOOTSTRAP_PACKAGE!, !BOOTSTRAP_PACKAGE_SIG!) DO (
            IF  EXIST "!Temp!\%%F" (
                DEL /Q "!Temp!\%%F"
        )
    )

GOTO :EOF

ENDLOCAL