@echo off
rem Builds Tracy's viewer (tracy-profiler), capture tool (tracy-capture) and zone statistics exporter (tracy-csvexport) from the Tracy sources xprofile uses - the same version as
rem the client, which is what a viewer must match. Their CMake downloads their own libraries (CPM: glfw, capstone, freetype, ...) into
rem a cache next to the output, once. Ninja (as Tracy's own builds): with the Visual Studio generator the viewer's "embed" step runs
rem where its helper is not found. NO_CCACHE: a ccache found on the PATH (Strawberry Perl ships one) breaks on paths with spaces.
rem   BuildTracyTools.bat <tracy source folder> <output folder>
rem The executables: <output>\profiler\tracy-profiler.exe, <output>\capture\tracy-capture.exe, <output>\csvexport\tracy-csvexport.exe
setlocal
if "%~2"=="" ( echo usage: BuildTracyTools.bat ^<tracy source folder^> ^<output folder^> & exit /b 1 )
set "TRACY=%~f1"
set "OUT=%~f2"
set "CPM_SOURCE_CACHE=%OUT%\cpm"
rem (no Visual Studio environment yet: the newest one's; a label, not a block - the ")" of "Program Files (x86)" would close it)
where cl >nul 2>nul
if not errorlevel 1 goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%I in (`call "%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%I"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:build
for %%T in (capture csvexport profiler) do (
    cmake -S "%TRACY%\%%T" -B "%OUT%\%%T" -G Ninja -DCMAKE_BUILD_TYPE=Release -DNO_CCACHE=ON || exit /b 1
    cmake --build "%OUT%\%%T" || exit /b 1
)
echo Tracy tools built in %OUT%
